// ════════════════════════════════════════════════════════════════════
//  package_manager.hpp — PATH integration + offline package manager
//
//  Adds Spring-Initializer-style ergonomics to the Bantu CLI:
//
//    bantu setup                  Add bantu to PATH (user install)
//    bantu setup --system         Add bantu to PATH (system install, sudo)
//    bantu uninstall              Remove bantu from PATH
//    bantu doctor                 Diagnose install + registry state
//
//    bantu install                 Install all deps from bantu.json
//    bantu add <pkg>               Add a dep + install it
//    bantu add <pkg>@<ver>         Add a specific version
//    bantu remove <pkg>            Remove a dep + uninstall it
//    bantu update                  Update all deps to latest
//    bantu update <pkg>            Update one dep to latest
//    bantu list                    List installed packages in project
//    bantu search <term>           Search local registry (offline)
//    bantu publish <dir>           Add a directory to local registry
//    bantu publish <dir> --as <n>  Publish under a different name
//
//  Design:
//    • Local-only registry at ~/.bantu/registry/<name>/<version>/
//      Works 100% offline — no internet needed.
//    • Each package is a folder with package.json + .b source files.
//    • Installed packages land in ./bantu_modules/<name>/ (like node_modules).
//    • Manifest bantu.json gains a "dependencies" object: { "name": "ver" }.
//
//  Cross-platform:
//    • Linux/macOS: ~/.local/bin/, ~/.bantu/registry/
//    • Windows:     %USERPROFILE%/.bantu/bin/, %USERPROFILE%/.bantu/registry/
// ════════════════════════════════════════════════════════════════════
#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdlib>

// ─── Platform-specific headers ────────────────────────────────────────
#ifdef _WIN32
    #include <direct.h>          // _getcwd, _mkdir, _rmdir
    #include <windows.h>         // GetModuleFileNameA, FindFirstFile
    #define getcwd _getcwd
    #define rmdir _rmdir
    #define PATH_SEP "\\"
    #define PATH_SEP_CH '\\'
#else
    #include <unistd.h>
    #include <pwd.h>             // getpwuid
    #include <limits.h>          // PATH_MAX
    #include <dirent.h>          // opendir, readdir
    #define PATH_SEP "/"
    #define PATH_SEP_CH '/'
#endif

namespace bantu_pkg {

// ════════════════════════════════════════════════════════════════════
//  Path / filesystem helpers
// ════════════════════════════════════════════════════════════════════

inline bool fileExists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

inline bool isDir(const std::string& path) {
    struct stat buffer;
    if (stat(path.c_str(), &buffer) != 0) return false;
    return S_ISDIR(buffer.st_mode);
}

inline std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

// Join two path segments with the platform separator.
inline std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + PATH_SEP + b;
}

// mkdir -p style recursive directory creation.
inline bool makeDirs(const std::string& path) {
    if (path.empty()) return false;
    if (isDir(path)) return true;

    // Build parent first.
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos && pos > 0) {
        std::string parent = path.substr(0, pos);
        if (!makeDirs(parent)) return false;
    }
#ifdef _WIN32
    return (_mkdir(path.c_str()) == 0) || errno == EEXIST;
#else
    return (mkdir(path.c_str(), 0755) == 0) || errno == EEXIST;
#endif
}

// Copy a single file.
inline bool copyFile(const std::string& src, const std::string& dst) {
    std::string content = readFile(src);
    if (content.empty() && !fileExists(src)) return false;
    return writeFile(dst, content);
}

// Copy a directory recursively. Returns number of files copied.
inline int copyDir(const std::string& src, const std::string& dst) {
    if (!isDir(src)) return 0;
    makeDirs(dst);
    int count = 0;
#ifdef _WIN32
    std::string pattern = src + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string s = join(src, name);
        std::string d = join(dst, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            count += copyDir(s, d);
        } else {
            if (copyFile(s, d)) count++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* dir = opendir(src.c_str());
    if (!dir) return 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string s = join(src, name);
        std::string d = join(dst, name);
        struct stat st;
        if (stat(s.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            count += copyDir(s, d);
        } else {
            if (copyFile(s, d)) count++;
        }
    }
    closedir(dir);
#endif
    return count;
}

// Remove a directory tree (rm -rf style).
inline bool removeDir(const std::string& path) {
    if (!fileExists(path)) return true;
#ifdef _WIN32
    // Recurse first, then remove.
    std::string pattern = path + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            std::string full = join(path, name);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                removeDir(full);
            } else {
                ::remove(full.c_str());
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return _rmdir(path.c_str()) == 0 || errno == ENOENT;
#else
    DIR* dir = opendir(path.c_str());
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            std::string full = join(path, name);
            struct stat st;
            if (stat(full.c_str(), &st) == 0) {
                if (S_ISDIR(st.st_mode)) removeDir(full);
                else ::remove(full.c_str());
            }
        }
        closedir(dir);
    }
    return rmdir(path.c_str()) == 0 || errno == ENOENT;
#endif
}

inline std::string getcwd_str() {
    char buf[2048];
    if (getcwd(buf, sizeof(buf)) != nullptr) return std::string(buf);
    return ".";
}

// ─── Home dir + Bantu install dirs ───────────────────────────────────

inline std::string getHomeDir() {
#ifdef _WIN32
    const char* up = std::getenv("USERPROFILE");
    if (up && *up) return std::string(up);
    const char* hd = std::getenv("HOMEDRIVE");
    const char* hp = std::getenv("HOMEPATH");
    if (hd && hp) return std::string(hd) + std::string(hp);
    return "C:\\Users\\Default";
#else
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home);
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir) return std::string(pw->pw_dir);
    return "/tmp";
