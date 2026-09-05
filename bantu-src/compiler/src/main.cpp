/**
 * Bantu Language - Main Entry Point
 *
 * Simple CLI Commands:
 *   bantu                    Start REPL
 *   bantu run <file.b>       Run a Bantu file
 *   bantu run                Run main.b in current directory
 *   bantu build <file.b>     Compile to standalone executable
 *   bantu build              Build main.b in current directory
 *   bantu init <name>        Create a new Bantu project
 *   bantu new <name>         Create a new Bantu project (alias)
 *   bantu --help             Show help
 *   bantu --version          Show version
 *
 * Build with CMake:
 *   mkdir build && cd build
 *   cmake .. -DCMAKE_BUILD_TYPE=Release
 *   make -j$(nproc)
 */

#include "types.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "linter.hpp"
#include "evaluator.hpp"
#include "environment.hpp"
#include "server.hpp"
#include "init_templates.hpp"
#include "package_manager.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cctype>
#include <bitset>
#include <cmath>
#include <algorithm>
#include <filesystem>     // C++17 — cross-platform mkdir/rm/cp (replaces POSIX shell calls)

// ─── Platform-specific POSIX headers ───
#ifdef _WIN32
    #include <direct.h>      // _getcwd
    #include <sys/stat.h>    // stat (works on Windows too)
    #define getcwd _getcwd
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>  // _NSGetExecutablePath
    #include <sys/stat.h>
    #include <unistd.h>
#else
    #include <sys/stat.h>
    #include <unistd.h>
#endif

const std::string BANTU_VERSION = "1.3.0";
const std::string BANTU_LANG = "Bantu";

// ─── Helpers ──────────────────────────────────────────────────────────

bool fileExists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

// ─── Cross-platform filesystem helpers (C++17 <filesystem>) ───────────
// These replace POSIX-only `system("mkdir -p ...")` / `system("rm -rf ...")`
// / `system("cp ...")` calls so the installer works identically on Windows,
// macOS, and Linux without needing a POSIX shell (no Git Bash / WSL required).

namespace fs = std::filesystem;

// Recursively create a directory (no-op if it already exists). Returns true
// on success or if the directory already existed.
bool fsMkdirP(const std::string& path) {
    if (path.empty()) return true;
    std::error_code ec;
    fs::create_directories(fs::path(path), ec);
    return !ec;
}

// Recursively remove a directory tree (no-op if it doesn't exist).
bool fsRmRf(const std::string& path) {
    if (path.empty()) return true;
    std::error_code ec;
    fs::remove_all(fs::path(path), ec);
    return !ec;
}

// Copy a single file. Overwrites the destination if it exists.
bool fsCopyFile(const std::string& src, const std::string& dst) {
    std::error_code ec;
    fs::copy_file(fs::path(src), fs::path(dst),
                  fs::copy_options::overwrite_existing, ec);
    return !ec;
}

// Recursively copy a directory tree. Creates dst if it doesn't exist.
bool fsCopyDir(const std::string& src, const std::string& dst) {
    std::error_code ec;
    fs::copy(fs::path(src), fs::path(dst),
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    return !ec;
}

// Copy all files matching a given extension from srcDir into dstDir.
// Used to copy *.b source files into the Android assets folder without
// relying on a shell glob.
int fsCopyByExtension(const std::string& srcDir, const std::string& dstDir,
                      const std::string& extWithDot) {
    if (!fsMkdirP(dstDir)) return 0;
    int copied = 0;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(fs::path(srcDir), ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == extWithDot) {
            std::string dst = dstDir + "/" + entry.path().filename().string();
            if (fsCopyFile(entry.path().string(), dst)) copied++;
        }
    }
    return copied;
}

// Copy a single named file from srcDir to dstDir if it exists.
bool fsCopyNamedIfExists(const std::string& srcDir, const std::string& dstDir,
                         const std::string& filename) {
    std::string src = srcDir + "/" + filename;
    if (!fileExists(src)) return false;
    if (!fsMkdirP(dstDir)) return false;
    return fsCopyFile(src, dstDir + "/" + filename);
}

// Recursively copy a named subdirectory (e.g. "public") from srcDir into dstDir.
bool fsCopyNamedDirIfExists(const std::string& srcDir, const std::string& dstDir,
                            const std::string& dirname) {
    std::string src = srcDir + "/" + dirname;
    if (!fs::is_directory(fs::path(src))) return false;
    if (!fsMkdirP(dstDir)) return false;
    return fsCopyDir(src, dstDir + "/" + dirname);
}

std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "  [ERROR] Cannot open file: " << path << "\n";
        return "";
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "  [ERROR] Cannot write to file: " << path << "\n";
        return false;
    }
    file << content;
    return true;
}

std::string getBasename(const std::string& path) {
    size_t lastSlash = path.find_last_of("/\\");
    std::string filename = (lastSlash == std::string::npos) ? path : path.substr(lastSlash + 1);
    size_t lastDot = filename.find_last_of('.');
    if (lastDot != std::string::npos) {
        return filename.substr(0, lastDot);
    }
    return filename;
}

std::string getCurrentDir() {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
        return std::string(cwd);
    }
    return ".";
}

// ─── Banner & Help ────────────────────────────────────────────────────

void printBanner() {
    std::cout << "\n";
    std::cout << "  ╔══════════════════════════════════════╗\n";
    std::cout << "  ║   Bantu Programming Language v" << BANTU_VERSION << "  ║\n";
    std::cout << "  ║   Built with C++ & CMake             ║\n";
    std::cout << "  ╚══════════════════════════════════════╝\n";
    std::cout << "\n";
    std::cout << "  Type 'exit' to quit, 'help' for commands\n";
    std::cout << "\n";
}

void printHelp() {
    std::cout << "\n";
    std::cout << "  Bantu Programming Language v" << BANTU_VERSION << "\n";
    std::cout << "  ─────────────────────────────────────\n";
    std::cout << "\n";
    std::cout << "  GETTING STARTED:\n";
    std::cout << "    bantu setup              Add bantu to PATH (user install)\n";
    std::cout << "    bantu setup --system     Add bantu to PATH (needs sudo/admin)\n";
    std::cout << "    bantu setup --seed       Also seed local registry with starter packages\n";
    std::cout << "    bantu uninstall          Remove bantu from PATH\n";
    std::cout << "    bantu doctor             Diagnose install + registry state\n";
    std::cout << "\n";
    std::cout << "  COMMANDS:\n";
    std::cout << "    bantu                    Start REPL\n";
    std::cout << "    bantu run <file.b>       Run a Bantu file\n";
    std::cout << "    bantu run                Run main.b in current dir\n";
    std::cout << "    bantu build <file.b>     Compile to executable\n";
    std::cout << "    bantu build              Build main.b in current dir\n";
    std::cout << "    bantu init <name>        Create a new CLI Bantu project\n";
    std::cout << "    bantu init --web <n>     Create a Sua web app (Spring Initializer-style)\n";
    std::cout << "    bantu new <name>         Alias of init\n";
    std::cout << "    bantu relay [port]       Start STUN/TURN relay server\n";
    std::cout << "\n";
    std::cout << "  PACKAGE MANAGER (offline, npm-style):\n";
    std::cout << "    bantu install            Install all deps from bantu.json\n";
    std::cout << "    bantu add <pkg>          Add a dep + install it\n";
    std::cout << "    bantu add <pkg>@<ver>    Pin a specific version\n";
    std::cout << "    bantu remove <pkg>       Remove a dep + uninstall it\n";
    std::cout << "    bantu update             Update all deps to latest\n";
    std::cout << "    bantu update <pkg>       Update one dep to latest\n";
    std::cout << "    bantu list               List installed packages in this project\n";
    std::cout << "    bantu search [term]      Search local registry (offline)\n";
    std::cout << "    bantu publish <dir>      Add a folder to local registry\n";
    std::cout << "    bantu publish <dir> --as <n>   Publish under a different name\n";
    std::cout << "\n";
    std::cout << "  v1.2.2 RELEASE COMMANDS:\n";
    std::cout << "    bantu build-windows [opts] [entry.b]  Generate NSIS installer (.exe)\n";
    std::cout << "      --name <Name>      Application name (default: BantuApp)\n";
    std::cout << "      --version <x.y.z>  Application version (default: 1.0.0)\n";
    std::cout << "    bantu bench              Run built-in micro-benchmark suite\n";
    std::cout << "\n";
    std::cout << "  DESKTOP APP INSTALLER (cross-platform, offline-ready):\n";
    std::cout << "    bantu installer                       Auto-detect host platform\n";
    std::cout << "    bantu installer [entry.b] --platform linux    Generate .deb + .desktop\n";
    std::cout << "    bantu installer [entry.b] --platform windows  Generate NSIS .exe installer\n";
    std::cout << "    bantu installer [entry.b] --platform macos    Generate .app bundle\n";
    std::cout << "    bantu installer [entry.b] --platform android  Generate Android Studio project + APK\n";
    std::cout << "      --name <Name>      Application name (default: from bantu.json or folder)\n";
    std::cout << "      --version <x.y.z>  Application version (default: 1.0.0)\n";
    std::cout << "      --icon <path>      Path to icon (.png/.ico/.icns) for shortcuts\n";
    std::cout << "      --bundle-bantu     Embed the bantu interpreter inside the installer\n";
    std::cout << "                          (default: on, so apps run on machines without Bantu)\n";
    std::cout << "\n";
    std::cout << "  GLOBAL FLAGS:\n";
    std::cout << "    bantu --help             Show this help\n";
    std::cout << "    bantu --version          Show version\n";
    std::cout << "    bantu --quiet run app.b  Run quietly (no [INCLUDE]/[Executed in..] logs)\n";
    std::cout << "    bantu -q run app.b       Alias of --quiet\n";
    std::cout << "\n";
    std::cout << "  EXAMPLES:\n";
    std::cout << "    bantu setup              # one-time: put bantu on PATH\n";
    std::cout << "    bantu init myproject     # new CLI project\n";
    std::cout << "    bantu init --web shop    # new Sua web app 'shop'\n";
    std::cout << "    cd myproject && bantu run\n";
    std::cout << "    bantu add math-utils     # install math-utils package\n";
    std::cout << "    bantu install            # reinstall all deps from bantu.json\n";
    std::cout << "    bantu installer app.b --platform linux   # build .deb installer\n";
    std::cout << "\n";
    std::cout << "  KEYWORDS:\n";
    std::cout << "    def, if, else, while, for, each..in,\n";
    std::cout << "    return, print, read, db, fetch, await,\n";
    std::cout << "    try, catch, break, continue, switch,\n";
    std::cout << "    const, private, public, from,\n";
    std::cout << "    class, extends, new, create, delete,\n";
    std::cout << "    update, calc, import, export\n";
    std::cout << "\n";
    std::cout << "  REAL-TIME (sua framework):\n";
    std::cout << "    channel, signal, stun, stream,\n";
    std::cout << "    broadcast, relay, connect, peer,\n";
    std::cout << "    ice, candidate, room, offer, answer\n";
    std::cout << "\n";
    std::cout << "  TYPES:\n";
    std::cout << "    number, string, bool, list, dict, any, func\n";
    std::cout << "\n";
    std::cout << "  VALUES:\n";
    std::cout << "    true, false, null\n";
    std::cout << "\n";
}

// ─── Run Code ─────────────────────────────────────────────────────────

// v1.2.2: global quiet flag — flips evaluator into quiet mode and suppresses
// the "Running:" / "[Executed in ...]" chit-chat. Set by --quiet on the CLI.
static bool g_quietMode = false;
// Compile gate: when true, `run`/`build` lint the source and refuse to execute
// on error-severity diagnostics. Disabled with `--no-lint`.
static bool g_lintGate = true;

void runCode(const std::string& source, const std::string& filename = "<repl>") {
    auto start = std::chrono::high_resolution_clock::now();

    Lexer lexer(source);
    auto tokens = lexer.tokenize();

    Parser parser(std::move(tokens));
    auto ast = parser.parse();

    // Compile gate: lint the parsed program (syntax errors from recovery + the
    // static checks) and refuse to execute if ANY error-severity diagnostic is
    // present. Warnings are printed but do not block. Fix-then-run, like a real
    // toolchain. (`--no-lint` is honored by the caller for an escape hatch.)
    if (g_lintGate) {
        std::vector<LintDiag> diags;
        for (const auto& e : parser.errors()) diags.push_back({e.line, e.col, "error", e.message});
        { std::set<std::string> consts; for (auto& node : ast) bantuLintWalk(node, consts, diags); }
        int errCount = 0;
        for (const auto& d : diags) {
            std::cerr << "  [" << d.severity << "] line " << d.line << ", col " << d.col << ": " << d.message << "\n";
            if (d.severity == "error") errCount++;
        }
        if (errCount > 0) {
            throw BantuError("compilation blocked by " + std::to_string(errCount)
                             + " error(s) — fix them and re-run", "LINT");
        }
    }

    Evaluator evaluator;
    evaluator.setQuiet(g_quietMode);
    // v1.2.1: tell the evaluator which file it's running so that
    // `include "./..."` statements resolve relative to it.
    if (filename != "<repl>") {
        evaluator.setEntryPoint(filename);
    }
    evaluator.evaluate(ast);

    auto end = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    if (filename != "<repl>" && !g_quietMode) {
        std::cout << "\n  [Executed in " << us.count() << " us]\n";
    }
}

// ─── REPL ─────────────────────────────────────────────────────────────

