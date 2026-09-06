#pragma once
// ════════════════════════════════════════════════════════════════════════════
//  dataframe_arrow.hpp — Parquet + Feather/Arrow-IPC I/O for arctic.
//
//  OPT-IN ONLY. This whole file compiles to nothing unless the interpreter is
//  built with -DBANTU_ARROW (and libarrow/libparquet linked). The default build
//  gains NO new dependency — same discipline as the libsodium (BANTU_SODIUM)
//  crypto module. Feature-detected at runtime via has_native("arrow").
//
//  WHY NATIVE: Parquet needs Thrift metadata + several encodings + compression
//  codecs, and Arrow IPC is a flatbuffers binary layout — neither is sensible to
//  implement in an interpreted language. Apache Arrow already does it well, and
//  arctic's Column layout (typed contiguous buffer + null mask) maps almost 1:1
//  onto arrow::Array, so the conversion is cheap.
//
//  Mapping (arctic dtype/overlay  <->  arrow type):
//    f64  <-> float64 · i64 <-> int64 · bool <-> boolean · utf8 <-> utf8/large_utf8
//    datetime <-> timestamp[ms, UTC] · date <-> date32[days]
//    categorical: written as its category strings (utf8); read back as utf8
//    (call col_to_categorical to re-encode). Dictionary arrays read as utf8 too.
//  Every column carries its null bitmap both ways.
// ════════════════════════════════════════════════════════════════════════════

#ifdef BANTU_ARROW

