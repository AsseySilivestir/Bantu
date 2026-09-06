#pragma once
/**
 * linter.hpp — static analysis for Bantu source.
 *
 * Produces diagnostics WITHOUT executing the program, so an editor (the VS Code
 * extension) can show squiggles live and `bantu run`/`build` can refuse to run
 * code with errors. Two diagnostic severities:
 *   "error"   → red   (syntax errors; const reassignment) — blocks compilation
 *   "warning" → yellow (obvious literal type-annotation mismatches)
 */
#include "lexer.hpp"
#include "parser.hpp"
#include "ast.hpp"
#include <set>
#include <string>
#include <vector>

struct LintDiag {
    int line;
    int col;
    std::string severity;  // "error" | "warning"
    std::string message;
};

// Walk the AST collecting const-reassignment errors and literal type mismatches.
// `consts` holds the names currently declared `const` in scope; it is copied
// when descending into a function body so a function-local const does not leak.
inline void bantuLintWalk(const std::shared_ptr<ASTNode>& node,
                          std::set<std::string>& consts,
                          std::vector<LintDiag>& out) {
    if (!node) return;

    if (auto vd = dynamic_cast<VarDeclNode*>(node.get())) {
        if (vd->typeAnnotation == "const") {
            consts.insert(vd->name);
        } else if (vd->typeAnnotation == "number" || vd->typeAnnotation == "string" ||
                   vd->typeAnnotation == "bool") {
            const char* actual = nullptr;
            if (dynamic_cast<StringNode*>(vd->init.get()))      actual = "string";
            else if (dynamic_cast<NumberNode*>(vd->init.get())) actual = "number";
            else if (dynamic_cast<BoolNode*>(vd->init.get()))   actual = "bool";
            if (actual && vd->typeAnnotation != actual) {
                out.push_back({vd->line, vd->col, "warning",
                    "'" + vd->name + "' is declared " + vd->typeAnnotation +
                    " but assigned a " + actual + " literal"});
            }
        }
        bantuLintWalk(vd->init, consts, out);
        return;
    }
    if (auto as = dynamic_cast<AssignNode*>(node.get())) {
        if (consts.count(as->name)) {
            out.push_back({as->line, as->col, "error",
                "cannot reassign constant '" + as->name + "'"});
        }
        bantuLintWalk(as->value, consts, out);
        return;
    }

    auto walkBody = [&](std::vector<std::shared_ptr<ASTNode>>& b) {
        for (auto& s : b) bantuLintWalk(s, consts, out);
    };
    if (auto n = dynamic_cast<IfNode*>(node.get()))            { walkBody(n->body); walkBody(n->elseBody); }
    else if (auto n = dynamic_cast<WhileNode*>(node.get()))    { walkBody(n->body); }
    else if (auto n = dynamic_cast<ForNode*>(node.get()))      { walkBody(n->body); }
    else if (auto n = dynamic_cast<EachNode*>(node.get()))     { walkBody(n->body); }
    else if (auto n = dynamic_cast<TryCatchNode*>(node.get())) { walkBody(n->tryBody); walkBody(n->catchBody); }
    else if (auto n = dynamic_cast<SwitchNode*>(node.get()))   { for (auto& c : n->cases) walkBody(c.body); walkBody(n->defaultBody); }
    else if (auto n = dynamic_cast<FuncDeclNode*>(node.get())) {
        std::set<std::string> localConsts = consts;   // function-scoped
        for (auto& s : n->body) bantuLintWalk(s, localConsts, out);
    }
}

// Escape a string for embedding in JSON output.
inline std::string bantuJsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\t': o += "\\t";  break;
            case '\r': o += "\\r";  break;
            default:   o += c;
        }
    }
    return o;
}

// Lint a source string: syntax errors (from the recovering parser) + static
// checks. Never throws — lexer/parser errors are captured as diagnostics.
inline std::vector<LintDiag> bantuLintSource(const std::string& source) {
    std::vector<LintDiag> diags;
    try {
        Lexer lexer(source);
        auto tokens = lexer.tokenize();
        Parser parser(std::move(tokens));
        auto ast = parser.parse();
        for (auto& e : parser.errors()) diags.push_back({e.line, e.col, "error", e.message});
        std::set<std::string> consts;
        for (auto& n : ast) bantuLintWalk(n, consts, diags);
    } catch (const BantuError& e) {
        diags.push_back({e.line, e.col, "error", e.message});
    } catch (const std::exception& e) {
        diags.push_back({0, 0, "error", e.what()});
    }
    return diags;
}