void runRepl() {
    printBanner();

    std::string line;
    std::string buffer;

    while (true) {
        if (buffer.empty()) {
            std::cout << "bantu> ";
        } else {
            std::cout << "  ...> ";
        }

        if (!std::getline(std::cin, line)) break;

        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        if (line == "exit" || line == "quit") {
            std::cout << "  Goodbye! Kwaheri!\n";
            break;
        }
        if (line == "help") {
            printHelp();
            continue;
        }
        if (line == "clear") {
            buffer.clear();
            continue;
        }

        buffer += line + "\n";

        int braceCount = 0;
        for (char c : buffer) {
            if (c == '{') braceCount++;
            if (c == '}') braceCount--;
        }

        if (braceCount <= 0) {
            try {
                runCode(buffer, "<repl>");
            } catch (const std::exception& e) {
                std::cerr << "  [ERROR] " << e.what() << "\n";
            }
            buffer.clear();
        }
    }
}

// ─── bantu lint ───────────────────────────────────────────────────────
// Static analysis without executing. `--json` emits machine-readable
// diagnostics for the VS Code extension; otherwise a human summary.
// Exit code is non-zero when any error-severity diagnostic is present.
int cmdLint(int argc, char* argv[]) {
    std::string path;
    bool json = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") json = true;
        else if (a == "--help" || a == "-h") {
            std::cout << "  Usage: bantu lint <file.b> [--json]\n";
            return 0;
        } else if (!a.empty() && a[0] != '-') {
            path = a;
        }
    }
    if (path.empty()) {
        std::cerr << "  Usage: bantu lint <file.b> [--json]\n";
        return 1;
    }
    if (!fileExists(path)) {
        std::cerr << "  [ERROR] File not found: " << path << "\n";
        return 1;
    }

    std::string source = readFile(path);
    auto diags = bantuLintSource(source);

    if (json) {
        std::cout << "[";
        for (size_t i = 0; i < diags.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << "{\"line\":" << diags[i].line
                      << ",\"col\":" << diags[i].col
                      << ",\"severity\":\"" << diags[i].severity << "\""
                      << ",\"message\":\"" << bantuJsonEscape(diags[i].message) << "\"}";
        }
        std::cout << "]\n";
    } else {
        int errs = 0, warns = 0;
        for (auto& d : diags) {
            (d.severity == "error" ? errs : warns)++;
            std::cerr << "  " << d.severity << " (line " << d.line << ", col " << d.col << "): "
                      << d.message << "\n";
        }
        if (diags.empty()) std::cout << "  ✓ " << path << " — no issues found\n";
        else std::cout << "  " << errs << " error(s), " << warns << " warning(s)\n";
    }

    for (auto& d : diags) if (d.severity == "error") return 1;
    return 0;
}

// ─── bantu run ────────────────────────────────────────────────────────

int cmdRun(const std::string& filePath) {
    std::string path = filePath;

    // If no file specified, look for main.b
    if (path.empty()) {
        if (fileExists("main.b")) {
            path = "main.b";
        } else if (fileExists("app.b")) {
            path = "app.b";
        } else if (fileExists("index.b")) {
            path = "index.b";
        } else {
            std::cerr << "  [ERROR] No file specified and no main.b, app.b, or index.b found.\n";
            std::cerr << "  Usage: bantu run <file.b> [--quiet]\n";
            return 1;
        }
    }

    if (!fileExists(path)) {
        std::cerr << "  [ERROR] File not found: " << path << "\n";
        return 1;
    }

    if (!g_quietMode) {
        std::cout << "  Running: " << path << "\n";
        std::cout << "  ────────────────────────────\n";
    }

    std::string source = readFile(path);
    if (source.empty()) return 1;

    try {
        runCode(source, path);
    } catch (const std::exception& e) {
        std::cerr << "  [FATAL] " << e.what() << "\n";
        return 1;
    }

    return 0;
}

// ─── bantu build ──────────────────────────────────────────────────────

std::string base64Encode(const std::string& input) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    int val = 0, valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            result.push_back(chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) result.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (result.size() % 4) result.push_back('=');
    return result;
}

int cmdBuild(const std::string& filePath) {
    std::string path = filePath;

    // If no file specified, look for main.b
    if (path.empty()) {
        if (fileExists("main.b")) {
            path = "main.b";
        } else if (fileExists("app.b")) {
            path = "app.b";
        } else if (fileExists("index.b")) {
            path = "index.b";
        } else {
            std::cerr << "  [ERROR] No file specified and no main.b, app.b, or index.b found.\n";
            std::cerr << "  Usage: bantu build <file.b>\n";
            return 1;
        }
    }

    if (!fileExists(path)) {
        std::cerr << "  [ERROR] File not found: " << path << "\n";
        return 1;
    }

    std::string basename = getBasename(path);
    std::string outputPath = "./" + basename;

    std::cout << "  Building: " << path << " -> " << outputPath << "\n";

    // Read the source file
    std::string source = readFile(path);
    if (source.empty()) return 1;

    // Encode source as base64 to avoid shell escaping issues
    std::string encoded = base64Encode(source);

    // Create a self-contained shell script
    std::ostringstream script;
    script << "#!/bin/bash\n";
    script << "# Bantu Compiled Executable - " << basename << "\n";
    script << "# Generated by bantu build v" << BANTU_VERSION << "\n";
    script << "# Source: " << path << "\n";
    script << "\n";

    // Find the bantu interpreter
    script << "BANTU_BIN=\"\"\n";
    script << "if command -v bantu &> /dev/null; then\n";
    script << "    BANTU_BIN=\"bantu\"\n";
    script << "elif [ -f \"$HOME/.local/bin/bantu\" ]; then\n";
    script << "    BANTU_BIN=\"$HOME/.local/bin/bantu\"\n";
    script << "elif [ -f \"/usr/local/bin/bantu\" ]; then\n";
    script << "    BANTU_BIN=\"/usr/local/bin/bantu\"\n";
    script << "else\n";
    script << "    echo \"[ERROR] Bantu interpreter not found. Install it first.\"\n";
    script << "    echo \"  https://github.com/AsseySilivestir/swahiliscript\"\n";
    script << "    exit 1\n";
    script << "fi\n";
    script << "\n";

    // Embed base64-encoded source and decode at runtime
    // Use temp file to avoid REPL mode (bantu with no args starts REPL)
    script << "BANTU_TMPFILE=$(mktemp /tmp/bantu_XXXXXX.b)\n";
    script << "echo '" << encoded << "' | base64 -d > \"$BANTU_TMPFILE\"\n";
    script << "\"$BANTU_BIN\" run \"$BANTU_TMPFILE\"\n";
    script << "rm -f \"$BANTU_TMPFILE\"\n";

    if (writeFile(outputPath, script.str())) {
        chmod(outputPath.c_str(), 0755);

        std::cout << "  ────────────────────────────\n";
        std::cout << "  Build successful: " << outputPath << "\n";
        std::cout << "  Run with: ." << outputPath.substr(1) << "\n";
        return 0;
    }

    return 1;
}

// ─── bantu init / bantu new ──────────────────────────────────────────

int cmdInit(const std::string& projectName) {
    if (projectName.empty()) {
        std::cerr << "  [ERROR] Project name required.\n";
        std::cerr << "  Usage: bantu init <project-name>\n";
        return 1;
    }

    // Check if directory already exists
    if (fileExists(projectName)) {
        std::cerr << "  [ERROR] Directory already exists: " << projectName << "\n";
        return 1;
    }

    std::cout << "  Creating project: " << projectName << "\n";

    // Create project directory.
    // Windows `mkdir` takes 1 arg, POSIX `mkdir` takes 2 (path, mode).
#ifdef _WIN32
    #define BANTU_MKDIR(p) mkdir(p)
#else
    #define BANTU_MKDIR(p) mkdir(p, 0755)
#endif
    BANTU_MKDIR(projectName.c_str());
    BANTU_MKDIR((projectName + "/src").c_str());

    // Create main.b
    std::string mainContent =
        "// " + projectName + " - Bantu Project\n"
        "// Created with bantu init v" + BANTU_VERSION + "\n\n"
        "print \"Hello from " + projectName + "!\";\n"
        "print \"Welcome to Bantu Programming Language!\";\n\n"
        "// Define your functions here\n"
        "def greet($name) {\n"
        "    print \"Hello, \" + $name + \"!\";\n"
        "    return true;\n"
        "}\n\n"
        "greet(\"World\");\n";

    writeFile(projectName + "/main.b", mainContent);

    // Create a README
    std::string readmeContent =
        "# " + projectName + "\n\n"
        "A Bantu programming language project.\n\n"
        "## Getting Started\n\n"
        "```bash\n"
        "bantu run main.b\n"
        "```\n\n"
        "## Build\n\n"
        "```bash\n"
        "bantu build main.b\n"
        "```\n\n"
        "## Project Structure\n\n"
        "```\n" +
        projectName + "/\n"
        "  main.b      # Entry point\n"
        "  src/        # Source modules\n"
        "```\n\n"
        "## Learn More\n\n"
        "- [Bantu Language](https://github.com/AsseySilivestir/swahiliscript)\n"
        "- Keywords: def, if, else, while, for, each..in, print, read, db, fetch, await, try, catch\n"
        "- Types: number, string, bool, list, dict, any, func\n"
        "- Web Framework: sua.get, sua.post, sua.start\n";

    writeFile(projectName + "/README.md", readmeContent);

    // Create bantu.json project config
    std::string configContent =
        "{\n"
        "  \"name\": \"" + projectName + "\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"entry\": \"main.b\",\n"
        "  \"template\": \"cli\",\n"
        "  \"language\": \"bantu\",\n"
        "  \"bantuVersion\": \"" + BANTU_VERSION + "\",\n"
        "  \"dependencies\": {}\n"
        "}\n";

    writeFile(projectName + "/bantu.json", configContent);

    // Create an example module in src/
    std::string moduleContent =
        "// utils.b - Utility functions for " + projectName + "\n\n"
        "def add($a, $b) {\n"
        "    return $a + $b;\n"
        "}\n\n"
        "def subtract($a, $b) {\n"
        "    return $a - $b;\n"
        "}\n";

    writeFile(projectName + "/src/utils.b", moduleContent);

    std::cout << "  ────────────────────────────\n";
    std::cout << "  Project created: " << projectName << "/\n";
    std::cout << "    " << projectName << "/main.b        (entry point)\n";
    std::cout << "    " << projectName << "/src/utils.b    (example module)\n";
    std::cout << "    " << projectName << "/bantu.json     (project config)\n";
    std::cout << "    " << projectName << "/README.md      (documentation)\n";
    std::cout << "\n";
    std::cout << "  Next steps:\n";
    std::cout << "    cd " << projectName << "\n";
    std::cout << "    bantu run main.b\n";
    std::cout << "    bantu add math-utils      # add a package\n";
    std::cout << "    bantu search              # browse local registry\n";

    return 0;
}

// ─── bantu init --web / bantu new --web ──────────────────────────────
// Scaffolds a Sua web app: server.b + frontend + launchers + Dockerfile
// + render.yaml + README. Like Spring Initializer for web apps.

int cmdInitWeb(const std::string& projectName) {
    if (projectName.empty()) {
        std::cerr << "  [ERROR] Project name required.\n";
        std::cerr << "  Usage: bantu init --web <project-name>\n";
        return 1;
    }

    // Reject names that would make bad directory names or bad SQL identifiers.
    if (projectName.find_first_of("/\\:*?\"<>|") != std::string::npos
        || projectName.find(' ') != std::string::npos) {
        std::cerr << "  [ERROR] Invalid project name: '" << projectName << "'\n";
        std::cerr << "  Names cannot contain spaces or any of: / \\ : * ? \" < > |\n";
        return 1;
    }

    if (fileExists(projectName)) {
        std::cerr << "  [ERROR] Directory already exists: " << projectName << "\n";
        return 1;
    }

    std::cout << "  Creating Sua web app: " << projectName << "\n";

#ifdef _WIN32
    #define BANTU_MKDIR_WEB(p) mkdir(p)
#else
    #define BANTU_MKDIR_WEB(p) mkdir(p, 0755)
#endif
    BANTU_MKDIR_WEB(projectName.c_str());
    BANTU_MKDIR_WEB((projectName + "/public").c_str());
    BANTU_MKDIR_WEB((projectName + "/public/css").c_str());
    BANTU_MKDIR_WEB((projectName + "/public/js").c_str());
#undef BANTU_MKDIR_WEB

    // ─── Write all files ────────────────────────────────────────────
    writeFile(projectName + "/main.b",              bantu_templates::main_b(projectName));
    writeFile(projectName + "/public/index.html",   bantu_templates::index_html(projectName));
    writeFile(projectName + "/public/css/style.css",bantu_templates::style_css());
    writeFile(projectName + "/public/js/app.js",    bantu_templates::app_js());
    writeFile(projectName + "/start.sh",            bantu_templates::start_sh());
    writeFile(projectName + "/start.bat",           bantu_templates::start_bat());
    writeFile(projectName + "/Dockerfile",          bantu_templates::dockerfile());
    writeFile(projectName + "/render.yaml",         bantu_templates::render_yaml(projectName));
    writeFile(projectName + "/.gitignore",          bantu_templates::gitignore());
    writeFile(projectName + "/README.md",           bantu_templates::readme_md(projectName));
    writeFile(projectName + "/bantu.json",          bantu_templates::bantu_json(projectName, BANTU_VERSION));

    // Make start.sh executable on POSIX
#ifndef _WIN32
    chmod((projectName + "/start.sh").c_str(), 0755);
#endif

    std::cout << "  ────────────────────────────\n";
    std::cout << "  Project created: " << projectName << "/\n";
    std::cout << "    " << projectName << "/main.b                ← backend (Sua + SQLite)\n";
    std::cout << "    " << projectName << "/public/                ← frontend (HTML/CSS/JS)\n";
    std::cout << "    " << projectName << "/start.sh / start.bat   ← launchers\n";
    std::cout << "    " << projectName << "/Dockerfile             ← Render-ready\n";
    std::cout << "    " << projectName << "/render.yaml            ← Render blueprint\n";
    std::cout << "    " << projectName << "/README.md              ← docs\n";
    std::cout << "\n";
    std::cout << "  Next steps:\n";
    std::cout << "    cd " << projectName << "\n";
    std::cout << "    ./start.sh          # Linux/Mac\n";
    std::cout << "    start.bat           # Windows\n";
    std::cout << "    bantu run main.b    # anywhere with bantu on PATH\n";
    std::cout << "\n";
    std::cout << "  Then open http://localhost:8080\n";

    return 0;
}