#endif
}

// ~/.bantu/  (or %USERPROFILE%\.bantu\)
inline std::string getBantuDir() {
    return join(getHomeDir(), ".bantu");
}

// ~/.bantu/registry/
inline std::string getRegistryDir() {
    return join(getBantuDir(), "registry");
}

// Where to install the bantu binary itself:
//   Linux/macOS: ~/.local/bin/  (user, no sudo)
//   Windows:     %USERPROFILE%/.bantu/bin/
inline std::string getUserInstallBinDir() {
#ifdef _WIN32
    return join(getBantuDir(), "bin");
#else
    return join(join(getHomeDir(), ".local"), "bin");
#endif
}

inline std::string getSystemInstallBinDir() {
#ifdef _WIN32
    return "C:\\Program Files\\bantu";
#else
    return "/usr/local/bin";
#endif
}

// Find the path to the currently-running bantu binary.
inline std::string getSelfBinaryPath() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (len == 0) return "";
    return std::string(buf, len);
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) {
        // macOS fallback
        len = readlink("/proc/curproc/file", buf, sizeof(buf) - 1);
    }
    if (len <= 0) {
        // Last-ditch: try argv[0] via $PATH
        return "bantu";
    }
    buf[len] = '\0';
    return std::string(buf);
#endif
}

inline std::string getBasename(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? path : path.substr(p + 1);
}

inline std::string getDirname(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? "." : path.substr(0, p);
}

// ════════════════════════════════════════════════════════════════════
//  PATH integration
// ════════════════════════════════════════════════════════════════════

// Check if a directory is already on PATH.
inline bool isOnPath(const std::string& dir) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p = path;
    std::string sep =
#ifdef _WIN32
        ";";
#else
        ":";
#endif
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(sep, start);
        std::string entry = (end == std::string::npos)
            ? p.substr(start)
            : p.substr(start, end - start);
        if (entry == dir) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

// Returns true if `bantu --version` resolves on PATH (i.e., bantu is callable
// from any directory).
inline bool isBantuOnPath() {
#ifdef _WIN32
    return isOnPath(getUserInstallBinDir());
#else
    return isOnPath(getUserInstallBinDir()) || isOnPath(getSystemInstallBinDir());
#endif
}

#ifdef _WIN32
// Append `dir` to the user's permanent PATH (HKCU\Environment\Path).
// Uses PowerShell to avoid the 1024-char truncation of `setx PATH`.
inline bool addToWindowsUserPath(const std::string& dir) {
    // Check if it's already there (read current value via PowerShell).
    std::string checkCmd =
        "powershell -NoProfile -Command \""
        "$p = [Environment]::GetEnvironmentVariable('Path','User'); "
        "if ($p -split ';' -contains '" + dir + "') { 'yes' } else { 'no' }"
        "\"";
    FILE* pipe = _popen(checkCmd.c_str(), "r");
    if (pipe) {
        char buf[64];
        std::string out;
        while (fgets(buf, sizeof(buf), pipe)) out += buf;
        _pclose(pipe);
        if (out.find("yes") != std::string::npos) return true;
    }

    std::string cmd =
        "powershell -NoProfile -Command \""
        "$cur = [Environment]::GetEnvironmentVariable('Path','User'); "
        "if ([string]::IsNullOrEmpty($cur)) { $new = '" + dir + "' } "
        "else { $new = $cur + ';' + '" + dir + "' }; "
        "[Environment]::SetEnvironmentVariable('Path', $new, 'User')"
        "\"";
    return (std::system(cmd.c_str()) == 0);
}

// Remove `dir` from the user's permanent PATH.
inline bool removeFromWindowsUserPath(const std::string& dir) {
    std::string cmd =
        "powershell -NoProfile -Command \""
        "$cur = [Environment]::GetEnvironmentVariable('Path','User'); "
        "$parts = $cur -split ';' | Where-Object { $_ -ne '" + dir + "' }; "
        "$new = $parts -join ';'; "
        "[Environment]::SetEnvironmentVariable('Path', $new, 'User')"
        "\"";
    return (std::system(cmd.c_str()) == 0);
}
#endif

#ifndef _WIN32
// Append `export PATH="$dir:$PATH"` to a shell rc file if not present.
inline bool addToShellRc(const std::string& rcPath, const std::string& dir) {
    if (!fileExists(rcPath)) {
        // Create it.
        writeFile(rcPath, "# Added by bantu setup\n");
    }
    std::string content = readFile(rcPath);
    if (content.find(dir) != std::string::npos) {
        return true;  // Already there.
    }
    std::string line = "\n# Added by bantu setup\nexport PATH=\"" + dir + ":$PATH\"\n";
    return writeFile(rcPath, content + line);
}

