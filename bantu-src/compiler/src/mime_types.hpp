#pragma once
// ════════════════════════════════════════════════════════════════════════════
//  mime_types.hpp — file extension → Content-Type, for sua's static file server.
//
//  Kept in its own header rather than inline in the server loop so the table is
//  one obvious place to extend, and so it is not owned by any one feature. The
//  original inline table covered nine extensions and was missing `.webmanifest`
//  (which a web app manifest served as a file requires), every font format,
//  WebP/AVIF, WASM and all media types — everything else fell through to
//  `application/octet-stream`, which browsers refuse to execute or render.
//
//  Text formats carry `charset=utf-8`; binary formats deliberately do not.
// ════════════════════════════════════════════════════════════════════════════

#include <cctype>
#include <string>

namespace bantu_mime {

// `ext` must be lowercase and without the dot. Use for_path() unless you have
// already normalised it.
inline std::string for_extension(const std::string& ext) {
    // ── markup / text ──
    if (ext == "html" || ext == "htm")   return "text/html; charset=utf-8";
    if (ext == "css")                    return "text/css; charset=utf-8";
    if (ext == "txt")                    return "text/plain; charset=utf-8";
    if (ext == "csv")                    return "text/csv; charset=utf-8";
    if (ext == "md")                     return "text/markdown; charset=utf-8";
    if (ext == "xml")                    return "application/xml; charset=utf-8";

    // ── scripts / data ──
    if (ext == "js" || ext == "mjs")     return "application/javascript; charset=utf-8";
    if (ext == "json" || ext == "map")   return "application/json; charset=utf-8";
    // A web app manifest. Browsers accept application/json too, but this is the
    // registered type and Lighthouse checks for it.
    if (ext == "webmanifest")            return "application/manifest+json; charset=utf-8";
    if (ext == "wasm")                   return "application/wasm";
    if (ext == "pdf")                    return "application/pdf";

    // ── images ──
    if (ext == "svg")                    return "image/svg+xml";
    if (ext == "png")                    return "image/png";
    if (ext == "jpg" || ext == "jpeg")   return "image/jpeg";
    if (ext == "gif")                    return "image/gif";
    if (ext == "webp")                   return "image/webp";
    if (ext == "avif")                   return "image/avif";
    if (ext == "bmp")                    return "image/bmp";
    if (ext == "ico")                    return "image/x-icon";

    // ── fonts ── (a wrong type here makes browsers reject the font outright)
    if (ext == "woff")                   return "font/woff";
    if (ext == "woff2")                  return "font/woff2";
    if (ext == "ttf")                    return "font/ttf";
    if (ext == "otf")                    return "font/otf";
    if (ext == "eot")                    return "application/vnd.ms-fontobject";

    // ── media ──
    if (ext == "mp4")                    return "video/mp4";
    if (ext == "webm")                   return "video/webm";
    if (ext == "ogv")                    return "video/ogg";
    if (ext == "mp3")                    return "audio/mpeg";
    if (ext == "ogg" || ext == "oga")    return "audio/ogg";
    if (ext == "wav")                    return "audio/wav";
    if (ext == "m4a")                    return "audio/mp4";

    return "application/octet-stream";
}

// Extract the extension from a path and resolve it. Handles uppercase
// extensions, and a dot that belongs to a directory rather than the filename
// (e.g. "./public/README" must not be read as extension "/public/README").
inline std::string for_path(const std::string& path) {
    size_t dot   = path.find_last_of('.');
    size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return "application/octet-stream";           // no extension
    std::string ext = path.substr(dot + 1);
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    return for_extension(ext);
}

// True for types where a stale copy actively breaks the app: an HTML shell that
// pins old asset URLs, or a manifest that pins an old start_url or icon set.
inline bool should_revalidate(const std::string& path) {
    std::string ct = for_path(path);
    return ct.rfind("text/html", 0) == 0
        || ct.rfind("application/manifest+json", 0) == 0;
}

} // namespace bantu_mime