// ─── bantu relay ────────────────────────────────────────────────────

int cmdRelay(int port) {
    if (port <= 0) port = 3478;

    std::cout << "\n";
    std::cout << "  ╔═══════════════════════════════════════════════╗\n";
    std::cout << "  ║   Bantu WebRTC Relay Server v" << BANTU_VERSION << "           ║\n";
    std::cout << "  ║   STUN/TURN NAT Traversal + Signaling        ║\n";
    std::cout << "  ╚═══════════════════════════════════════════════╝\n";
    std::cout << "\n";
    std::cout << "  [RELAY] STUN server:    UDP port " << port << "\n";
    std::cout << "  [RELAY] TURN relay:     UDP port " << (port + 1) << "\n";
    std::cout << "  [RELAY] Relay ports:    50000-60000\n";
    std::cout << "  [RELAY] Signaling:      SDP offer/answer\n";
    std::cout << "  [RELAY] ICE:            Candidate gathering\n";
    std::cout << "  [RELAY] WebSocket:      Upgrade support\n";
    std::cout << "\n";
    std::cout << "  Built-in NAT traversal means your Bantu apps\n";
    std::cout << "  can connect directly peer-to-peer, even behind\n";
    std::cout << "  firewalls and symmetric NATs!\n";
    std::cout << "\n";
    std::cout << "  Press Ctrl+C to stop the relay server.\n";
    std::cout << "\n";

    // Start the STUN server
    StunServer stunServer(port);
    if (!stunServer.start()) {
        std::cerr << "  [ERROR] Failed to start STUN server on port " << port << "\n";
        return 1;
    }

    // Start the TURN relay server
    TurnServer turnServer(port + 1);
    if (!turnServer.start()) {
        std::cerr << "  [ERROR] Failed to start TURN server on port " << (port + 1) << "\n";
        return 1;
    }

    // Keep the main thread alive
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}

// ─── bantu setup / bantu uninstall / bantu doctor ───────────────────
// PATH integration. These just delegate to package_manager.hpp.

int cmdSetup(int argc, char* argv[]) {
    bool systemWide = false;
    bool seed = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--system" || a == "-s") systemWide = true;
        else if (a == "--seed")           seed = true;
        else if (a == "--user" || a == "-u") systemWide = false;
        else if (a == "--help" || a == "-h") {
            std::cout << "  Usage:\n";
            std::cout << "    bantu setup              Add bantu to PATH (user install)\n";
            std::cout << "    bantu setup --system     Add bantu to PATH (system-wide; needs sudo)\n";
            std::cout << "    bantu setup --seed       Also seed local registry with starter packages\n";
            std::cout << "    bantu uninstall          Remove bantu from PATH\n";
            return 0;
        }
    }

    std::cout << "\n";
    std::cout << "  ╔══════════════════════════════════════════╗\n";
    std::cout << "  ║   Bantu PATH Integration v" << BANTU_VERSION << "           ║\n";
    std::cout << "  ╚══════════════════════════════════════════╝\n\n";

    if (!bantu_pkg::installToPath(systemWide)) {
        std::cerr << "\n  [FAIL] PATH integration failed.\n";
        return 1;
    }

    if (seed) {
        std::cout << "\n  ── Seeding local registry with starter packages ──\n";
        int n = bantu_pkg::seedStarterRegistry();
        std::cout << "  [OK] Seeded " << n << " starter packages into "
                  << bantu_pkg::getRegistryDir() << "\n";
        std::cout << "       Run 'bantu search' to see them.\n";
    }

    std::cout << "\n  ────────────────────────────\n";
    std::cout << "  Done. Verify with:\n";
    std::cout << "    bantu --version\n";
    std::cout << "    bantu doctor\n";
    std::cout << "\n";
    return 0;
}

int cmdUninstall() {
    std::cout << "\n  Removing bantu from PATH...\n\n";
    if (!bantu_pkg::removeFromPath()) {
        std::cerr << "  [WARN] Could not fully clean PATH entries.\n";
    }
    std::cout << "\n  [OK] Done. The bantu binary itself was NOT deleted from disk.\n";
    std::cout << "       Manually remove " << bantu_pkg::getUserInstallBinDir()
              << " if you want a full uninstall.\n";
    return 0;
}

int cmdDoctor() {
    return bantu_pkg::runDoctor();
}

// ─── bantu install / add / remove / update / list / search / publish ─
// Offline package manager. All delegate to package_manager.hpp.

int cmdInstall() {
    std::cout << "\n  Installing all dependencies from bantu.json...\n\n";
    int n = bantu_pkg::installAllFromManifest();
    return (n < 0) ? 1 : 0;
}

int cmdAdd(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "  [ERROR] Package name required.\n";
        std::cerr << "  Usage: bantu add <pkg-name>\n";
        std::cerr << "         bantu add <pkg-name>@<version>\n";
        return 1;
    }
    std::cout << "\n";
    bool any = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            std::cout << "  Usage: bantu add <pkg-name>[@<version>]\n";
            return 0;
        }
        if (!a.empty() && a[0] == '-') continue;
        std::cout << "  ── Adding " << a << " ──\n";
        if (bantu_pkg::addDependency(a)) any = true;
    }
    if (!any) {
        std::cerr << "  [ERROR] No package name provided.\n";
        return 1;
    }
    return 0;
}

int cmdRemove(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "  [ERROR] Package name required.\n";
        std::cerr << "  Usage: bantu remove <pkg-name>\n";
        return 1;
    }
    std::cout << "\n";
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (!a.empty() && a[0] == '-') continue;
        bantu_pkg::removeDependency(a);
    }
    return 0;
}

int cmdUpdate(int argc, char* argv[]) {
    std::cout << "\n";
    if (argc < 3) {
        std::cout << "  Updating all dependencies to latest...\n\n";
        int n = bantu_pkg::updateAll();
        return (n < 0) ? 1 : 0;
    }
    int rc = 0;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (!a.empty() && a[0] == '-') continue;
        std::cout << "  ── Updating " << a << " ──\n";
        if (!bantu_pkg::updatePackage(a)) rc = 1;
    }
    return rc;
}

int cmdList() {
    bantu_pkg::listInstalled();
    return 0;
}

int cmdSearch(int argc, char* argv[]) {
    std::string term = (argc >= 3) ? std::string(argv[2]) : "";
    bantu_pkg::searchRegistry(term);
    return 0;
}

int cmdPublish(int argc, char* argv[]) {
    std::string dir;
    std::string asName;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--as" && i + 1 < argc) {
            asName = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::cout << "  Usage: bantu publish <folder> [--as <name>]\n";
            std::cout << "  The folder must contain package.json + .b source files.\n";
            return 0;
        } else if (!a.empty() && a[0] != '-') {
            dir = a;
        }
    }
    if (dir.empty()) {
        std::cerr << "  [ERROR] Folder required.\n";
        std::cerr << "  Usage: bantu publish <folder> [--as <name>]\n";
        return 1;
    }
    std::cout << "\n  Publishing " << dir << "...\n\n";
    return bantu_pkg::publishPackage(dir, asName) ? 0 : 1;
}

// ─── bantu build-windows ──────────────────────────────────────────────
// v1.2.1: Generate a Windows installer (.exe via NSIS) for the current
// Bantu project. Requires makensis (NSIS) on PATH. The installer bundles:
//   - bantu.exe (the interpreter)
//   - the project's .b files
//   - bantu.json manifest
//   - a launcher .bat that runs `bantu run main.b`
// On success, prints the path to the generated installer.
//
// Usage:
//   bantu build-windows                 # uses main.b / app.b / index.b
//   bantu build-windows <file.b>        # uses <file.b> as entry point
//   bantu build-windows --name "MyApp"  # set installer app name
//   bantu build-windows --version 1.0.0 # set installer version
//
// The generated installer is at ./dist/<Name>-Setup-<Version>.exe
int cmdBuildWindows(int argc, char* argv[]) {
    std::string entryFile;
    std::string appName = "BantuApp";
    std::string appVersion = "1.0.0";

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--name" && i + 1 < argc) {
            appName = argv[++i];
        } else if (a == "--version" && i + 1 < argc) {
            appVersion = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::cout << "  Usage: bantu build-windows [options] [entry-file.b]\n";
            std::cout << "  Options:\n";
            std::cout << "    --name <Name>      Application name (default: BantuApp)\n";
            std::cout << "    --version <x.y.z>  Application version (default: 1.0.0)\n";
            std::cout << "\n  Requires NSIS (makensis) on PATH.\n";
            std::cout << "  Output: ./dist/" << appName << "-Setup-" << appVersion << ".exe\n";
            return 0;
        } else if (!a.empty() && a[0] != '-') {
            entryFile = a;
        }
    }

    // Resolve entry file
    if (entryFile.empty()) {
        if (fileExists("main.b")) entryFile = "main.b";
        else if (fileExists("app.b")) entryFile = "app.b";
        else if (fileExists("index.b")) entryFile = "index.b";
        else {
            std::cerr << "  [ERROR] No entry file found (looked for main.b, app.b, index.b)\n";
            return 1;
        }
    }
    if (!fileExists(entryFile)) {
        std::cerr << "  [ERROR] Entry file not found: " << entryFile << "\n";
        return 1;
    }

    // Create dist/ directory
    if (system("mkdir -p dist") != 0) {
        std::cerr << "  [ERROR] Failed to create dist/ directory\n";
        return 1;
    }

    // Write NSIS installer script
    std::string nsiPath = "dist/installer.nsi";
    std::ofstream nsi(nsiPath);
    if (!nsi.is_open()) {
        std::cerr << "  [ERROR] Cannot write " << nsiPath << "\n";
        return 1;
    }

    nsi << "; Generated by bantu build-windows v" << BANTU_VERSION << "\n";
    nsi << "!define APPNAME \"" << appName << "\"\n";
    nsi << "!define APPVERSION \"" << appVersion << "\"\n";
    nsi << "!define APPPUBLISHER \"Bantu\"\n";
    nsi << "\n";
    nsi << "Name \"${APPNAME} ${APPVERSION}\"\n";
    nsi << "OutFile \"${APPNAME}-Setup-${APPVERSION}.exe\"\n";
    nsi << "InstallDir \"$LOCALAPPDATA\\${APPNAME}\"\n";
    nsi << "RequestExecutionLevel user\n";
    nsi << "\n";
    nsi << "Page directory\n";
    nsi << "Page instfiles\n";
    nsi << "\n";
    nsi << "Section \"${APPNAME}\"\n";
    nsi << "  SetOutPath \"$INSTDIR\"\n";
    nsi << "  ; Bundle the Bantu interpreter (must be on PATH or co-located)\n";
    nsi << "  File /nonfatal \"bantu.exe\"\n";
    nsi << "  File /nonfatal \"bantu\"\n";
    nsi << "  ; Bundle the project source\n";
    nsi << "  File \"*.b\"\n";
    nsi << "  File /nonfatal \"bantu.json\"\n";
    nsi << "  File /nonfatal \"*.bat\"\n";
    nsi << "\n";
    nsi << "  ; Create launcher batch file\n";
    nsi << "  FileOpen $0 \"$INSTDIR\\run-${APPNAME}.bat\" w\n";
    nsi << "  FileWrite $0 '@echo off$\r$\n'\n";
    nsi << "  FileWrite $0 'cd /d \"%~dp0\"$\r$\n'\n";
    nsi << "  FileWrite $0 'bantu.exe run " << entryFile << "$\r$\n'\n";
    nsi << "  FileWrite $0 'pause$\r$\n'\n";
    nsi << "  FileClose $0\n";
    nsi << "\n";
    nsi << "  ; Create Start Menu shortcut\n";
    nsi << "  CreateDirectory \"$SMPROGRAMS\\${APPNAME}\"\n";
    nsi << "  CreateShortcut \"$SMPROGRAMS\\${APPNAME}\\${APPNAME}.lnk\" \\\n";
    nsi << "    \"$INSTDIR\\run-${APPNAME}.bat\" \"\" \"$INSTDIR\\bantu.exe\"\n";
    nsi << "\n";
    nsi << "  ; Uninstaller\n";
    nsi << "  WriteUninstaller \"$INSTDIR\\uninstall-${APPNAME}.exe\"\n";
    nsi << "  CreateShortcut \"$SMPROGRAMS\\${APPNAME}\\Uninstall.lnk\" \\\n";
    nsi << "    \"$INSTDIR\\uninstall-${APPNAME}.exe\"\n";
    nsi << "SectionEnd\n";
    nsi << "\n";
    nsi << "Section \"Uninstall\"\n";
    nsi << "  Delete \"$INSTDIR\\*.*\"\n";
    nsi << "  RMDir \"$INSTDIR\"\n";
    nsi << "  Delete \"$SMPROGRAMS\\${APPNAME}\\*.*\"\n";
    nsi << "  RMDir \"$SMPROGRAMS\\${APPNAME}\"\n";
    nsi << "SectionEnd\n";
    nsi.close();

    std::cout << "  [BUILD-WINDOWS] NSIS script written: " << nsiPath << "\n";
    std::cout << "  [BUILD-WINDOWS] App: " << appName << " v" << appVersion << "\n";
    std::cout << "  [BUILD-WINDOWS] Entry: " << entryFile << "\n";

    // Try to invoke makensis
    int rc = system("makensis dist/installer.nsi");
    if (rc != 0) {
        std::cerr << "\n  [WARN] makensis not found or failed (rc=" << rc << ")\n";
        std::cerr << "  Install NSIS to generate the .exe:\n";
        std::cerr << "    Linux:  sudo apt install nsis\n";
        std::cerr << "    macOS:  brew install nsis\n";
        std::cerr << "    Windows: https://nsis.sourceforge.io/Download\n";
        std::cerr << "\n  Then run:\n";
        std::cerr << "    makensis dist/installer.nsi\n";
        std::cerr << "\n  The script is ready at " << nsiPath << "\n";
        return 1;
    }

    std::cout << "\n  [BUILD-WINDOWS] Installer generated:\n";
    std::cout << "    dist/" << appName << "-Setup-" << appVersion << ".exe\n";
    return 0;
}