inline bool removeFromShellRc(const std::string& rcPath, const std::string& dir) {
    if (!fileExists(rcPath)) return true;
    std::string content = readFile(rcPath);
    std::string out;
    size_t pos = 0;
    bool changed = false;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = (nl == std::string::npos)
            ? content.substr(pos)
            : content.substr(pos, nl - pos);
        // Skip lines that reference our dir OR the marker comment.
        if (line.find(dir) != std::string::npos
            || line.find("# Added by bantu setup") != std::string::npos) {
            changed = true;
        } else {
            out += line;
            if (nl != std::string::npos) out += "\n";
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (changed) writeFile(rcPath, out);
    return true;
}
#endif

// Copy the current bantu binary (and on Windows, its sibling DLLs)
// into the install directory.
inline bool installSelfTo(const std::string& installDir) {
    std::string selfPath = getSelfBinaryPath();
    if (selfPath.empty() || selfPath == "bantu") {
        std::cerr << "  [ERROR] Cannot locate running bantu binary.\n";
        return false;
    }

    if (!makeDirs(installDir)) {
        std::cerr << "  [ERROR] Cannot create install dir: " << installDir << "\n";
        return false;
    }

    std::string selfDir = getDirname(selfPath);
    std::string selfBase = getBasename(selfPath);

#ifdef _WIN32
    // On Windows, copy bantu.exe + every *.dll next to it.
    std::string targetExe = join(installDir, "bantu.exe");
    if (!copyFile(selfPath, targetExe)) {
        std::cerr << "  [ERROR] Failed to copy " << selfPath << " -> " << targetExe << "\n";
        return false;
    }
    // Scan for sibling DLLs.
    std::string pattern = selfDir + "\\*.dll";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    int dllCount = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            std::string src = join(selfDir, name);
            std::string dst = join(installDir, name);
            if (copyFile(src, dst)) dllCount++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::cout << "  [OK] Copied bantu.exe + " << dllCount << " DLL(s) to " << installDir << "\n";
    return true;
#else
    std::string target = join(installDir, "bantu");
    if (!copyFile(selfPath, target)) {
        std::cerr << "  [ERROR] Failed to copy " << selfPath << " -> " << target << "\n";
        return false;
    }
    chmod(target.c_str(), 0755);
    std::cout << "  [OK] Copied bantu -> " << target << "\n";
    return true;
#endif
}

// Full PATH integration: copy binary + modify PATH env var.
// systemWide = true  → /usr/local/bin (Linux, needs sudo) | Program Files (Windows, admin)
// systemWide = false → ~/.local/bin (Linux) | %USERPROFILE%\.bantu\bin (Windows)
inline bool installToPath(bool systemWide) {
    std::string installDir = systemWide
        ? getSystemInstallBinDir()
        : getUserInstallBinDir();

    std::cout << "  [SETUP] Installing bantu to: " << installDir << "\n";

#ifndef _WIN32
    if (systemWide) {
        // On Linux, system install needs sudo for /usr/local/bin.
        std::cout << "  [INFO] System install requires root. Trying with sudo...\n";
        std::string selfPath = getSelfBinaryPath();
        std::string cmd = "sudo install -m 0755 \"" + selfPath + "\" \"" + installDir + "/bantu\"";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "  [ERROR] sudo install failed (rc=" << rc << ")\n";
            return false;
        }
        std::cout << "  [OK] bantu installed system-wide at " << installDir << "/bantu\n";
        std::cout << "\n  bantu --version  should now work from any directory.\n";
        return true;
    }
#endif

    if (!installSelfTo(installDir)) return false;

    // ─── Modify PATH ───
#ifdef _WIN32
    if (!addToWindowsUserPath(installDir)) {
        std::cerr << "  [WARN] Failed to add " << installDir << " to user PATH.\n";
        std::cerr << "         Add it manually via System → Environment Variables.\n";
        return false;
    }
    std::cout << "  [OK] Added " << installDir << " to user PATH.\n";
    std::cout << "\n  IMPORTANT: Close this terminal and open a new one for the PATH change to take effect.\n";
    std::cout << "             (Or in VSCode: close integrated terminal with Ctrl+Shift+P → 'Terminal: Kill All' and reopen.)\n";
#else
    // Append to ~/.bashrc, ~/.zshrc, and ~/.profile (whichever exist).
    std::string home = getHomeDir();
    std::vector<std::string> rcs = {
        join(home, ".bashrc"),
        join(home, ".zshrc"),
        join(home, ".profile"),
        join(home, ".bash_profile")
    };
    bool added = false;
    for (const auto& rc : rcs) {
        // Only modify rc files that already exist (or .bashrc which we'll create if missing).
        if (fileExists(rc) || rc.find(".bashrc") != std::string::npos) {
            if (addToShellRc(rc, installDir)) {
                std::cout << "  [OK] Updated " << rc << "\n";
                added = true;
            }
        }
    }
    if (!added) {
        std::cerr << "  [WARN] No shell rc files found. Manually add to PATH:\n";
        std::cerr << "         export PATH=\"" << installDir << ":$PATH\"\n";
    } else {
        std::cout << "\n  IMPORTANT: Run 'source ~/.bashrc' (or open a new terminal) for PATH to take effect.\n";
    }
#endif

    return true;
}

// Remove bantu from PATH (does not delete the binary, just removes the PATH entry).
inline bool removeFromPath() {
    bool ok = true;
#ifdef _WIN32
    std::string installDir = getUserInstallBinDir();
    ok &= removeFromWindowsUserPath(installDir);
    std::cout << "  [OK] Removed " << installDir << " from user PATH.\n";
#else
    std::string installDir = getUserInstallBinDir();
    std::string home = getHomeDir();
    std::vector<std::string> rcs = {
        join(home, ".bashrc"),
        join(home, ".zshrc"),
        join(home, ".profile"),
        join(home, ".bash_profile")
    };
    for (const auto& rc : rcs) {
        if (fileExists(rc)) {
            removeFromShellRc(rc, installDir);
            std::cout << "  [OK] Cleaned " << rc << "\n";
        }
    }
#endif
    return ok;
}

// ════════════════════════════════════════════════════════════════════
//  Manifest (bantu.json)
// ════════════════════════════════════════════════════════════════════

struct Dependency {
    std::string name;
    std::string version;  // "latest" means "newest in registry"
};

