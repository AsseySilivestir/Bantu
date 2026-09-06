#pragma once
/**
 * Bantu v1.2.1 — PostgreSQL Driver (libpq)
 *
 * Real PostgreSQL connectivity for Bantu. When Bantu is compiled with
 * libpq available (`-DHAS_LIBPQ` and `libpq-fe.h` on the include path),
 * this driver is used in place of the in-process stub and `sua.postgres.*`
 * calls route to a live `PGconn*`.
 *
 * Build (Linux):
 *   sudo apt install libpq-dev
 *   cmake .. -DBANTU_POSTGRES=ON
 *
 * Build (Windows / vcpkg):
 *   vcpkg install libpq
 *   cmake .. -DBANTU_POSTGRES=ON -DCMAKE_TOOLCHAIN_FILE=...
 *
 * Connection string format (standard libpq):
 *   "host=localhost port=5432 dbname=app user=postgres password=secret"
 *
 * URL form is also accepted:
 *   "postgresql://user:secret@localhost:5432/app"
 */

#ifdef HAS_LIBPQ
#include <libpq-fe.h>
#endif

#include <string>
#include <vector>
#include <iostream>

namespace bantu {

class PostgresDriver {
public:
#ifdef HAS_LIBPQ
    PostgresDriver() = default;
    ~PostgresDriver() { close(); }

    bool connect(const std::string& connStr) {
        conn_ = PQconnectdb(connStr.c_str());
        if (!conn_ || PQstatus(conn_) != CONNECTION_OK) {
            std::cerr << "  [POSTGRES] libpq connect failed: "
                      << (conn_ ? PQerrorMessage(conn_) : "null conn") << "\n";
            if (conn_) { PQfinish(conn_); conn_ = nullptr; }
            return false;
        }
        std::cout << "  [POSTGRES] libpq connected\n";
        return true;
    }

    // Returns rows as a vector of dicts (column name -> string value).
    std::vector<std::unordered_map<std::string, std::string>> query(const std::string& sql) {
        std::vector<std::unordered_map<std::string, std::string>> rows;
        if (!conn_) return rows;
        PGresult* res = PQexec(conn_, sql.c_str());
        if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
            std::cerr << "  [POSTGRES] query failed: "
                      << (res ? PQresultErrorMessage(res) : "null") << "\n";
            if (res) PQclear(res);
            return rows;
        }
        int nRows = PQntuples(res);
        int nCols = PQnfields(res);
        for (int r = 0; r < nRows; ++r) {
            std::unordered_map<std::string, std::string> row;
            for (int c = 0; c < nCols; ++c) {
                row[PQfname(res, c)] = PQgetvalue(res, r, c);
            }
            rows.push_back(std::move(row));
        }
        PQclear(res);
        return rows;
    }

    int exec(const std::string& sql) {
        if (!conn_) return -1;
        PGresult* res = PQexec(conn_, sql.c_str());
        int rc = (res && PQresultStatus(res) == PGRES_COMMAND_OK) ? 0 : -1;
        if (res) PQclear(res);
        return rc;
    }

    void close() {
        if (conn_) { PQfinish(conn_); conn_ = nullptr; }
    }

    bool isConnected() const { return conn_ && PQstatus(conn_) == CONNECTION_OK; }

private:
    PGconn* conn_ = nullptr;
#else
    // Stub mode (default build): no-op driver. The evaluator's built-in
    // simulation handles `sua.postgres.*` calls in this case.
    bool connect(const std::string&) { return false; }
    std::vector<std::unordered_map<std::string, std::string>> query(const std::string&) { return {}; }
    int exec(const std::string&) { return -1; }
    void close() {}
    bool isConnected() const { return false; }
#endif
};

} // namespace bantu