// ─── bantu installer ─────────────────────────────────────────────────
// Cross-platform desktop installer generator.
//
//   bantu installer                        # auto-detect host platform
//   bantu installer app.b --platform linux
//   bantu installer app.b --platform windows
//   bantu installer app.b --platform macos
//
// Output goes to ./dist/. Each platform produces a runnable, offline-
// ready installer that bundles the bantu interpreter (so end users
// don't need Bantu installed) plus the project's .b source files and
// a launcher.
//
// Linux:   dist/<name>_<version>_amd64.deb  (dpkg-deb)
//          Also writes a .desktop file so the app appears in the
//          launcher menu.
//
// Windows: dist/<name>-Setup-<version>.exe  (NSIS .nsi)
//          If makensis is on PATH the .exe is built; otherwise the
//          .nsi is left for the user to compile.
//
// macOS:   dist/<Name>.app bundle
//          Contents/{MacOS,Resources} layout with Info.plist and a
//          launcher script. The .app can be opened with `open` or
//          double-clicked in Finder.

// Trim leading/trailing whitespace
static std::string trimStr(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Find a JSON string field by key. Very small JSON parser — enough
// for bantu.json which the scaffolder writes itself.
static std::string jsonField(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return "";
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

// Locate the bantu interpreter to embed in the installer.
// Search order: $BANTU_BIN env, alongside current argv[0], PATH.
static std::string findSelfBinary() {
    const char* envBin = std::getenv("BANTU_BIN");
    if (envBin && *envBin && fileExists(envBin)) {
        return std::string(envBin);
    }
#ifdef _WIN32
    // Windows: GetModuleFileNameA returns the absolute path of the running .exe
    char buf[4096];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf) - 1);
    if (n > 0 && n < sizeof(buf)) {
        buf[n] = 0;
        return std::string(buf);
    }
    return "";
#elif defined(__APPLE__)
    // macOS: use _NSGetExecutablePath (from <mach-o/dyld.h>)
    // Returns the absolute path of the current process's executable,
    // even if invoked via a symlink or relative path.
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        // Resolve any symlinks in the path (e.g. /usr/local/bin/bantu → real location)
        char resolved[4096];
        if (realpath(buf, resolved) != nullptr) {
            return std::string(resolved);
        }
        return std::string(buf);
    }
    // Buffer too small — try with a heap allocation
    if (size > 0 && size < 1'000'000) {
        char* heapBuf = new char[size + 1];
        if (_NSGetExecutablePath(heapBuf, &size) == 0) {
            std::string result(heapBuf);
            delete[] heapBuf;
            char resolved[4096];
            if (realpath(result.c_str(), resolved) != nullptr) {
                return std::string(resolved);
            }
            return result;
        }
        delete[] heapBuf;
    }
    // Fall back to PATH lookup
    int rc = system("command -v bantu > /tmp/.bantu_bin_path 2>/dev/null");
    (void)rc;
    std::ifstream f("/tmp/.bantu_bin_path");
    if (f.is_open()) {
        std::string line;
        std::getline(f, line);
        if (!line.empty() && fileExists(line)) return line;
    }
    return "";
#else
    // Linux: /proc/self/exe is a symlink to the actual executable
    if (fileExists("/proc/self/exe")) {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = 0; return std::string(buf); }
    }
    // Fall back to PATH lookup
    int rc = system("command -v bantu > /tmp/.bantu_bin_path 2>/dev/null");
    (void)rc;
    std::ifstream f("/tmp/.bantu_bin_path");
    if (f.is_open()) {
        std::string line;
        std::getline(f, line);
        if (!line.empty() && fileExists(line)) return line;
    }
    return "";
#endif
}

// ── Linux: build a .deb package ──
static int buildLinuxInstaller(
    const std::string& entryFile,
    const std::string& appName,
    const std::string& appVersion,
    const std::string& iconPath,
    bool bundleBantu)
{
    std::string distDir = "dist";
    if (system(("mkdir -p " + distDir).c_str()) != 0) {
        std::cerr << "  [ERROR] Cannot create dist/\n";
        return 1;
    }

    // Sanitize appName for package name (lowercase, no spaces).
    std::string pkgName = appName;
    for (auto& c : pkgName) {
        if (std::isalnum((unsigned char)c)) c = (char)std::tolower((unsigned char)c);
        else c = '_';
    }
    std::string pkgVer = appVersion;
    std::string stagingDir = "dist/" + pkgName + "_" + pkgVer + "_amd64_staging";
    std::string cleanCmd = "rm -rf '" + stagingDir + "'";
    system(cleanCmd.c_str());

    // Standard .deb layout
    std::string debRoot = stagingDir + "/usr";
    std::string binDir       = debRoot + "/bin";
    std::string libDir       = debRoot + "/lib/" + pkgName;
    std::string shareDir     = debRoot + "/share/" + pkgName;
    std::string appsDir      = debRoot + "/share/applications";
    std::string iconsDir     = debRoot + "/share/icons/hicolor/256x256/apps";

    system(("mkdir -p '" + binDir   + "'").c_str());
    system(("mkdir -p '" + libDir   + "'").c_str());
    system(("mkdir -p '" + shareDir + "'").c_str());
    system(("mkdir -p '" + appsDir  + "'").c_str());
    system(("mkdir -p '" + iconsDir + "'").c_str());

    // ── lib/<pkg>/: copy all .b files (entry first), bantu.json, public/
    system(("cp *.b '" + libDir + "/' 2>/dev/null").c_str());
    system(("cp bantu.json '" + libDir + "/' 2>/dev/null").c_str());
    system(("cp -r public '" + libDir + "/' 2>/dev/null").c_str());

    // ── Optional: bundle the bantu interpreter
    std::string bantuBinPath;
    if (bundleBantu) {
        bantuBinPath = findSelfBinary();
        if (!bantuBinPath.empty()) {
            system(("cp '" + bantuBinPath + "' '" + libDir + "/bantu' 2>/dev/null").c_str());
            system(("chmod +x '" + libDir + "/bantu'").c_str());
            std::cout << "  [INSTALLER] Bundled interpreter: " << bantuBinPath << "\n";
        } else {
            std::cerr << "  [WARN] Could not locate bantu binary to bundle.\n";
            std::cerr << "         Set $BANTU_BIN to point at it, or rerun without --bundle-bantu.\n";
        }
    }

    // ── bin/<pkg>: launcher script (this is what users run from terminal)
    std::string launcherPath = binDir + "/" + pkgName;
    std::ofstream launcher(launcherPath);
    if (!launcher.is_open()) {
        std::cerr << "  [ERROR] Cannot write launcher: " << launcherPath << "\n";
        return 1;
    }
    launcher << "#!/bin/bash\n";
    launcher << "# Generated by bantu installer v" << BANTU_VERSION << "\n";
    launcher << "# App: " << appName << " v" << appVersion << "\n";
    launcher << "set -e\n";
    launcher << "APP_LIB=\"/usr/lib/" << pkgName << "\"\n";
    launcher << "if [ ! -d \"$APP_LIB\" ]; then\n";
    launcher << "    APP_LIB=\"$HOME/.local/lib/" << pkgName << "\"\n";
    launcher << "fi\n";
    launcher << "if [ -x \"$APP_LIB/bantu\" ]; then\n";
    launcher << "    BANTU_BIN=\"$APP_LIB/bantu\"\n";
    launcher << "elif command -v bantu >/dev/null 2>&1; then\n";
    launcher << "    BANTU_BIN=\"bantu\"\n";
    launcher << "else\n";
    launcher << "    echo \"[ERROR] Bantu interpreter not found. Install Bantu or use --bundle-bantu.\" >&2\n";
    launcher << "    exit 1\n";
    launcher << "fi\n";
    launcher << "cd \"$APP_LIB\"\n";
    launcher << "exec \"$BANTU_BIN\" run " << entryFile << " \"$@\"\n";
    launcher.close();
    system(("chmod +x '" + launcherPath + "'").c_str());

    // ── share/applications/<pkg>.desktop — launcher menu entry
    std::string desktopPath = appsDir + "/" + pkgName + ".desktop";
    std::ofstream desktop(desktopPath);
    if (desktop.is_open()) {
        desktop << "[Desktop Entry]\n";
        desktop << "Version=" << appVersion << "\n";
        desktop << "Type=Application\n";
        desktop << "Name=" << appName << "\n";
        desktop << "Comment=" << appName << " (Bantu app)\n";
        desktop << "Exec=" << pkgName << "\n";
        if (!iconPath.empty() && fileExists(iconPath)) {
            // Copy the icon into the icons dir
            system(("cp '" + iconPath + "' '" + iconsDir + "/" + pkgName + ".png' 2>/dev/null").c_str());
            desktop << "Icon=" << pkgName << "\n";
        } else {
            desktop << "Icon=utilities-terminal\n";
        }
        desktop << "Terminal=true\n";
        desktop << "Categories=Development;Utility;\n";
        desktop.close();
    }

    // ── DEBIAN/control
    std::string debianDir = stagingDir + "/DEBIAN";
    system(("mkdir -p '" + debianDir + "'").c_str());
    std::ofstream ctrl(debianDir + "/control");
    if (!ctrl.is_open()) {
        std::cerr << "  [ERROR] Cannot write DEBIAN/control\n";
        return 1;
    }
    ctrl << "Package: " << pkgName << "\n";
    ctrl << "Version: " << appVersion << "\n";
    ctrl << "Architecture: amd64\n";
    ctrl << "Maintainer: Assey Silivestir <assey@bantu-lang.dev>\n";
    ctrl << "Section: utils\n";
    ctrl << "Priority: optional\n";
    ctrl << "Description: " << appName << " — Bantu desktop application\n";
    ctrl << " Bundled Bantu interpreter + .b source files. Runs offline.\n";
    ctrl << " Built with bantu installer v" << BANTU_VERSION << ".\n";
    ctrl.close();

    // ── DEBIAN/postinst — fix permissions after unpack
    std::ofstream postinst(debianDir + "/postinst");
    if (postinst.is_open()) {
        postinst << "#!/bin/bash\n";
        postinst << "set -e\n";
        postinst << "chmod +x /usr/bin/" << pkgName << "\n";
        postinst << "chmod -R +r /usr/lib/" << pkgName << " 2>/dev/null || true\n";
        postinst << "if [ -x /usr/lib/" << pkgName << "/bantu ]; then\n";
        postinst << "    chmod +x /usr/lib/" << pkgName << "/bantu\n";
        postinst << "fi\n";
        postinst << "exit 0\n";
        postinst.close();
        system(("chmod +x '" + debianDir + "/postinst'").c_str());
    }

    // ── DEBIAN/prerm — pre-remove (no-op, just placeholder)
    std::ofstream prerm(debianDir + "/prerm");
    if (prerm.is_open()) {
        prerm << "#!/bin/bash\nset -e\nexit 0\n";
        prerm.close();
        system(("chmod +x '" + debianDir + "/prerm'").c_str());
    }

    // ── Build the .deb
    std::string debName = pkgName + "_" + pkgVer + "_amd64.deb";
    std::string buildCmd = "dpkg-deb --build '" + stagingDir + "' 'dist/" + debName + "'";
    std::cout << "  [INSTALLER] Building .deb: " << debName << "\n";
    int rc = system(buildCmd.c_str());
    if (rc != 0) {
        std::cerr << "\n  [ERROR] dpkg-deb failed (rc=" << rc << ").\n";
        std::cerr << "  Staging dir left at: " << stagingDir << "\n";
        std::cerr << "  On non-Debian Linux, install 'dpkg-deb' or use the staging dir directly.\n";
        return 1;
    }

    std::cout << "\n  [INSTALLER] Linux installer ready:\n";
    std::cout << "    dist/" << debName << "\n";
    std::cout << "\n  Install on any Debian/Ubuntu machine:\n";
    std::cout << "    sudo dpkg -i dist/" << debName << "\n";
    std::cout << "    " << pkgName << "              # run from terminal\n";
    std::cout << "    # Or find '" << appName << "' in your application launcher.\n";
    std::cout << "\n  Uninstall:\n";
    std::cout << "    sudo dpkg -r " << pkgName << "\n";
    return 0;
}