#include "dataframe_native.hpp"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <arrow/ipc/feather.h>
#include <arrow/compute/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace arctic {
namespace arrowio {

inline void checkOk(const arrow::Status& s, const char* what) {
    if (!s.ok()) throw std::runtime_error(std::string(what) + ": " + s.ToString());
}
template <class T>
inline T valueOrThrow(arrow::Result<T> r, const char* what) {
    if (!r.ok()) throw std::runtime_error(std::string(what) + ": " + r.status().ToString());
    return std::move(r).ValueOrDie();
}

// ── Column → arrow::Array ─────────────────────────────────────────────────────
inline std::shared_ptr<arrow::Array> columnToArrow(const Column& c) {
    auto* pool = arrow::default_memory_pool();
    std::shared_ptr<arrow::Array> out;

    if (c.logical == Logical::DATETIME) {
        arrow::TimestampBuilder b(arrow::timestamp(arrow::TimeUnit::MILLI, "UTC"), pool);
        checkOk(b.Reserve(c.n), "reserve");
        for (size_t i = 0; i < c.n; i++) c.valid[i] ? checkOk(b.Append(c.i64[i]), "append") : checkOk(b.AppendNull(), "appendnull");
        checkOk(b.Finish(&out), "finish"); return out;
    }
    if (c.logical == Logical::DATE) {
        arrow::Date32Builder b(pool);
        checkOk(b.Reserve(c.n), "reserve");
        for (size_t i = 0; i < c.n; i++) c.valid[i] ? checkOk(b.Append((int32_t)c.i64[i]), "append") : checkOk(b.AppendNull(), "appendnull");
        checkOk(b.Finish(&out), "finish"); return out;
    }
    if (c.logical == Logical::CAT) {   // materialize category text → utf8
        ColumnPtr u = catToUtf8(c);
        return columnToArrow(*u);
    }
    switch (c.dtype) {
        case DType::F64: {
            arrow::DoubleBuilder b(pool); checkOk(b.Reserve(c.n), "reserve");
            for (size_t i = 0; i < c.n; i++) c.valid[i] ? checkOk(b.Append(c.f64[i]), "append") : checkOk(b.AppendNull(), "n");
            checkOk(b.Finish(&out), "finish"); return out;
        }
        case DType::I64: {
            arrow::Int64Builder b(pool); checkOk(b.Reserve(c.n), "reserve");
            for (size_t i = 0; i < c.n; i++) c.valid[i] ? checkOk(b.Append(c.i64[i]), "append") : checkOk(b.AppendNull(), "n");
            checkOk(b.Finish(&out), "finish"); return out;
        }
        case DType::BOOL: {
            arrow::BooleanBuilder b(pool); checkOk(b.Reserve(c.n), "reserve");
            for (size_t i = 0; i < c.n; i++) c.valid[i] ? checkOk(b.Append(c.b[i] != 0), "append") : checkOk(b.AppendNull(), "n");
            checkOk(b.Finish(&out), "finish"); return out;
        }
        case DType::UTF8: {
            arrow::StringBuilder b(pool);
            for (size_t i = 0; i < c.n; i++) c.valid[i] ? checkOk(b.Append(c.s[i]), "append") : checkOk(b.AppendNull(), "n");
            checkOk(b.Finish(&out), "finish"); return out;
        }
    }
    throw std::runtime_error("columnToArrow: unhandled dtype");
}

// ── arrow::ChunkedArray → Column ─────────────────────────────────────────────
inline ColumnPtr arrowToColumn(const std::shared_ptr<arrow::ChunkedArray>& ca) {
    auto tid = ca->type()->id();
    auto c = std::make_shared<Column>();
    c->n = (size_t)ca->length();
    c->valid.assign(c->n, 1);

    // choose our storage + logical overlay from the arrow type
    int64_t tsScaleToMsNum = 1, tsScaleToMsDen = 1;  // for timestamps
    if (tid == arrow::Type::DOUBLE || tid == arrow::Type::FLOAT) { c->dtype = DType::F64; c->f64.resize(c->n); }
    else if (tid == arrow::Type::INT64 || tid == arrow::Type::INT32 || tid == arrow::Type::INT16 || tid == arrow::Type::INT8
          || tid == arrow::Type::UINT64 || tid == arrow::Type::UINT32 || tid == arrow::Type::UINT16 || tid == arrow::Type::UINT8) {
        c->dtype = DType::I64; c->i64.resize(c->n);
    }
    else if (tid == arrow::Type::BOOL) { c->dtype = DType::BOOL; c->b.resize(c->n); }
    else if (tid == arrow::Type::TIMESTAMP) {
        c->dtype = DType::I64; c->i64.resize(c->n); c->logical = Logical::DATETIME;
        auto ts = std::static_pointer_cast<arrow::TimestampType>(ca->type());
        switch (ts->unit()) {
            case arrow::TimeUnit::SECOND: tsScaleToMsNum = 1000; break;
            case arrow::TimeUnit::MILLI:  break;
            case arrow::TimeUnit::MICRO:  tsScaleToMsDen = 1000; break;
            case arrow::TimeUnit::NANO:   tsScaleToMsDen = 1000000; break;
        }
    }
    else if (tid == arrow::Type::DATE32) { c->dtype = DType::I64; c->i64.resize(c->n); c->logical = Logical::DATE; }
    else if (tid == arrow::Type::DATE64) { c->dtype = DType::I64; c->i64.resize(c->n); c->logical = Logical::DATETIME; }
    else { c->dtype = DType::UTF8; c->s.resize(c->n); }   // strings, dictionaries, everything else → text

    size_t row = 0;
    for (int ch = 0; ch < ca->num_chunks(); ch++) {
        std::shared_ptr<arrow::Array> arr = ca->chunk(ch);
        // Normalize the chunk ONCE (whole-array casts, never per element):
        //  • dictionary → decode to values · integers → int64 · float → double.
        if (tid == arrow::Type::DICTIONARY)
            arr = valueOrThrow(arrow::compute::CallFunction("dictionary_decode", {arr}), "dictionary_decode").make_array();
        if (c->dtype == DType::I64 && c->logical == Logical::NONE
            && tid != arrow::Type::DATE64 && arr->type_id() != arrow::Type::INT64)
            arr = valueOrThrow(arrow::compute::Cast(*arr, arrow::int64()), "cast");
        if (c->dtype == DType::F64 && arr->type_id() != arrow::Type::DOUBLE)
            arr = valueOrThrow(arrow::compute::Cast(*arr, arrow::float64()), "cast");

        int64_t len = arr->length();
        for (int64_t i = 0; i < len; i++, row++) {
            if (arr->IsNull(i)) { c->valid[row] = 0; continue; }
            switch (c->dtype) {
                case DType::F64: c->f64[row] = std::static_pointer_cast<arrow::DoubleArray>(arr)->Value(i); break;
                case DType::I64: {
                    if (c->logical == Logical::DATETIME) {
                        if (tid == arrow::Type::DATE64) {
                            c->i64[row] = std::static_pointer_cast<arrow::Date64Array>(arr)->Value(i);   // already ms
                        } else {
                            int64_t v = std::static_pointer_cast<arrow::TimestampArray>(arr)->Value(i);
                            c->i64[row] = v * tsScaleToMsNum / tsScaleToMsDen;
                        }
                    } else if (c->logical == Logical::DATE) {
                        c->i64[row] = std::static_pointer_cast<arrow::Date32Array>(arr)->Value(i);
                    } else if (tid == arrow::Type::DATE64) {
                        c->i64[row] = std::static_pointer_cast<arrow::Date64Array>(arr)->Value(i);
                    } else {
                        c->i64[row] = std::static_pointer_cast<arrow::Int64Array>(arr)->Value(i);
                    }
                    break;
                }
                case DType::BOOL: c->b[row] = std::static_pointer_cast<arrow::BooleanArray>(arr)->Value(i) ? 1 : 0; break;
                case DType::UTF8: {
                    if (arr->type_id() == arrow::Type::LARGE_STRING) c->s[row] = std::static_pointer_cast<arrow::LargeStringArray>(arr)->GetString(i);
                    else if (arr->type_id() == arrow::Type::STRING)   c->s[row] = std::static_pointer_cast<arrow::StringArray>(arr)->GetString(i);
                    else c->s[row] = arr->ToString();   // last-resort text
                    break;
                }
            }
        }
    }
    return c;
}

// ── Frame <-> Table ──────────────────────────────────────────────────────────
inline std::shared_ptr<arrow::Table> frameToTable(const std::vector<std::string>& names,
                                                  const std::vector<ColumnPtr>& cols) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    for (size_t j = 0; j < names.size(); j++) {
        auto a = columnToArrow(*cols[j]);
        fields.push_back(arrow::field(names[j], a->type()));
        arrays.push_back(a);
    }
    return arrow::Table::Make(arrow::schema(fields), arrays);
}