struct Manifest {
    std::string name;
    std::string version;
    std::string entry;
    std::string language;
    std::string bantuVersion;
    std::string templateType;  // "cli" or "web"
    std::vector<Dependency> dependencies;
};

// Tiny JSON-ish reader for the subset we care about in bantu.json.
// We don't ship a JSON library, so we do regex-style scanning.
// This handles the format we WRITE (see writeManifest), nothing fancier.
inline std::string extractJsonField(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return "";
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return "";
    p++;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
    if (p >= json.size()) return "";
    if (json[p] == '"') {
        size_t end = json.find('"', p + 1);
        if (end == std::string::npos) return "";
        return json.substr(p + 1, end - p - 1);
    }
    // Bare value (number, true/false)
    size_t end = p;
    while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != '\n') end++;
    return json.substr(p, end - p);
}

// Extract "dependencies": { "name": "ver", "name2": "ver2" }
inline std::vector<Dependency> extractDeps(const std::string& json) {
    std::vector<Dependency> deps;
    size_t p = json.find("\"dependencies\"");
    if (p == std::string::npos) return deps;
    size_t brace = json.find('{', p);
    if (brace == std::string::npos) return deps;
    size_t end = json.find('}', brace);
    if (end == std::string::npos) return deps;
    std::string body = json.substr(brace + 1, end - brace - 1);

    // Parse "key": "value" pairs.
    size_t pos = 0;
    while (pos < body.size()) {
        size_t k1 = body.find('"', pos);
        if (k1 == std::string::npos) break;
        size_t k2 = body.find('"', k1 + 1);
        if (k2 == std::string::npos) break;
        std::string name = body.substr(k1 + 1, k2 - k1 - 1);
        size_t colon = body.find(':', k2);
        if (colon == std::string::npos) break;
        size_t v1 = body.find('"', colon);
        if (v1 == std::string::npos) break;
        size_t v2 = body.find('"', v1 + 1);
        if (v2 == std::string::npos) break;
        std::string ver = body.substr(v1 + 1, v2 - v1 - 1);
        deps.push_back({ name, ver });
        pos = v2 + 1;
    }
    return deps;
}

inline Manifest readManifest(const std::string& path) {
    Manifest m;
    std::string content = readFile(path);
    if (content.empty()) return m;
    m.name          = extractJsonField(content, "name");
    m.version       = extractJsonField(content, "version");
    m.entry         = extractJsonField(content, "entry");
    m.language      = extractJsonField(content, "language");
    m.bantuVersion  = extractJsonField(content, "bantuVersion");
    m.templateType  = extractJsonField(content, "template");
    m.dependencies  = extractDeps(content);
    return m;
}

inline bool writeManifest(const std::string& path, const Manifest& m) {
    std::ostringstream s;
    s << "{\n";
    s << "  \"name\": \""        << m.name << "\",\n";
    s << "  \"version\": \""     << m.version << "\",\n";
    s << "  \"entry\": \""       << m.entry << "\",\n";
    if (!m.templateType.empty())
        s << "  \"template\": \""     << m.templateType << "\",\n";
    s << "  \"language\": \""    << m.language << "\",\n";
    s << "  \"bantuVersion\": \"" << m.bantuVersion << "\",\n";
    if (m.dependencies.empty()) {
        s << "  \"dependencies\": {}\n";
    } else {
        s << "  \"dependencies\": {\n";
        for (size_t i = 0; i < m.dependencies.size(); ++i) {
            s << "    \"" << m.dependencies[i].name << "\": \""
              << m.dependencies[i].version << "\"";
            if (i + 1 < m.dependencies.size()) s << ",";
            s << "\n";
        }
        s << "  }\n";
    }
    s << "}\n";
    return writeFile(path, s.str());
}

// ════════════════════════════════════════════════════════════════════
//  Local registry (~/.bantu/registry/)
// ════════════════════════════════════════════════════════════════════