// ── Windows: build an NSIS .exe installer ──
static int buildWindowsInstaller(
    const std::string& entryFile,
    const std::string& appName,
    const std::string& appVersion,
    const std::string& iconPath,
    bool bundleBantu)
{
    std::string distDir = "dist";
    if (system(("mkdir -p " + distDir).c_str()) != 0) {
        std::cerr << "  [ERROR] Cannot create dist/\n";
        return 1;
    }

    std::string nsiPath = distDir + "/installer-" + appName + ".nsi";
    std::ofstream nsi(nsiPath);
    if (!nsi.is_open()) {
        std::cerr << "  [ERROR] Cannot write " << nsiPath << "\n";
        return 1;
    }

    nsi << "; Generated by bantu installer v" << BANTU_VERSION << "\n";
    nsi << "!define APPNAME \"" << appName << "\"\n";
    nsi << "!define APPVERSION \"" << appVersion << "\"\n";
    nsi << "!define APPPUBLISHER \"Bantu\"\n";
    nsi << "\n";
    nsi << "Name \"${APPNAME} ${APPVERSION}\"\n";
    nsi << "OutFile \"${APPNAME}-Setup-${APPVERSION}.exe\"\n";
    nsi << "InstallDir \"$LOCALAPPDATA\\${APPNAME}\"\n";
    nsi << "RequestExecutionLevel user\n";
    nsi << "\n";
    nsi << "Page directory\n";
    nsi << "Page instfiles\n";
    nsi << "\n";
    nsi << "Section \"${APPNAME}\"\n";
    nsi << "  SetOutPath \"$INSTDIR\"\n";
    if (bundleBantu) {
        nsi << "  ; Bundle the Bantu interpreter so the app runs on machines without Bantu installed.\n";
        nsi << "  File /nonfatal \"bantu.exe\"\n";
        nsi << "  File /nonfatal \"bantu\"\n";
    }
    nsi << "  ; Project source files\n";
    nsi << "  File \"*.b\"\n";
    nsi << "  File /nonfatal \"bantu.json\"\n";
    nsi << "  File /nonfatal \"*.bat\"\n";
    if (!iconPath.empty()) {
        nsi << "  File /nonfatal \"" << iconPath << "\"\n";
    }
    nsi << "\n";
    nsi << "  ; Launcher batch file\n";
    nsi << "  FileOpen $0 \"$INSTDIR\\run-${APPNAME}.bat\" w\n";
    nsi << "  FileWrite $0 '@echo off$\r$\n'\n";
    nsi << "  FileWrite $0 'cd /d \"%~dp0\"$\r$\n'\n";
    nsi << "  FileWrite $0 'if exist bantu.exe (bantu.exe run " << entryFile << ") else (bantu run " << entryFile << ")$\r$\n'\n";
    nsi << "  FileWrite $0 'pause$\r$\n'\n";
    nsi << "  FileClose $0\n";
    nsi << "\n";
    nsi << "  ; Start Menu shortcut\n";
    nsi << "  CreateDirectory \"$SMPROGRAMS\\${APPNAME}\"\n";
    nsi << "  CreateShortcut \"$SMPROGRAMS\\${APPNAME}\\${APPNAME}.lnk\" \\\n";
    nsi << "    \"$INSTDIR\\run-${APPNAME}.bat\" \"\" \"$INSTDIR\\bantu.exe\"\n";
    nsi << "\n";
    nsi << "  ; Uninstaller\n";
    nsi << "  WriteUninstaller \"$INSTDIR\\uninstall-${APPNAME}.exe\"\n";
    nsi << "  CreateShortcut \"$SMPROGRAMS\\${APPNAME}\\Uninstall.lnk\" \\\n";
    nsi << "    \"$INSTDIR\\uninstall-${APPNAME}.exe\"\n";
    nsi << "SectionEnd\n";
    nsi << "\n";
    nsi << "Section \"Uninstall\"\n";
    nsi << "  Delete \"$INSTDIR\\*.*\"\n";
    nsi << "  RMDir \"$INSTDIR\"\n";
    nsi << "  Delete \"$SMPROGRAMS\\${APPNAME}\\*.*\"\n";
    nsi << "  RMDir \"$SMPROGRAMS\\${APPNAME}\"\n";
    nsi << "SectionEnd\n";
    nsi.close();

    std::cout << "  [INSTALLER] NSIS script written: " << nsiPath << "\n";
    std::cout << "  [INSTALLER] App: " << appName << " v" << appVersion << "\n";
    std::cout << "  [INSTALLER] Entry: " << entryFile << "\n";
    if (bundleBantu) {
        std::cout << "  [INSTALLER] Bundling: bantu.exe will be embedded if present in cwd.\n";
    }

    // Try to invoke makensis
    int rc = system("makensis dist/installer.nsi 2>/dev/null || makensis dist/installer-*.nsi");
    if (rc != 0) {
        std::cerr << "\n  [WARN] makensis not found or failed (rc=" << rc << ").\n";
        std::cerr << "  Install NSIS to generate the .exe:\n";
        std::cerr << "    Linux:  sudo apt install nsis\n";
        std::cerr << "    macOS:  brew install nsis\n";
        std::cerr << "    Windows: https://nsis.sourceforge.io/Download\n";
        std::cerr << "\n  Then run:\n";
        std::cerr << "    makensis " << nsiPath << "\n";
        std::cerr << "\n  Output: dist/" << appName << "-Setup-" << appVersion << ".exe\n";
        return 0;  // not fatal — script is still valid
    }

    std::cout << "\n  [INSTALLER] Windows installer ready:\n";
    std::cout << "    dist/" << appName << "-Setup-" << appVersion << ".exe\n";
    return 0;
}

// ── macOS: build a .app bundle ──
static int buildMacOSInstaller(
    const std::string& entryFile,
    const std::string& appName,
    const std::string& appVersion,
    const std::string& iconPath,
    bool bundleBantu)
{
    std::string distDir = "dist";
    if (system(("mkdir -p " + distDir).c_str()) != 0) {
        std::cerr << "  [ERROR] Cannot create dist/\n";
        return 1;
    }

    // .app bundle layout
    std::string appDir   = distDir + "/" + appName + ".app";
    std::string contents = appDir + "/Contents";
    std::string macosDir = contents + "/MacOS";
    std::string resDir   = contents + "/Resources";
    system(("rm -rf '" + appDir + "'").c_str());
    system(("mkdir -p '" + macosDir + "'").c_str());
    system(("mkdir -p '" + resDir + "'").c_str());

    // Copy source files into Resources/
    system(("cp *.b '" + resDir + "/' 2>/dev/null").c_str());
    system(("cp bantu.json '" + resDir + "/' 2>/dev/null").c_str());
    system(("cp -r public '" + resDir + "/' 2>/dev/null").c_str());
    if (!iconPath.empty() && fileExists(iconPath)) {
        system(("cp '" + iconPath + "' '" + resDir + "/app.icns' 2>/dev/null").c_str());
    }
    if (bundleBantu) {
        std::string bantuBinPath = findSelfBinary();
        if (!bantuBinPath.empty()) {
            system(("cp '" + bantuBinPath + "' '" + macosDir + "/bantu' 2>/dev/null").c_str());
            system(("chmod +x '" + macosDir + "/bantu'").c_str());
            std::cout << "  [INSTALLER] Bundled interpreter: " << bantuBinPath << "\n";
        } else {
            std::cerr << "  [WARN] Could not locate bantu binary to bundle.\n";
        }
    }

    // Info.plist
    std::ofstream plist(contents + "/Info.plist");
    if (plist.is_open()) {
        plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        plist << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
        plist << "<plist version=\"1.0\">\n<dict>\n";
        plist << "  <key>CFBundleName</key><string>" << appName << "</string>\n";
        plist << "  <key>CFBundleDisplayName</key><string>" << appName << "</string>\n";
        plist << "  <key>CFBundleIdentifier</key><string>dev.bantu-lang." << appName << "</string>\n";
        plist << "  <key>CFBundleVersion</key><string>" << appVersion << "</string>\n";
        plist << "  <key>CFBundleShortVersionString</key><string>" << appVersion << "</string>\n";
        plist << "  <key>CFBundlePackageType</key><string>APPL</string>\n";
        plist << "  <key>CFBundleExecutable</key><string>" << appName << "</string>\n";
        plist << "  <key>CFBundleIconFile</key><string>app.icns</string>\n";
        plist << "  <key>NSHighResolutionCapable</key><true/>\n";
        plist << "  <key>LSMinimumSystemVersion</key><string>10.13</string>\n";
        plist << "</dict>\n</plist>\n";
        plist.close();
    }

    // Launcher script (the CFBundleExecutable)
    std::string launcherPath = macosDir + "/" + appName;
    std::ofstream launcher(launcherPath);
    if (!launcher.is_open()) {
        std::cerr << "  [ERROR] Cannot write launcher: " << launcherPath << "\n";
        return 1;
    }
    launcher << "#!/bin/bash\n";
    launcher << "# Generated by bantu installer v" << BANTU_VERSION << "\n";
    launcher << "set -e\n";
    launcher << "APP_DIR=\"$(dirname \"$(dirname \"$0\")\")/Resources\"\n";
    launcher << "if [ -x \"$(dirname \"$0\")/bantu\" ]; then\n";
    launcher << "    BANTU_BIN=\"$(dirname \"$0\")/bantu\"\n";
    launcher << "elif command -v bantu >/dev/null 2>&1; then\n";
    launcher << "    BANTU_BIN=\"bantu\"\n";
    launcher << "else\n";
    launcher << "    echo \"[ERROR] Bantu interpreter not found.\" >&2\n";
    launcher << "    exit 1\n";
    launcher << "fi\n";
    launcher << "cd \"$APP_DIR\"\n";
    launcher << "exec \"$BANTU_BIN\" run " << entryFile << " \"$@\"\n";
    launcher.close();
    system(("chmod +x '" + launcherPath + "'").c_str());

    // PkgInfo
    std::ofstream pkgInfo(contents + "/PkgInfo");
    if (pkgInfo.is_open()) {
        pkgInfo.write("APPL????", 8);
        pkgInfo.close();
    }

    std::cout << "\n  [INSTALLER] macOS .app bundle ready:\n";
    std::cout << "    " << appDir << "\n";
    std::cout << "\n  Run on this machine:\n";
    std::cout << "    open " << appDir << "\n";
    std::cout << "\n  Distribute as a .dmg (requires hdiutil on macOS):\n";
    std::cout << "    hdiutil create -volname '" << appName << "' -srcfolder " << appDir
              << " -ov -format UDZO dist/" << appName << "-" << appVersion << ".dmg\n";
    return 0;
}


// ── Android: build an Android Studio project + APK ──
// Generates a full Gradle project under dist/android/<AppName>/
// The project bundles the .b source files as app/src/main/assets/bantu/
// and (if available) a pre-built arm64 bantu binary in jniLibs/arm64-v8a/.
//
// The MainActivity starts the bundled Bantu web server in a background
// thread and renders the app in a WebView pointed at http://127.0.0.1:PORT/.
// End users get a signed-debug APK they can sideload on any Android 7.0+
// phone — the app runs fully offline with no Bantu installed on the device.
//
// If gradle is on PATH and ANDROID_HOME is set, we invoke `gradle assembleDebug`
// automatically; otherwise the user opens the project in Android Studio or
// runs `./build-apk.sh` once the SDK is installed.

// Sanitize appName into a valid Java package name (lowercase, dot-separated).
static std::string sanitizePkgName(const std::string& appName) {
    std::string s = appName;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    std::string out;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.') {
            out += c;
        } else if (c == ' ' || c == '-' || c == '_') {
            // word separator → dot
            if (!out.empty() && out.back() != '.') out += '.';
        }
    }
    // Strip leading/trailing dots
    while (!out.empty() && out.front() == '.') out.erase(out.begin());
    while (!out.empty() && out.back() == '.') out.pop_back();
    if (out.empty()) out = "bantu.app";
    // Must have at least one dot for Java package convention
    if (out.find('.') == std::string::npos) out = "dev.bantu." + out;
    // Don't start with a digit
    if (!out.empty() && out[0] >= '0' && out[0] <= '9') out = "dev." + out;
    return out;
}

// Convert package name to a Java class name (PascalCase, stripped of dots).
static std::string sanitizeClassName(const std::string& appName) {
    std::string s = appName;
    std::string out;
    bool capitalizeNext = true;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            if (capitalizeNext && c >= 'a' && c <= 'z') {
                out += (char)(c - 32);
            } else {
                out += c;
            }
            capitalizeNext = false;
        } else {
            capitalizeNext = true;
        }
    }
    if (out.empty()) out = "BantuApp";
    // Don't start with a digit
    if (out[0] >= '0' && out[0] <= '9') out = "App" + out;
    return out;
}

