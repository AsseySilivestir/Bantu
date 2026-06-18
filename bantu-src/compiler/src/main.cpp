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
#include "evaluator.hpp"
#include "environment.hpp"
#include "server.hpp"
#include "init_templates.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <bitset>
#include <cmath>

// ─── Platform-specific POSIX headers ───
#ifdef _WIN32
    #include <direct.h>      // _getcwd
    #include <sys/stat.h>    // stat (works on Windows too)
    #define getcwd _getcwd
#else
    #include <sys/stat.h>
    #include <unistd.h>
#endif

const std::string BANTU_VERSION = "1.1.0";
const std::string BANTU_LANG = "Bantu";

// ─── Helpers ──────────────────────────────────────────────────────────

bool fileExists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
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
    std::cout << "  COMMANDS:\n";
    std::cout << "    bantu                  Start REPL\n";
    std::cout << "    bantu run <file.b>     Run a Bantu file\n";
    std::cout << "    bantu run              Run main.b in current dir\n";
    std::cout << "    bantu build <file.b>   Compile to executable\n";
    std::cout << "    bantu build            Build main.b in current dir\n";
    std::cout << "    bantu init <name>      Create a new Bantu project (CLI)\n";
    std::cout << "    bantu init --web <n>   Create a Sua web app (Spring Initializer-style)\n";
    std::cout << "    bantu new <name>       Alias of init\n";
    std::cout << "    bantu relay [port]    Start STUN/TURN relay server\n";
    std::cout << "    bantu --help           Show this help\n";
    std::cout << "    bantu --version        Show version\n";
    std::cout << "\n";
    std::cout << "  EXAMPLES:\n";
    std::cout << "    bantu run hello.b      Run hello.b\n";
    std::cout << "    bantu run              Run main.b\n";
    std::cout << "    bantu build app.b      Build app.b -> ./app\n";
    std::cout << "    bantu init myproject   Create new CLI project\n";
    std::cout << "    bantu init --web shop  Create new Sua web app 'shop'\n";
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

void runCode(const std::string& source, const std::string& filename = "<repl>") {
    auto start = std::chrono::high_resolution_clock::now();

    Lexer lexer(source);
    auto tokens = lexer.tokenize();

    Parser parser(std::move(tokens));
    auto ast = parser.parse();

    Evaluator evaluator;
    evaluator.evaluate(ast);

    auto end = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    if (filename != "<repl>") {
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
            std::cerr << "  Usage: bantu run <file.b>\n";
            return 1;
        }
    }

    if (!fileExists(path)) {
        std::cerr << "  [ERROR] File not found: " << path << "\n";
        return 1;
    }

    std::cout << "  Running: " << path << "\n";
    std::cout << "  ────────────────────────────\n";

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
        "  \"language\": \"bantu\",\n"
        "  \"bantuVersion\": \"" + BANTU_VERSION + "\"\n"
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
        std::string file = (argc >= 3) ? argv[2] : "";
        return cmdRun(file);
    }

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

    // ─── bantu <file.b> (legacy shorthand for run) ───
    if (command.size() > 2 && command.substr(command.size() - 2) == ".b") {
        return cmdRun(command);
    }

    // ─── Unknown command ───
    std::cerr << "  [ERROR] Unknown command: " << command << "\n";
    std::cerr << "  Run 'bantu --help' for available commands.\n";
    return 1;
}