// List all packages in the local registry.
// Returns a map: name -> list of versions.
inline std::map<std::string, std::vector<std::string>> listRegistry() {
    std::map<std::string, std::vector<std::string>> out;
    std::string reg = getRegistryDir();
    if (!isDir(reg)) return out;

#ifdef _WIN32
    std::string pattern = reg + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::string pkgDir = join(reg, name);
        // Enumerate version subdirs.
        std::string vpat = pkgDir + "\\*";
        WIN32_FIND_DATAA vfd;
        HANDLE hv = FindFirstFileA(vpat.c_str(), &vfd);
        if (hv != INVALID_HANDLE_VALUE) {
            do {
                std::string v = vfd.cFileName;
                if (v == "." || v == "..") continue;
                if (vfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    out[name].push_back(v);
                }
            } while (FindNextFileA(hv, &vfd));
            FindClose(hv);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* dir = opendir(reg.c_str());
    if (!dir) return out;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string pkgDir = join(reg, name);
        struct stat st;
        if (stat(pkgDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        DIR* vdir = opendir(pkgDir.c_str());
        if (!vdir) continue;
        struct dirent* vent;
        while ((vent = readdir(vdir)) != nullptr) {
            std::string v = vent->d_name;
            if (v == "." || v == "..") continue;
            std::string vPath = join(pkgDir, v);
            struct stat vs;
            if (stat(vPath.c_str(), &vs) == 0 && S_ISDIR(vs.st_mode)) {
                out[name].push_back(v);
            }
        }
        closedir(vdir);
    }
    closedir(dir);
#endif
    return out;
}

// Find the newest version of a package in the registry.
// Returns empty string if not found.
inline std::string findLatestVersion(const std::string& pkgName) {
    auto reg = listRegistry();
    auto it = reg.find(pkgName);
    if (it == reg.end() || it->second.empty()) return "";
    std::string best = it->second[0];
    for (const auto& v : it->second) {
        if (v > best) best = v;  // Lexicographic — fine for "1.0.0" style.
    }
    return best;
}

// Find the registry path for a specific package + version.
// If version is "latest" or empty, picks the newest.
inline std::string findPackageInRegistry(const std::string& pkgName, std::string version) {
    if (version.empty() || version == "latest") {
        version = findLatestVersion(pkgName);
        if (version.empty()) return "";
    }
    std::string path = join(join(getRegistryDir(), pkgName), version);
    return isDir(path) ? path : "";
}

// Project-local modules dir: ./bantu_modules/
inline std::string getModulesDir() {
    return join(getcwd_str(), "bantu_modules");
}

inline std::string getModuleDir(const std::string& pkgName) {
    return join(getModulesDir(), pkgName);
}

// Is a package currently installed in this project?
inline bool isPackageInstalled(const std::string& pkgName) {
    return isDir(getModuleDir(pkgName));
}

// Install a package from the local registry into ./bantu_modules/<name>/
inline bool installPackage(const std::string& pkgName, std::string version) {
    std::string src = findPackageInRegistry(pkgName, version);
    if (src.empty()) {
        std::cerr << "  [ERROR] Package not found in local registry: " << pkgName;
        if (!version.empty() && version != "latest")
            std::cerr << "@" << version;
        std::cerr << "\n";
        std::cerr << "  Available packages (bantu search):\n";
        auto reg = listRegistry();
        if (reg.empty()) {
            std::cerr << "    (registry is empty)\n";
            std::cerr << "  Try:  bantu publish ./your-pkg-folder\n";
        } else {
            for (const auto& kv : reg) {
                std::cerr << "    " << kv.first;
                if (!kv.second.empty()) std::cerr << " (v" << kv.second.back() << ")";
                std::cerr << "\n";
            }
        }
        return false;
    }

    std::string dst = getModuleDir(pkgName);
    if (isDir(dst)) {
        std::cout << "  [INFO] Reinstalling " << pkgName << " (overwriting)...\n";
        removeDir(dst);
    }
    makeDirs(getModulesDir());
    int n = copyDir(src, dst);
    if (n == 0) {
        std::cerr << "  [ERROR] Failed to copy package files from " << src << "\n";
        return false;
    }
    std::cout << "  [OK] Installed " << pkgName << " (" << n << " files) -> " << dst << "\n";
    return true;
}

// Remove a package from ./bantu_modules/
inline bool uninstallPackage(const std::string& pkgName) {
    std::string dst = getModuleDir(pkgName);
    if (!isDir(dst)) {
        std::cout << "  [INFO] " << pkgName << " is not installed.\n";
        return true;
    }
    if (removeDir(dst)) {
        std::cout << "  [OK] Removed " << pkgName << " from bantu_modules/\n";
        return true;
    }
    std::cerr << "  [ERROR] Failed to remove " << dst << "\n";
    return false;
}

// Add a dependency to bantu.json + install it.
// `spec` can be "pkg-name" or "pkg-name@version".
inline bool addDependency(const std::string& spec) {
    std::string name = spec;
    std::string ver = "latest";
    size_t at = spec.find('@');
    if (at != std::string::npos) {
        name = spec.substr(0, at);
        ver = spec.substr(at + 1);
    }

    if (!installPackage(name, ver)) return false;

    // Resolve actual installed version (for the manifest).
    if (ver == "latest" || ver.empty()) {
        ver = findLatestVersion(name);
        if (ver.empty()) ver = "latest";
    }

    // Update bantu.json.
    std::string manifestPath = join(getcwd_str(), "bantu.json");
    Manifest m = readManifest(manifestPath);
    if (m.name.empty()) {
        // No manifest yet — create a minimal one.
        m.name = getBasename(getcwd_str());
        m.version = "1.0.0";
        m.entry = "main.b";
        m.language = "bantu";
        m.bantuVersion = "1.1.0";
    }

    // Replace if exists, else append.
    bool found = false;
    for (auto& d : m.dependencies) {
        if (d.name == name) {
            d.version = ver;
            found = true;
            break;
        }
    }
    if (!found) m.dependencies.push_back({ name, ver });

    if (writeManifest(manifestPath, m)) {
        std::cout << "  [OK] Added \"" << name << "\": \"" << ver << "\" to bantu.json\n";
    } else {
        std::cerr << "  [WARN] Could not update bantu.json\n";
    }

    // Ensure bantu_modules/.gitignore exists (so packages aren't committed).
    std::string gi = join(getModulesDir(), ".gitignore");
    if (!fileExists(gi)) {
        writeFile(gi, "# Auto-created by bantu add\n*\n!.gitignore\n");
    }

    return true;
}

// Remove a dependency from bantu.json + uninstall it.
inline bool removeDependency(const std::string& pkgName) {
    std::string manifestPath = join(getcwd_str(), "bantu.json");
    Manifest m = readManifest(manifestPath);
    if (m.name.empty()) {
        std::cerr << "  [ERROR] No bantu.json found in current directory.\n";
        return false;
    }

    bool found = false;
    std::vector<Dependency> kept;
    for (const auto& d : m.dependencies) {
        if (d.name == pkgName) {
            found = true;
        } else {
            kept.push_back(d);
        }
    }
    if (!found) {
        std::cerr << "  [WARN] " << pkgName << " is not listed in bantu.json\n";
    } else {
        m.dependencies = kept;
        writeManifest(manifestPath, m);
        std::cout << "  [OK] Removed \"" << pkgName << "\" from bantu.json\n";
    }

    uninstallPackage(pkgName);
    return true;
}

// Install every dependency listed in bantu.json.
// Returns count of successfully installed packages.
inline int installAllFromManifest() {
    std::string manifestPath = join(getcwd_str(), "bantu.json");
    if (!fileExists(manifestPath)) {
        std::cerr << "  [ERROR] No bantu.json in current directory.\n";
        std::cerr << "  Run 'bantu init <name>' first, or 'bantu add <pkg>' to create one.\n";
        return -1;
    }
    Manifest m = readManifest(manifestPath);
    if (m.dependencies.empty()) {
        std::cout << "  [OK] No dependencies in bantu.json — nothing to install.\n";
        return 0;
    }

    makeDirs(getModulesDir());

    int ok = 0;
    for (const auto& d : m.dependencies) {
        std::cout << "\n  ── Installing " << d.name << "@" << d.version << " ──\n";
        if (installPackage(d.name, d.version)) ok++;
    }
    std::cout << "\n  ────────────────────────────\n";
    std::cout << "  Installed " << ok << "/" << m.dependencies.size() << " packages.\n";
    return ok;
}

// Update a specific package to its latest version.
inline bool updatePackage(const std::string& pkgName) {
    std::string latest = findLatestVersion(pkgName);
    if (latest.empty()) {
        std::cerr << "  [ERROR] Package not found in registry: " << pkgName << "\n";
        return false;
    }

    // Update bantu.json with the new version.
    std::string manifestPath = join(getcwd_str(), "bantu.json");
    Manifest m = readManifest(manifestPath);
    bool found = false;
    for (auto& d : m.dependencies) {
        if (d.name == pkgName) {
            d.version = latest;
            found = true;
            break;
        }
    }
    if (!found) {
        std::cerr << "  [WARN] " << pkgName << " not in bantu.json — adding it.\n";
        m.dependencies.push_back({ pkgName, latest });
    }
    writeManifest(manifestPath, m);

    // Reinstall.
    if (isPackageInstalled(pkgName)) {
        removeDir(getModuleDir(pkgName));
    }
    return installPackage(pkgName, latest);
}

// Update all dependencies to their latest versions.
inline int updateAll() {
    std::string manifestPath = join(getcwd_str(), "bantu.json");
    if (!fileExists(manifestPath)) {
        std::cerr << "  [ERROR] No bantu.json in current directory.\n";
        return -1;
    }
    Manifest m = readManifest(manifestPath);
    if (m.dependencies.empty()) {
        std::cout << "  [OK] No dependencies to update.\n";
        return 0;
    }

    int ok = 0;
    for (const auto& d : m.dependencies) {
        std::cout << "\n  ── Updating " << d.name << " ──\n";
        if (updatePackage(d.name)) ok++;
    }
    std::cout << "\n  ────────────────────────────\n";
    std::cout << "  Updated " << ok << "/" << m.dependencies.size() << " packages.\n";
    return ok;
}

// List packages installed in the current project (./bantu_modules/).
inline void listInstalled() {
    std::string modDir = getModulesDir();
    if (!isDir(modDir)) {
        std::cout << "  No bantu_modules/ directory — no packages installed.\n";
        return;
    }

    std::string manifestPath = join(getcwd_str(), "bantu.json");
    Manifest m = readManifest(manifestPath);

    std::cout << "\n  Installed packages (./bantu_modules/):\n";
    std::cout << "  ────────────────────────────\n";

#ifdef _WIN32
    std::string pattern = modDir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        std::cout << "  (none)\n";
        return;
    }
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == ".." || name == ".gitignore") continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::string reqVer = "?";
        for (const auto& d : m.dependencies) {
            if (d.name == name) { reqVer = d.version; break; }
        }
        std::cout << "    " << name << "  (requested: " << reqVer << ")\n";
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* dir = opendir(modDir.c_str());
    if (!dir) { std::cout << "  (none)\n"; return; }
    struct dirent* ent;
    bool any = false;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == ".." || name == ".gitignore") continue;
        std::string full = join(modDir, name);
        struct stat st;
        if (stat(full.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        std::string reqVer = "?";
        for (const auto& d : m.dependencies) {
            if (d.name == name) { reqVer = d.version; break; }
        }
        std::cout << "    " << name << "  (requested: " << reqVer << ")\n";
        any = true;
    }
    closedir(dir);
    if (!any) std::cout << "  (none)\n";
#endif
}