static int buildAndroidInstaller(
    const std::string& entryFile,
    const std::string& appName,
    const std::string& appVersion,
    const std::string& iconPath,
    bool bundleBantu)
{
    std::string distDir = "dist";
    fsMkdirP(distDir);

    std::string pkgName    = sanitizePkgName(appName);
    std::string className  = sanitizeClassName(appName);
    std::string pkgPath    = pkgName;
    std::replace(pkgPath.begin(), pkgPath.end(), '.', '/');

    // Filesystem-safe project dir name (no spaces / special chars).
    std::string safeDirName;
    for (char c : appName) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_') {
            safeDirName += c;
        } else if (c == ' ' || c == '.') {
            if (!safeDirName.empty() && safeDirName.back() != '-') safeDirName += '-';
        }
    }
    while (!safeDirName.empty() && safeDirName.front() == '-') safeDirName.erase(safeDirName.begin());
    while (!safeDirName.empty() && safeDirName.back() == '-') safeDirName.pop_back();
    if (safeDirName.empty()) safeDirName = "BantuApp";

    std::string projectDir = distDir + "/android/" + safeDirName;
    fsRmRf(projectDir);
    fsMkdirP(projectDir);

    // ─── Project-level Gradle files ───
    writeFile(projectDir + "/settings.gradle",
        R"GRADLE(pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = ")GRADLE" + appName + R"GRADLE("
include ':app'
)GRADLE");

    writeFile(projectDir + "/build.gradle",
        R"GRADLE(// Top-level build file — Generated by bantu installer v)GRADLE"
        + BANTU_VERSION + R"GRADLE(
plugins {
    id 'com.android.application' version '8.1.0' apply false
    id 'org.jetbrains.kotlin.android' version '1.9.0' apply false
}
)GRADLE");

    writeFile(projectDir + "/gradle.properties",
        R"PROPS(org.gradle.jvmargs=-Xmx2048m
android.useAndroidX=true
kotlin.code.style=official
android.nonTransitiveRClass=true
)PROPS");

    writeFile(projectDir + "/local.properties.sample",
        "# Copy this file to local.properties and set your Android SDK path.\n"
        "# Example (Linux/macOS):\n"
        "sdk.dir=/home/you/Android/Sdk\n"
        "# Example (Windows):\n"
        "# sdk.dir=C:\\\\Users\\\\you\\\\AppData\\\\Local\\\\Android\\\\Sdk\n");

    // .gitignore
    writeFile(projectDir + "/.gitignore",
        R"GIT(*.iml
.gradle
/local.properties
/.idea
.DS_Store
/build
/captures
.externalNativeBuild
.cxx
local.properties
/app/build
*.apk
*.ap_
*.dex
)GIT");

    // ─── App module ───
    std::string appDir     = projectDir + "/app";
    std::string mainDir    = appDir + "/src/main";
    std::string javaDir    = mainDir + "/java/" + pkgPath;
    std::string resDir     = mainDir + "/res";
    std::string assetsDir  = mainDir + "/assets/bantu";
    std::string jniDir     = mainDir + "/jniLibs/arm64-v8a";
    std::string jniX64Dir  = mainDir + "/jniLibs/x86_64";
    fsMkdirP(javaDir);
    fsMkdirP(resDir + "/layout");
    fsMkdirP(resDir + "/values");
    fsMkdirP(resDir + "/mipmap-anydpi-v26");
    fsMkdirP(resDir + "/drawable");
    fsMkdirP(resDir + "/xml");
    fsMkdirP(assetsDir);
    fsMkdirP(jniDir);
    fsMkdirP(jniX64Dir);

    // app/build.gradle
    writeFile(appDir + "/build.gradle",
        R"GRADLE(plugins {
    id 'com.android.application'
    id 'org.jetbrains.kotlin.android'
}

android {
    namespace ')GRADLE" + pkgName + R"GRADLE('
    compileSdk 34

    defaultConfig {
        applicationId ")GRADLE" + pkgName + R"GRADLE("
        minSdk 24
        targetSdk 34
        versionCode 1
        versionName ")GRADLE" + appVersion + R"GRADLE("
        ndk {
            abiFilters 'arm64-v8a', 'x86_64'
        }
    }

    buildTypes {
        release {
            minifyEnabled false
            proguardFiles getDefaultProguardFile('proguard-android-optimize.txt'), 'proguard-rules.pro'
        }
        debug {
            minifyEnabled false
        }
    }

    compileOptions {
        sourceCompatibility JavaVersion.VERSION_17
        targetCompatibility JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = '17' }

    // Important: extract native libs so we can chmod+exec the bundled bantu.
    packagingOptions {
        jniLibs { useLegacyPackaging true }
    }
}

dependencies {
    implementation 'androidx.core:core-ktx:1.12.0'
    implementation 'androidx.appcompat:appcompat:1.6.1'
    implementation 'com.google.android.material:material:1.11.0'
    implementation 'androidx.webkit:webkit:1.9.0'
}
)GRADLE");

    writeFile(appDir + "/proguard-rules.pro", "# Add project-specific ProGuard rules here.\n");

    // AndroidManifest.xml
    writeFile(mainDir + "/AndroidManifest.xml",
        R"XML(<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android">

    <uses-permission android:name="android.permission.INTERNET" />
    <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />

    <application
        android:allowBackup="true"
        android:icon="@mipmap/ic_launcher"
        android:label="@string/app_name"
        android:roundIcon="@mipmap/ic_launcher"
        android:supportsRtl="true"
        android:theme="@style/Theme.BantuApp"
        android:usesCleartextTraffic="true">
        <activity
            android:name=".)XML" + className + R"XML("
            android:exported="true"
            android:configChanges="orientation|screenSize|keyboardHidden">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>

</manifest>
)XML");

    // MainActivity.kt
    writeFile(javaDir + "/" + className + ".kt",
        "package " + pkgName + "\n\n"
        R"KT(import android.annotation.SuppressLint
import android.os.Bundle
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import android.os.Handler
import android.os.Looper
import android.util.Log

class )KT" + className + R"KT( : AppCompatActivity() {

    private lateinit var webView: WebView
    private lateinit var statusText: TextView
    private val handler = Handler(Looper.getMainLooper())
    private var runner: BantuRunner? = null

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.status_text)
        webView = findViewById(R.id.webview)
        webView.settings.javaScriptEnabled = true
        webView.settings.domStorageEnabled = true
        webView.webViewClient = WebViewClient()
        webView.settings.allowFileAccess = true
        webView.settings.allowContentAccess = true

        statusText.text = "Starting Bantu runtime..."

        runner = BantuRunner(this).apply {
            start { port ->
                handler.post {
                    if (port > 0) {
                        statusText.text = "Bantu running on port $port"
                        webView.loadUrl("http://127.0.0.1:$port/")
                    } else {
                        statusText.text = "Bantu failed to start (check logcat: tag 'BantuRunner')"
                    }
                }
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        runner?.stop()
    }
}
)KT");

    // BantuRunner.kt — extracts .b files + bundled bantu binary, runs as subprocess.
    writeFile(javaDir + "/BantuRunner.kt",
        "package " + pkgName + "\n\n"
        R"KT(import android.content.Context
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Extracts the bundled .b source files and the arm64 bantu binary from the
 * app's assets/jniLibs into the app's private data directory, then launches
 * `./bantu run )KT" + entryFile + R"KT(` as a subprocess and waits for the
 * Sua HTTP server to bind a port (parsed from stdout).
 *
 * If no bundled bantu binary is present (the user did not provide one),
 * the runner reports failure — see BUILD-ANDROID.md for how to cross-compile
 * Bantu for Android arm64-v8a using the NDK.
 */
class BantuRunner(private val ctx: Context) {
    private val TAG = "BantuRunner"
    private val started = AtomicBoolean(false)
    private var process: Process? = null
    private var portThread: Thread? = null

    fun start(onReady: (Int) -> Unit) {
        if (!started.compareAndSet(false, true)) return

        Thread {
            try {
                val workDir = File(ctx.filesDir, "bantu-app").apply { mkdirs() }

                // 1. Extract .b files + bantu.json from assets/bantu/
                extractAssets("bantu", workDir)

                // 2. Extract the bundled bantu binary (if present) from jniLibs.
                //    Android packages native libs under lib/<abi>/lib*.so — but
                //    the file is the actual ELF we want to exec.
                val bantuBin = File(workDir, "bantu")
                val nativeDir = File(ctx.applicationInfo.nativeLibraryDir)
                val bundled = File(nativeDir, "libbantu.so")
                if (bundled.exists()) {
                    bundled.copyTo(bantuBin, overwrite = true)
                    @Suppress("UnsafeNewApiCall")
                    bantuBin.setExecutable(true, true)
                    Log.i(TAG, "Extracted bundled bantu: ${bantuBin.absolutePath} (${bundled.length()} bytes)")
                } else {
                    Log.e(TAG, "No bundled bantu binary at ${bundled.absolutePath}")
                    Log.e(TAG, "See BUILD-ANDROID.md for cross-compile instructions (NDK required).")
                    onReady(0)
                    return@Thread
                }

                // 3. Launch `./bantu run )KT" + entryFile + R"KT(` and capture stdout.
                val pb = ProcessBuilder(bantuBin.absolutePath, "run", ")KT" + entryFile + R"KT(")
                    .directory(workDir)
                    .redirectErrorStream(true)
                pb.environment()["BANTU_QUIET"] = "1"
                val p = pb.start()
                process = p

                // 4. Parse stdout for a port hint: "Listening on port 8080" or similar.
                portThread = Thread {
                    try {
                        val reader = p.inputStream.bufferedReader()
                        var line: String?
                        var detectedPort = 0
                        while (reader.readLine().also { line = it } != null) {
                            val l = line ?: continue
                            Log.i(TAG, l)
                            if (detectedPort == 0) {
                                // Sua server typically prints: "Sua server listening on port 8080"
                                val m = Regex("""port\s+(\d{2,5})""").find(l.lowercase())
                                if (m != null) {
                                    detectedPort = m.groupValues[1].toInt()
                                    onReady(detectedPort)
                                }
                            }
                        }
                        if (detectedPort == 0) onReady(0)
                    } catch (e: Exception) {
                        Log.e(TAG, "stdout reader died: ${e.message}")
                        onReady(0)
                    }
                }
                portThread?.start()
            } catch (e: Exception) {
                Log.e(TAG, "Failed to start Bantu: ${e.message}", e)
                onReady(0)
            }
        }.start()
    }

    fun stop() {
        try { process?.destroy() } catch (_: Exception) {}
    }

    private fun extractAssets(assetDir: String, destDir: File) {
        val list = ctx.assets.list(assetDir) ?: return
        for (name in list) {
            val path = "$assetDir/$name"
            val out = File(destDir, name)
            val sub = ctx.assets.list(path)
            if (sub != null && sub.isNotEmpty()) {
                out.mkdirs()
                extractAssets(path, out)
            } else {
                ctx.assets.open(path).use { input ->
                    FileOutputStream(out).use { input.copyTo(it) }
                }
                Log.i(TAG, "Extracted asset: $name")
            }
        }
    }
}
)KT");

    // activity_main.xml
    writeFile(resDir + "/layout/activity_main.xml",
        R"XML(<?xml version="1.0" encoding="utf-8"?>
<LinearLayout xmlns:android="http://schemas.android.com/apk/res/android"
    android:layout_width="match_parent"
    android:layout_height="match_parent"
    android:orientation="vertical">

    <TextView
        android:id="@+id/status_text"
        android:layout_width="match_parent"
        android:layout_height="wrap_content"
        android:padding="8dp"
        android:text="Initializing..."
        android:textSize="12sp"
        android:background="#1a1a2e"
        android:textColor="#bd93f9" />

    <WebView
        android:id="@+id/webview"
        android:layout_width="match_parent"
        android:layout_height="0dp"
        android:layout_weight="1" />

</LinearLayout>
)XML");

    // strings.xml
    writeFile(resDir + "/values/strings.xml",
        R"XML(<?xml version="1.0" encoding="utf-8"?>
<resources>
    <string name="app_name">)XML" + appName + R"XML(</string>
</resources>
)XML");

    // themes.xml — Dracula-themed base.
    writeFile(resDir + "/values/themes.xml",
        R"XML(<?xml version="1.0" encoding="utf-8"?>
<resources>
    <style name="Theme.BantuApp" parent="Theme.MaterialComponents.DayNight.NoActionBar">
        <item name="colorPrimary">#bd93f9</item>
        <item name="colorPrimaryVariant">#6272a4</item>
        <item name="colorOnPrimary">#f8f8f2</item>
        <item name="colorSecondary">#ff79c6</item>
        <item name="colorOnSecondary">#282a36</item>
        <item name="android:statusBarColor">#282a36</item>
    </style>
</resources>
)XML");

    // ic_launcher (a simple SVG → mipmap-anydpi-v26 with adaptive XML, plus a
    // fallback PNG generated by build-apk.sh if ImageMagick is available).
    writeFile(resDir + "/mipmap-anydpi-v26/ic_launcher.xml",
        R"XML(<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@color/ic_launcher_background"/>
    <foreground android:drawable="@drawable/ic_launcher_foreground"/>
</adaptive-icon>
)XML");

    writeFile(resDir + "/values/colors.xml",
        R"XML(<?xml version="1.0" encoding="utf-8"?>
<resources>
    <color name="ic_launcher_background">#282a36</color>
</resources>
)XML");

    writeFile(resDir + "/drawable/ic_launcher_foreground.xml",
        R"XML(<?xml version="1.0" encoding="utf-8"?>
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    android:width="108dp"
    android:height="108dp"
    android:viewportWidth="108"
    android:viewportHeight="108">
    <path
        android:fillColor="#f8f8f2"
        android:pathData="M30,30 L30,78 L42,78 L42,58 L66,78 L82,78 L52,50 L82,30 L66,30 L42,52 L42,30 Z"/>
</vector>
)XML");

    // Copy .b source files + bantu.json + public/ into assets/bantu/
    // (Cross-platform: uses C++17 <filesystem> instead of shell globs.)
    int nB = fsCopyByExtension(".", assetsDir, ".b");
    fsCopyNamedIfExists(".", assetsDir, "bantu.json");
    fsCopyNamedDirIfExists(".", assetsDir, "public");
    if (nB > 0) {
        std::cout << "  [INSTALLER] Copied " << nB << " .b source file(s) into assets/.\n";
    }

    // Optionally embed the bantu binary as libbantu.so.
    // Search order:
    //   1. ./android/libbantu.so (arm64)  + ./android/libbantu-x86_64.so
    //   2. $BANTU_ANDROID_ARM64  + $BANTU_ANDROID_X86_64
    //   3. ~/.bantu/android/arm64-v8a/libbantu.so  + .../x86_64/...
    bool bundledArm64 = false;
    bool bundledX64 = false;

    if (bundleBantu) {
        // arm64
        std::vector<std::string> armCandidates = {
            "android/libbantu.so",
            "android/arm64-v8a/libbantu.so",
        };
        const char* envArm = std::getenv("BANTU_ANDROID_ARM64");
        if (envArm && *envArm) armCandidates.push_back(envArm);
        const char* home = std::getenv("HOME");
        if (home && *home) {
            armCandidates.push_back(std::string(home) + "/.bantu/android/arm64-v8a/libbantu.so");
        }
        for (const auto& p : armCandidates) {
            if (fileExists(p)) {
                fsCopyFile(p, jniDir + "/libbantu.so");
                std::cout << "  [INSTALLER] Bundled arm64-v8a bantu: " << p << "\n";
                bundledArm64 = true;
                break;
            }
        }
        // x86_64 (for emulator)
        std::vector<std::string> x64Candidates = {
            "android/libbantu-x86_64.so",
            "android/x86_64/libbantu.so",
        };
        const char* envX64 = std::getenv("BANTU_ANDROID_X86_64");
        if (envX64 && *envX64) x64Candidates.push_back(envX64);
        if (home && *home) {
            x64Candidates.push_back(std::string(home) + "/.bantu/android/x86_64/libbantu.so");
        }
        for (const auto& p : x64Candidates) {
            if (fileExists(p)) {
                fsCopyFile(p, jniX64Dir + "/libbantu.so");
                std::cout << "  [INSTALLER] Bundled x86_64 bantu: " << p << "\n";
                bundledX64 = true;
                break;
            }
        }

        if (!bundledArm64 && !bundledX64) {
            std::cout << "  [INSTALLER] No pre-built Android bantu binary found.\n";
            std::cout << "              Place a cross-compiled arm64 bantu at one of:\n";
            std::cout << "                ./android/libbantu.so\n";
            std::cout << "                ./android/arm64-v8a/libbantu.so\n";
            std::cout << "                ~/.bantu/android/arm64-v8a/libbantu.so\n";
            std::cout << "                or set $BANTU_ANDROID_ARM64\n";
            std::cout << "              See BUILD-ANDROID.md in the project for NDK instructions.\n";
            std::cout << "              The APK will build but show a 'bantu not found' screen at runtime.\n";
        }
    } else {
        std::cout << "  [INSTALLER] --no-bundle-bantu: skipping native binary embed.\n";
        std::cout << "              The generated APK will not run unless the device has Bantu installed.\n";
    }

    // build-apk.sh — wraps gradle assembleDebug
    writeFile(projectDir + "/build-apk.sh",
        R"BASH(#!/usr/bin/env bash
# Build the Android APK from the project generated by `bantu installer --platform android`.
# Requires: Android SDK + JDK 17 + (optionally) Android NDK for cross-compiling bantu.
set -euo pipefail

cd "$(dirname "$0")"

# Locate gradle
if [ ! -x ./gradlew ]; then
    if command -v gradle >/dev/null 2>&1; then
        gradle wrapper --gradle-version 8.4
    else
        echo "[ERROR] No gradlew and no system gradle."
        echo "        Install gradle (apt install gradle / brew install gradle) and re-run."
        exit 1
    fi
fi

# Require ANDROID_HOME (or local.properties)
if [ ! -f local.properties ] && [ -z "${ANDROID_HOME:-}" ]; then
    echo "[ERROR] Neither local.properties nor ANDROID_HOME is set."
    echo "        Create local.properties with: sdk.dir=/path/to/Android/Sdk"
    echo "        OR: export ANDROID_HOME=/path/to/Android/Sdk"
    exit 1
fi

echo "[build-apk] Building debug APK..."
./gradlew assembleDebug --no-daemon

APK="app/build/outputs/apk/debug/app-debug.apk"
OUT="app-debug.apk"
cp "$APK" "$OUT"
echo ""
echo "[build-apk] APK ready: $OUT ($(du -h "$OUT" | cut -f1))"
echo "[build-apk] Install on a connected device with:"
echo "    adb install -r $OUT"
)BASH");
#ifndef _WIN32
    // Unix: mark the shell helper executable.
    chmod((projectDir + "/build-apk.sh").c_str(), 0755);