inline Value tableToFrame(const std::shared_ptr<arrow::Table>& t) {
    ObjectMap out; std::vector<Value> names; ObjectMap cols;
    for (int j = 0; j < t->num_columns(); j++) {
        std::string nm = t->schema()->field(j)->name();
        names.push_back(Value(nm));
        cols[nm] = wrap(arrowToColumn(t->column(j)));
    }
    size_t nrows = (size_t)t->num_rows();
    out["names"] = Value(std::move(names));
    out["cols"] = Value(std::move(cols));
    out["shape"] = Value(std::vector<Value>{ Value((double)nrows), Value((double)t->num_columns()) });
    return Value(std::move(out));
}

// Map requested column names to indices in a schema (order = as requested).
inline std::vector<int> columnIndices(const std::shared_ptr<arrow::Schema>& schema,
                                      const std::vector<std::string>& want) {
    std::unordered_map<std::string,int> idx;
    for (int i = 0; i < schema->num_fields(); i++) idx[schema->field(i)->name()] = i;
    std::vector<int> out;
    for (auto& nm : want) { auto it = idx.find(nm); if (it != idx.end()) out.push_back(it->second); }
    return out;
}

// ── Parquet ──────────────────────────────────────────────────────────────────
inline void writeParquet(const std::vector<std::string>& names, const std::vector<ColumnPtr>& cols,
                         const std::string& path, const std::string& compression) {
    auto table = frameToTable(names, cols);
    auto outfile = valueOrThrow(arrow::io::FileOutputStream::Open(path), "open output");
    auto codec = arrow::Compression::SNAPPY;
    if (compression == "zstd") codec = arrow::Compression::ZSTD;
    else if (compression == "gzip") codec = arrow::Compression::GZIP;
    else if (compression == "none" || compression == "uncompressed") codec = arrow::Compression::UNCOMPRESSED;
    auto props = parquet::WriterProperties::Builder().compression(codec)->build();
    checkOk(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), outfile,
                                       /*chunk_size*/ 1 << 20, props), "write parquet");
    checkOk(outfile->Close(), "close");
}

inline Value readParquet(const std::string& path, const std::vector<std::string>& usecols) {
    auto infile = valueOrThrow(arrow::io::ReadableFile::Open(path), "open parquet");
    std::unique_ptr<parquet::arrow::FileReader> reader;
    parquet::arrow::FileReaderBuilder builder;
    checkOk(builder.Open(infile), "open parquet");
    checkOk(builder.memory_pool(arrow::default_memory_pool())->Build(&reader), "build parquet reader");
    std::shared_ptr<arrow::Table> table;
    if (usecols.empty()) {
        checkOk(reader->ReadTable(&table), "read table");
    } else {
        std::shared_ptr<arrow::Schema> schema;
        checkOk(reader->GetSchema(&schema), "schema");
        std::vector<int> indices = columnIndices(schema, usecols);
        checkOk(reader->ReadTable(indices, &table), "read table (projected)");
    }
    return tableToFrame(table);
}

// ── Feather / Arrow IPC ──────────────────────────────────────────────────────
inline void writeFeather(const std::vector<std::string>& names, const std::vector<ColumnPtr>& cols,
                         const std::string& path) {
    auto table = frameToTable(names, cols);
    auto outfile = valueOrThrow(arrow::io::FileOutputStream::Open(path), "open output");
    checkOk(arrow::ipc::feather::WriteTable(*table, outfile.get()), "write feather");
    checkOk(outfile->Close(), "close");
}

inline Value readFeather(const std::string& path, const std::vector<std::string>& usecols) {
    auto infile = valueOrThrow(arrow::io::ReadableFile::Open(path), "open feather");
    auto reader = valueOrThrow(arrow::ipc::feather::Reader::Open(infile), "open feather reader");
    std::shared_ptr<arrow::Table> table;
    if (usecols.empty()) {
        checkOk(reader->Read(&table), "read feather");
    } else {
        checkOk(reader->Read(usecols, &table), "read feather (projected)");
    }
    return tableToFrame(table);
}

} // namespace arrowio
} // namespace arctic

#endif // BANTU_ARROW