// Search the local registry for packages matching `term` (substring match).
// Empty term lists all.
inline void searchRegistry(const std::string& term) {
    auto reg = listRegistry();
    if (reg.empty()) {
        std::cout << "\n  Local registry is empty.\n";
        std::cout << "  ────────────────────────────\n";
        std::cout << "  To add packages:\n";
        std::cout << "    1. Create a folder with your .b files + package.json\n";
        std::cout << "    2. bantu publish ./your-folder\n";
        std::cout << "  Registry location: " << getRegistryDir() << "\n\n";
        return;
    }

    std::cout << "\n  Local registry (" << getRegistryDir() << "):\n";
    std::cout << "  ────────────────────────────\n";
    int shown = 0;
    for (const auto& kv : reg) {
        if (!term.empty() && kv.first.find(term) == std::string::npos) continue;
        std::cout << "  " << kv.first;
        if (!kv.second.empty()) {
            std::cout << "  versions: ";
            for (size_t i = 0; i < kv.second.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << kv.second[i];
            }
        }
        // Show package.json description if present.
        std::string pj = join(join(join(getRegistryDir(), kv.first),
                                  kv.second.empty() ? "" : kv.second.back()),
                              "package.json");
        if (fileExists(pj)) {
            std::string content = readFile(pj);
            std::string desc = extractJsonField(content, "description");
            if (!desc.empty()) std::cout << "\n      " << desc;
        }
        std::cout << "\n";
        shown++;
    }
    if (shown == 0) {
        std::cout << "  No packages match '" << term << "'.\n";
    }
    std::cout << "\n";
}