#endif

    // build-apk.bat — Windows equivalent so users on cmd / PowerShell can
    // build the APK without needing Git Bash or WSL.
    writeFile(projectDir + "/build-apk.bat",
        R"BAT(@echo off
REM Build the Android APK from the project generated by `bantu installer --platform android`.
REM Requires: Android SDK + JDK 17 + (optionally) Android NDK for cross-compiling bantu.
setlocal enabledelayedexpansion

cd /d "%~dp0"

REM Locate gradle wrapper
if not exist gradlew.bat (
    where gradle >nul 2>nul
    if !errorlevel! == 0 (
        call gradle wrapper --gradle-version 8.4
        if !errorlevel! neq 0 (
            echo [ERROR] gradle wrapper generation failed.
            exit /b 1
        )
    ) else (
        echo [ERROR] No gradlew.bat and no system gradle on PATH.
        echo        Install gradle from https://gradle.org/install/ and re-run.
        exit /b 1
    )
)

REM Require ANDROID_HOME (or local.properties)
if not exist local.properties (
    if "%ANDROID_HOME%"=="" (
        echo [ERROR] Neither local.properties nor ANDROID_HOME is set.
        echo        Create local.properties with: sdk.dir=C:\path\to\Android\Sdk
        echo        OR: set ANDROID_HOME=C:\path\to\Android\Sdk
        exit /b 1
    )
)

echo [build-apk] Building debug APK...
call gradlew.bat assembleDebug --no-daemon
if !errorlevel! neq 0 (
    echo [ERROR] Gradle build failed.
    exit /b !errorlevel!
)

set "APK=app\build\outputs\apk\debug\app-debug.apk"
set "OUT=app-debug.apk"
copy /Y "%APK%" "%OUT%" >nul
echo.
echo [build-apk] APK ready: %OUT%
for %%I in (%OUT%) do echo [build-apk] Size: %%~zI bytes
echo [build-apk] Install on a connected device with:
echo     adb install -r %OUT%
)BAT");

    // BUILD-ANDROID.md — NDK cross-compile instructions
    writeFile(projectDir + "/BUILD-ANDROID.md",
        R"MD(# Building the Android APK

This project was generated by `bantu installer --platform android`.

## What's inside

- `app/src/main/assets/bantu/` — your `.b` source files + `bantu.json`
- `app/src/main/jniLibs/{arm64-v8a,x86_64}/libbantu.so` — pre-built Bantu
  interpreter for Android (if one was found at build time)
- `app/src/main/java/<pkg>/MainActivity.kt` — WebView activity that loads
  `http://127.0.0.1:<port>/` after starting the bundled Bantu server
- `app/src/main/java/<pkg>/BantuRunner.kt` — extracts assets + launches
  the bundled bantu as a subprocess

## Step 1 — Install the Android SDK

Install Android Studio (or just the command-line SDK tools) and JDK 17.
Set `ANDROID_HOME` to the SDK path:

```bash
export ANDROID_HOME=$HOME/Android/Sdk
```

## Step 2 — Cross-compile Bantu for Android (one-time)

To actually run Bantu on a phone, you need a native binary compiled for
the phone's CPU (arm64-v8a on modern phones, x86_64 on emulators).

```bash
# Install Android NDK via Android Studio SDK Manager or:
sdkmanager "ndk;26.1.10909125"

export NDK_ROOT=$ANDROID_HOME/ndk/26.1.10909125

# From the Bantu source tree:
cd bantu-src/compiler
mkdir build-android-arm64 && cd build-android-arm64
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$NDK_ROOT/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24
make -j$(nproc)

# The output binary is ./bantu — install it where the installer looks:
mkdir -p ~/.bantu/android/arm64-v8a
cp bantu ~/.bantu/android/arm64-v8a/libbantu.so
```

Repeat with `-DANDROID_ABI=x86_64` for emulator support, outputting to
`~/.bantu/android/x86_64/libbantu.so`.

## Step 3 — Build the APK

```bash
cd dist/android/<AppName>
./build-apk.sh
```

Or open the project in Android Studio and press Run.

## Step 4 — Install on a phone

```bash
adb install -r app-debug.apk
```

Enable USB debugging on the phone first (Developer options → USB debugging).

## Notes

- The app runs **fully offline** — no internet, no Bantu installed on the
  phone required. The bundled `libbantu.so` is extracted at first launch.
- `android:usesCleartextTraffic="true"` is set so the WebView can load
  `http://127.0.0.1:PORT/`. This is safe — traffic never leaves the device.
- The debug APK is signed with the Android debug key. For distribution,
  generate your own keystore and sign with `apksigner`.
)MD");

    // README.md
    writeFile(projectDir + "/README.md",
        "# " + appName + " — Android\n\n"
        "Generated by `bantu installer --platform android` (Bantu v" + BANTU_VERSION + ").\n\n"
        "- **Package**: `" + pkgName + "`\n"
        "- **Entry**: `" + entryFile + "`\n"
        "- **Version**: " + appVersion + "\n\n"
        "## Quick start\n\n"
        "```bash\n"
        "./build-apk.sh            # build APK (needs Android SDK + JDK 17)\n"
        "adb install -r app-debug.apk\n"
        "```\n\n"
        "See `BUILD-ANDROID.md` for full instructions including how to\n"
        "cross-compile Bantu for Android arm64 with the NDK.\n");

    // ─── Try to auto-build the APK directly ────────────────────────────
    // The user asked: "I want it to make .apk direct if I run
    // `bantu installer --platform android`." So that's what we do —
    // invoke gradle (the wrapper if present, otherwise the system gradle)
    // to actually produce app-debug.apk, then copy it to a clean path
    // at dist/<AppName>-debug.apk.
    std::cout << "\n  [INSTALLER] Android Studio project ready:\n";
    std::cout << "    " << projectDir << "/\n";
    std::cout << "\n  [INSTALLER] Attempting to build the APK now...\n";

    // Decide which gradle command to use. On Windows we use gradlew.bat;
    // on Unix we use ./gradlew. If neither wrapper exists yet, fall back
    // to system `gradle` and have it generate the wrapper first.
    std::string gradleCmd;
    bool needWrapperBoot = false;
#ifdef _WIN32
    if (fileExists(projectDir + "/gradlew.bat")) {
        gradleCmd = "gradlew.bat";
    } else if (std::system("where gradle >nul 2>nul") == 0) {
        gradleCmd = "gradlew.bat";
        needWrapperBoot = true;
    }
#else
    if (fileExists(projectDir + "/gradlew")) {
        gradleCmd = "./gradlew";
    } else if (std::system("command -v gradle >/dev/null 2>&1") == 0) {
        gradleCmd = "./gradlew";
        needWrapperBoot = true;
    }
#endif

    // Also check that ANDROID_HOME or local.properties is set, otherwise
    // gradle will fail with a confusing error.
    bool hasSdk = false;
    if (fileExists(projectDir + "/local.properties")) {
        hasSdk = true;
    } else {
        const char* ah = std::getenv("ANDROID_HOME");
        if (!ah || !*ah) ah = std::getenv("ANDROID_SDK_ROOT");
        if (ah && *ah) hasSdk = true;
    }

    if (gradleCmd.empty()) {
        std::cout << "  [INSTALLER] Gradle not found on PATH.\n";
        std::cout << "              Install gradle (apt install gradle / brew install gradle /\n";
        std::cout << "              https://gradle.org/install/) and re-run, OR open the project\n";
        std::cout << "              in Android Studio and press Run.\n";
    } else if (!hasSdk) {
        std::cout << "  [INSTALLER] Android SDK not configured.\n";
        std::cout << "              Set ANDROID_HOME or create local.properties with:\n";
        std::cout << "                sdk.dir=/path/to/Android/Sdk\n";
        std::cout << "              Then re-run, OR open the project in Android Studio.\n";
    } else {
        // Save current directory, cd into the project, run gradle, then cd back.
        std::string cwd = fs::current_path().string();
        std::error_code ec;
        fs::current_path(fs::path(projectDir), ec);

        int rc = 1;
        if (needWrapperBoot) {
            std::cout << "  [INSTALLER] Generating gradle wrapper (one-time)...\n";
#ifdef _WIN32
            rc = std::system("gradle wrapper --gradle-version 8.4");
#else
            rc = std::system("gradle wrapper --gradle-version 8.4");
#endif
            if (rc == 0) {
                std::cout << "  [INSTALLER] Building debug APK (this may take 1-3 min on first run)...\n";
#ifdef _WIN32
                rc = std::system("gradlew.bat assembleDebug --no-daemon");
#else
                rc = std::system("./gradlew assembleDebug --no-daemon");
#endif
            }
        } else {
            std::cout << "  [INSTALLER] Building debug APK (this may take 1-3 min on first run)...\n";
#ifdef _WIN32
            rc = std::system((gradleCmd + " assembleDebug --no-daemon").c_str());
#else
            rc = std::system((gradleCmd + " assembleDebug --no-daemon").c_str());
#endif
        }

        // Restore the original working directory.
        fs::current_path(fs::path(cwd), ec);

        // Look for the built APK and copy it to a clean top-level path.
        std::string builtApk = projectDir + "/app/build/outputs/apk/debug/app-debug.apk";
        std::string finalApk = distDir + "/" + safeDirName + "-debug.apk";

        if (rc == 0 && fileExists(builtApk)) {
            fsCopyFile(builtApk, finalApk);
            std::cout << "\n  [INSTALLER] ✓ APK built successfully!\n";
            std::cout << "    " << finalApk << "\n";
            // Show file size
            try {
                auto sz = fs::file_size(fs::path(finalApk));
                std::cout << "    Size: " << (sz / 1024) << " KB\n";
            } catch (...) {}
            std::cout << "\n  Install on a connected device with:\n";
            std::cout << "    adb install -r " << finalApk << "\n";
        } else if (rc == 0) {
            std::cout << "\n  [INSTALLER] Gradle reported success but the APK file was not found at the expected path.\n";
            std::cout << "    Looked for: " << builtApk << "\n";
            std::cout << "    The project is at: " << projectDir << "/\n";
            std::cout << "    Try opening it in Android Studio and pressing Run.\n";
        } else {
            std::cout << "\n  [INSTALLER] Gradle build failed (exit code " << rc << ").\n";
            std::cout << "    The project is at: " << projectDir << "/\n";
            std::cout << "    Common causes:\n";
            std::cout << "      - JDK 17 not installed (gradle 8.4 requires JDK 17)\n";
            std::cout << "      - Android SDK build-tools / platform-tools not installed\n";
            std::cout << "      - ANDROID_HOME points to the wrong directory\n";
            std::cout << "    Open the project in Android Studio for detailed error output,\n";
            std::cout << "    or re-run manually inside the project directory:\n";
#ifdef _WIN32
            std::cout << "      cd " << projectDir << " && build-apk.bat\n";
#else
            std::cout << "      cd " << projectDir << " && ./build-apk.sh\n";
#endif
        }
    }

    // Helpful next steps regardless of whether the auto-build succeeded.
    if (!gradleCmd.empty() && hasSdk) {
        // Already attempted the build above — nothing more to add.
    } else {
        std::cout << "\n  Next steps:\n";
        std::cout << "    1. Cross-compile bantu for Android arm64 (see BUILD-ANDROID.md)\n";
#ifdef _WIN32
        std::cout << "    2. cd " << projectDir << " && build-apk.bat\n";
#else
        std::cout << "    2. cd " << projectDir << " && ./build-apk.sh\n";
#endif
        std::cout << "    3. adb install -r " << distDir << "/" << safeDirName << "-debug.apk\n";
    }
    return 0;
}