// Publish a directory to the local registry.
//   publishDir  — folder containing package.json + .b source files
//   asName      — override package name (optional, empty = use package.json name)
// Returns true on success.
inline bool publishPackage(const std::string& publishDir, const std::string& asName) {
    if (!isDir(publishDir)) {
        std::cerr << "  [ERROR] Not a directory: " << publishDir << "\n";
        return false;
    }

    // Read package.json from the source directory.
    std::string pjPath = join(publishDir, "package.json");
    if (!fileExists(pjPath)) {
        std::cerr << "  [ERROR] No package.json in " << publishDir << "\n";
        std::cerr << "  Expected format:\n";
        std::cerr << "    {\n";
        std::cerr << "      \"name\": \"my-pkg\",\n";
        std::cerr << "      \"version\": \"1.0.0\",\n";
        std::cerr << "      \"main\": \"index.b\",\n";
        std::cerr << "      \"description\": \"...\"\n";
        std::cerr << "    }\n";
        return false;
    }

    std::string pjContent = readFile(pjPath);
    std::string name = asName.empty() ? extractJsonField(pjContent, "name") : asName;
    std::string version = extractJsonField(pjContent, "version");

    if (name.empty()) {
        std::cerr << "  [ERROR] package.json has no \"name\" field.\n";
        return false;
    }
    if (version.empty()) {
        version = "1.0.0";
        std::cout << "  [INFO] No version in package.json, defaulting to 1.0.0\n";
    }

    // Reject bad names.
    if (name.find_first_of("/\\:*?\"<>|") != std::string::npos
        || name.find(' ') != std::string::npos) {
        std::cerr << "  [ERROR] Invalid package name: '" << name << "'\n";
        return false;
    }

    std::string target = join(join(getRegistryDir(), name), version);
    if (isDir(target)) {
        std::cout << "  [INFO] Overwriting existing " << name << "@" << version << "\n";
        removeDir(target);
    }
    makeDirs(target);

    int n = copyDir(publishDir, target);
    if (n == 0) {
        std::cerr << "  [ERROR] Failed to copy package files.\n";
        return false;
    }

    std::cout << "  [OK] Published " << name << "@" << version << " (" << n << " files)\n";
    std::cout << "  Location: " << target << "\n";
    std::cout << "\n  Use it in any project:\n";
    std::cout << "    bantu add " << name << "\n";
    return true;
}

// ════════════════════════════════════════════════════════════════════
//  Doctor — diagnose install + registry state
// ════════════════════════════════════════════════════════════════════

inline int runDoctor() {
    int issues = 0;
    std::cout << "\n  Bantu install diagnostics\n";
    std::cout << "  ════════════════════════════\n\n";

    // Self path
    std::string self = getSelfBinaryPath();
    std::cout << "  [1] Running binary:       " << self << "\n";
    if (self.empty() || self == "bantu") {
        std::cout << "      ⚠ Could not determine running binary path.\n";
        issues++;
    }

    // Home dir
    std::string home = getHomeDir();
    std::cout << "  [2] Home directory:       " << home << "\n";

    // Bantu dir
    std::string bdir = getBantuDir();
    bool bdirOk = isDir(bdir);
    std::cout << "  [3] Bantu config dir:     " << bdir << "  "
              << (bdirOk ? "[ OK ]" : "[ missing ]") << "\n";
    if (!bdirOk) {
        std::cout << "      Will be auto-created on first use.\n";
    }

    // Registry
    std::string reg = getRegistryDir();
    bool regOk = isDir(reg);
    std::cout << "  [4] Local registry:       " << reg << "  "
              << (regOk ? "[ OK ]" : "[ empty ]") << "\n";
    if (regOk) {
        auto packages = listRegistry();
        std::cout << "      Packages: " << packages.size() << "\n";
    }

    // Install dir
    std::string userBin = getUserInstallBinDir();
    bool userBinOk = isDir(userBin);
    std::cout << "  [5] User install dir:     " << userBin << "  "
              << (userBinOk ? "[ OK ]" : "[ missing ]") << "\n";
#ifdef _WIN32
    std::string installedExe = join(userBin, "bantu.exe");
#else
    std::string installedExe = join(userBin, "bantu");
#endif
    if (userBinOk) {
        bool hasBin = fileExists(installedExe);
        std::cout << "      bantu installed: " << (hasBin ? "[ yes ]" : "[ no ]") << "\n";
    }

    // PATH
    bool onPath = isBantuOnPath();
    std::cout << "  [6] bantu on PATH:        " << (onPath ? "[ yes ]" : "[ no ]") << "\n";
    if (!onPath) {
        std::cout << "      ⚠ Run 'bantu setup' to add it.\n";
        issues++;
    }

    // Project manifest
    std::string manifestPath = join(getcwd_str(), "bantu.json");
    if (fileExists(manifestPath)) {
        Manifest m = readManifest(manifestPath);
        std::cout << "  [7] Project manifest:     " << manifestPath << "  [ OK ]\n";
        std::cout << "      name:           " << m.name << "\n";
        std::cout << "      version:        " << m.version << "\n";
        std::cout << "      entry:          " << m.entry << "\n";
        std::cout << "      dependencies:   " << m.dependencies.size() << "\n";
    } else {
        std::cout << "  [7] Project manifest:     (none in cwd)\n";
    }

    // Project modules
    std::string modDir = getModulesDir();
    if (isDir(modDir)) {
        int count = 0;
#ifdef _WIN32
        std::string pattern = modDir + "\\*";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::string name = fd.cFileName;
                if (name == "." || name == ".." || name == ".gitignore") continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) count++;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
#else
        DIR* dir = opendir(modDir.c_str());
        if (dir) {
            struct dirent* ent;
            while ((ent = readdir(dir)) != nullptr) {
                std::string name = ent->d_name;
                if (name == "." || name == ".." || name == ".gitignore") continue;
                std::string full = join(modDir, name);
                struct stat st;
                if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) count++;
            }
            closedir(dir);
        }
#endif
        std::cout << "  [8] bantu_modules/:       " << modDir << "  (" << count << " pkgs)\n";
    } else {
        std::cout << "  [8] bantu_modules/:       (none)\n";
    }

    std::cout << "\n  Issues found: " << issues << "\n";
    std::cout << "\n";
    return issues;
}

// ════════════════════════════════════════════════════════════════════
//  Starter packages — seed the local registry with useful utilities
// ════════════════════════════════════════════════════════════════════

// Write a starter package into a temp directory, then publish it.
inline bool seedStarterPackage(const std::string& name, const std::string& description,
                                const std::string& mainB) {
    // Stage into a temp dir.
    std::string staging =
#ifdef _WIN32
        std::string(std::getenv("TEMP") ? std::getenv("TEMP") : "C:\\Temp");
#else
        "/tmp";
#endif
    staging = join(staging, "bantu-seed-" + name);
    if (isDir(staging)) removeDir(staging);
    makeDirs(staging);

    std::string pj = "{\n"
                     "  \"name\": \"" + name + "\",\n"
                     "  \"version\": \"1.0.0\",\n"
                     "  \"main\": \"index.b\",\n"
                     "  \"description\": \"" + description + "\"\n"
                     "}\n";
    writeFile(join(staging, "package.json"), pj);
    writeFile(join(staging, "index.b"), mainB);

    bool ok = publishPackage(staging, name);
    removeDir(staging);
    return ok;
}

// Seed the local registry with a handful of useful starter packages
// so users have something to install right away (works fully offline).
inline int seedStarterRegistry() {
    int n = 0;

    // math-utils
    if (seedStarterPackage(
            "math-utils",
            "Math helpers: add, sub, mul, div, factorial, fibonacci",
            "// math-utils — starter package\n"
            "def add($a, $b) { return $a + $b; }\n"
            "def sub($a, $b) { return $a - $b; }\n"
            "def mul($a, $b) { return $a * $b; }\n"
            "def div($a, $b) { if ($b == 0) { return 0; } return $a / $b; }\n\n"
            "def factorial($n) {\n"
            "    if ($n <= 1) { return 1; }\n"
            "    return $n * factorial($n - 1);\n"
            "}\n\n"
            "def fibonacci($n) {\n"
            "    if ($n < 2) { return $n; }\n"
            "    return fibonacci($n - 1) + fibonacci($n - 2);\n"
            "}\n"
            )) n++;

    // string-utils
    if (seedStarterPackage(
            "string-utils",
            "String helpers: upper, lower, reverse, trim, length, contains",
            "// string-utils — starter package\n"
            "def upper($s) {\n"
            "    // Bantu has no built-in upper(); use ASCII math.\n"
            "    string $out = \"\";\n"
            "    number $i = 0;\n"
            "    while ($i < len($s)) {\n"
            "        string $c = $s[$i];\n"
            "        number $code = asc($c);\n"
            "        if ($code >= 97 && $code <= 122) {\n"
            "            $c = chr($code - 32);\n"
            "        }\n"
            "        $out = $out + $c;\n"
            "        $i = $i + 1;\n"
            "    }\n"
            "    return $out;\n"
            "}\n\n"
            "def reverse($s) {\n"
            "    string $out = \"\";\n"
            "    number $i = len($s) - 1;\n"
            "    while ($i >= 0) {\n"
            "        $out = $out + $s[$i];\n"
            "        $i = $i - 1;\n"
            "    }\n"
            "    return $out;\n"
            "}\n\n"
            "def contains($haystack, $needle) {\n"
            "    number $i = instr($haystack, $needle);\n"
            "    return $i >= 0;\n"
            "}\n"
            )) n++;

    // http-utils
    if (seedStarterPackage(
            "http-utils",
            "HTTP helpers: json ok/error responses, basic auth check",
            "// http-utils — starter package for Sua apps\n\n"
            "// Standard 200 OK JSON response.\n"
            "def ok($res, $data) {\n"
            "    $res.json({ \"ok\": true, \"data\": $data });\n"
            "    return null;\n"
            "}\n\n"
            "// Standard error response.\n"
            "def error($res, $status, $message) {\n"
            "    $res.status($status).json({ \"ok\": false, \"error\": $message });\n"
            "    return null;\n"
            "}\n\n"
            "// Check Bearer token.\n"
            "def requireAuth($req) {\n"
            "    string $auth = $req.headers.authorization;\n"
            "    if (!$auth) { return false; }\n"
            "    return startsWith($auth, \"Bearer \");\n"
            "}\n"
            )) n++;

    return n;
}

} // namespace bantu_pkg