int cmdInstaller(int argc, char* argv[]) {
    std::string entryFile;
    std::string appName;
    std::string appVersion;
    std::string iconPath;
    std::string platform;
    bool bundleBantu = true;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--name" && i + 1 < argc) {
            appName = argv[++i];
        } else if (a == "--version" && i + 1 < argc) {
            appVersion = argv[++i];
        } else if (a == "--icon" && i + 1 < argc) {
            iconPath = argv[++i];
        } else if (a == "--platform" && i + 1 < argc) {
            platform = argv[++i];
        } else if (a == "--no-bundle-bantu") {
            bundleBantu = false;
        } else if (a == "--bundle-bantu") {
            bundleBantu = true;
        } else if (a == "--help" || a == "-h") {
            std::cout << "  Usage: bantu installer [entry.b] [options]\n";
            std::cout << "  Options:\n";
            std::cout << "    --name <Name>          Application name (default: from bantu.json or folder)\n";
            std::cout << "    --version <x.y.z>      Application version (default: 1.0.0)\n";
            std::cout << "    --icon <path>          Icon file (.png/.ico/.icns)\n";
            std::cout << "    --platform <p>         linux | windows | macos | android (default: auto-detect)\n";
            std::cout << "    --bundle-bantu         Embed bantu interpreter (default: on)\n";
            std::cout << "    --no-bundle-bantu      Don't embed bantu (smaller installer, requires Bantu on target)\n";
            std::cout << "\n  Output: ./dist/<name>_<version>_*.{deb,exe,app}\n";
            std::cout << "          ./dist/android/<name>/ (Android Studio project)\n";
            return 0;
        } else if (!a.empty() && a[0] != '-') {
            entryFile = a;
        }
    }

    // Resolve entry file
    if (entryFile.empty()) {
        if (fileExists("main.b")) entryFile = "main.b";
        else if (fileExists("app.b")) entryFile = "app.b";
        else if (fileExists("index.b")) entryFile = "index.b";
        else if (fileExists("server.b")) entryFile = "server.b";
        else {
            std::cerr << "  [ERROR] No entry file found (looked for main.b, app.b, index.b, server.b)\n";
            return 1;
        }
    }
    if (!fileExists(entryFile)) {
        std::cerr << "  [ERROR] Entry file not found: " << entryFile << "\n";
        return 1;
    }

    // Read bantu.json to fill in defaults
    if (appName.empty() || appVersion.empty()) {
        if (fileExists("bantu.json")) {
            std::string json = readFile("bantu.json");
            if (appName.empty())    appName    = jsonField(json, "name");
            if (appVersion.empty()) appVersion = jsonField(json, "version");
        }
    }
    // Fall back to folder name + 1.0.0
    if (appName.empty()) {
        std::string cwd = getCurrentDir();
        size_t slash = cwd.find_last_of("/\\");
        appName = (slash == std::string::npos) ? cwd : cwd.substr(slash + 1);
        if (appName.empty()) appName = "BantuApp";
    }
    if (appVersion.empty()) appVersion = "1.0.0";

    // Auto-detect platform if not specified
    if (platform.empty()) {
#ifdef _WIN32
        platform = "windows";
#elif defined(__APPLE__)
        platform = "macos";
#else
        platform = "linux";
#endif
    }

    // Normalize
    if (platform == "win" || platform == "win32") platform = "windows";
    if (platform == "mac" || platform == "osx")   platform = "macos";
    if (platform == "apk" || platform == "droid") platform = "android";

    std::cout << "\n  ╔══════════════════════════════════════════════╗\n";
    std::cout << "  ║   bantu installer — desktop installer builder ║\n";
    std::cout << "  ╚══════════════════════════════════════════════╝\n\n";
    std::cout << "  App:      " << appName << " v" << appVersion << "\n";
    std::cout << "  Entry:    " << entryFile << "\n";
    std::cout << "  Platform: " << platform << "\n";
    std::cout << "  Bundle:   " << (bundleBantu ? "yes (embed bantu)" : "no (use system bantu)") << "\n";
    if (!iconPath.empty()) std::cout << "  Icon:     " << iconPath << "\n";
    std::cout << "\n";

    if (platform == "linux") {
        return buildLinuxInstaller(entryFile, appName, appVersion, iconPath, bundleBantu);
    } else if (platform == "windows") {
        return buildWindowsInstaller(entryFile, appName, appVersion, iconPath, bundleBantu);
    } else if (platform == "macos") {
        return buildMacOSInstaller(entryFile, appName, appVersion, iconPath, bundleBantu);
    } else if (platform == "android") {
        return buildAndroidInstaller(entryFile, appName, appVersion, iconPath, bundleBantu);
    }

    std::cerr << "  [ERROR] Unknown platform: " << platform << "\n";
    std::cerr << "  Valid: linux, windows, macos, android\n";
    return 1;
}

// ─── bantu bench ──────────────────────────────────────────────────────
// v1.2.1: Run built-in micro-benchmarks and print results to stdout.
// Used by the Bantu benchmark suite (see /benchmarks).
int cmdBench() {
    std::cout << "\n  Bantu v" << BANTU_VERSION << " — Benchmark Suite\n";
    std::cout << "  ────────────────────────────────────────────\n\n";

    auto run = [](const std::string& name, const std::string& code, int iters) {
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iters; ++i) {
            try { runCode(code, "<bench>"); }
            catch (...) {}
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        double perIter = (double)ms / iters;
        std::cout << "  " << name;
        for (int p = (int)name.size(); p < 36; ++p) std::cout << ' ';
        std::cout << iters << " iters in " << ms << " ms  ("
                  << perIter << " ms/iter)\n";
    };

    run("fib(28) recursive",
        "def fib($n) { if ($n < 2) { return $n; } return fib($n-1) + fib($n-2); } print(fib(28));",
        3);
    run("loop 1M arithmetic",
        "$sum = 0; for ($i = 0; $i < 1000000; $i += 1) { $sum += $i; } print($sum);",
        5);
    run("list push 100k",
        "$l = []; for ($i = 0; $i < 100000; $i += 1) { $l.push($i); } print($l.size());",
        5);
    run("string concat 10k x 100",
        "$s = \"\"; for ($i = 0; $i < 10000; $i += 1) { $s = $s + \"x\"; } print($s.size());",
        3);
    run("dict set/get 100k",
        "$d = {}; for ($i = 0; $i < 100000; $i += 1) { $d[\"k\" + $i] = $i; } print($d.size());",
        3);

    std::cout << "\n  Done.\n";
    return 0;
}

// ─── Main ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::signal(SIGINT, [](int) {
        std::cout << "\n  [Interrupted] Type 'exit' to quit\n";
    });

    // No arguments → REPL
    if (argc < 2) {
        runRepl();
        return 0;
    }

    std::string command = argv[1];

    // v1.2.2: --quiet / -q is a global flag. It must be the FIRST argument
    // and applies to whichever command follows. Usage:
    //   bantu --quiet run server.b
    //   bantu -q run server.b
    if (command == "--quiet" || command == "-q") {
        g_quietMode = true;
        if (argc < 3) {
            runRepl();
            return 0;
        }
        // Shift argv so the rest of the dispatcher sees the real command
        command = argv[2];
        // Rebuild argv without the --quiet slot: shift everything left.
        for (int i = 2; i < argc - 1; ++i) {
            argv[i] = argv[i + 1];
        }
        argv[argc - 1] = nullptr;
        argc -= 1;
    }

    // ─── Global flags ───
    if (command == "--help" || command == "-h") {
        printHelp();
        return 0;
    }

    if (command == "--version" || command == "-v") {
        std::cout << "Bantu v" << BANTU_VERSION << "\n";
        return 0;
    }

    // ─── bantu run ───
    if (command == "run") {
        std::string file = "";
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--quiet" || a == "-q") { g_quietMode = true; }
            else if (a == "--no-lint") { g_lintGate = false; }
            else if (!a.empty() && a[0] != '-') { file = a; }
        }
        return cmdRun(file);
    }

    // ─── bantu lint ───
    if (command == "lint") return cmdLint(argc, argv);

    // ─── bantu build ───
    if (command == "build") {
        std::string file = (argc >= 3) ? argv[2] : "";
        return cmdBuild(file);
    }

    // ─── bantu init / bantu new ───
    // Supports:
    //   bantu init <name>            ← CLI "Hello World" (default)
    //   bantu init --web <name>      ← Sua web app (Spring Initializer-style)
    //   bantu new <name>             ← alias of init
    //   bantu new --web <name>       ← alias of init --web
    if (command == "init" || command == "new") {
        // Parse args. The --web flag can come before or after the name.
        bool web = false;
        std::string name = "";
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--web" || a == "-w" || a == "web") {
                web = true;
            } else if (a == "--cli" || a == "-c") {
                web = false;
            } else if (a == "--help" || a == "-h") {
                std::cout << "  Usage:\n";
                std::cout << "    bantu init <name>           CLI project (default)\n";
                std::cout << "    bantu init --web <name>     Sua web app starter\n";
                return 0;
            } else if (!a.empty() && a[0] != '-') {
                name = a;
            }
        }
        if (web) {
            return cmdInitWeb(name);
        }
        return cmdInit(name);
    }

    // ─── bantu relay ───
    if (command == "relay") {
        // Manual port parser (avoids std::atoi → __isoc23_strtol@GLIBC_2.38 at -O2)
        int port = 3478;
        if (argc >= 3 && argv[2] && argv[2][0]) {
            int v = 0;
            bool ok = true;
            for (const char* p = argv[2]; *p; ++p) {
                if (*p < '0' || *p > '9') { ok = false; break; }
                v = v * 10 + (*p - '0');
            }
            if (ok) port = v;
        }
        return cmdRelay(port);
    }

    // ─── bantu setup / uninstall / doctor (PATH integration) ───
    if (command == "setup")  return cmdSetup(argc, argv);
    if (command == "uninstall" || command == "unlink") return cmdUninstall();
    if (command == "doctor") return cmdDoctor();

    // ─── bantu install / add / remove / update / list / search / publish ─
    if (command == "install" || command == "i")  return cmdInstall();
    if (command == "add"      || command == "a")  return cmdAdd(argc, argv);
    if (command == "remove"   || command == "rm") return cmdRemove(argc, argv);
    if (command == "update"   || command == "up") return cmdUpdate(argc, argv);
    if (command == "list"     || command == "ls") return cmdList();
    if (command == "search"   || command == "find") return cmdSearch(argc, argv);
    if (command == "publish"  || command == "pub") return cmdPublish(argc, argv);

    // ─── bantu build-windows / bench (v1.2.1) ───
    if (command == "build-windows" || command == "build-win") return cmdBuildWindows(argc, argv);
    if (command == "bench")        return cmdBench();

    // ─── bantu installer (cross-platform desktop installer generator) ───
    if (command == "installer" || command == "installer") return cmdInstaller(argc, argv);

    // ─── bantu <file.b> (legacy shorthand for run) ───
    if (command.size() > 2 && command.substr(command.size() - 2) == ".b") {
        return cmdRun(command);
    }

    // ─── Unknown command ───
    std::cerr << "  [ERROR] Unknown command: " << command << "\n";
    std::cerr << "  Run 'bantu --help' for available commands.\n";
    return 1;
}
