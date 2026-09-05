#pragma once
/**
 * Bantu Language - Recursive Descent Parser
 * Transforms token stream into AST
 */

#include "types.hpp"
#include "ast.hpp"
#include <vector>
#include <memory>
#include <stdexcept>

// Windows headers (pulled in transitively via <curl/curl.h> → <winsock2.h> → <windows.h>)
// define CONST/DELETE/TRUE/FALSE macros that clash with BantuTokenType enum values.
#ifdef CONST
#undef CONST
#endif
#ifdef DELETE
#undef DELETE
#endif
#ifdef TRUE
#undef TRUE
#endif
#ifdef FALSE
#undef FALSE
#endif
// IN is a Windows SAL annotation macro
#ifdef IN
#undef IN
#endif

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)), pos_(0) {}

    std::vector<std::shared_ptr<ASTNode>> parse() {
        std::vector<std::shared_ptr<ASTNode>> program;
        while (!isAtEnd()) {
            size_t before = pos_;
            try {
                auto stmt = parseStatement();
                if (stmt) program.push_back(std::move(stmt));
            } catch (const BantuError& e) {
                // Record the diagnostic and recover to the next statement so a
                // single mistake reports once instead of spinning the parser.
                errors_.push_back(e);
                synchronize();
                if (errors_.size() >= kMaxParseErrors) break;
            }
            // Defensive: guarantee forward progress even if some path consumed
            // no tokens, so a malformed program can never loop forever.
            if (pos_ == before && !isAtEnd()) advance();
        }
        return program;
    }

    // Diagnostics gathered during error-recovering parse — consumed by the
    // linter and by the run/build compile gate. Empty on clean source.
    const std::vector<BantuError>& errors() const { return errors_; }
    bool hasErrors() const { return !errors_.empty(); }

private:
    static constexpr size_t kMaxParseErrors = 200;
    std::vector<Token> tokens_;
    size_t pos_;
    std::vector<BantuError> errors_;

    // Skip ahead to the next plausible statement boundary after a syntax error
    // so one bad token doesn't cascade into a flood of bogus diagnostics.
    // Always advances at least one token, guaranteeing termination.
    void synchronize() {
        if (!isAtEnd()) advance();  // consume the offending token
        while (!isAtEnd()) {
            if (tokens_[pos_ - 1].type == BantuTokenType::SEMICOLON) return;
            switch (current().type) {
                case BantuTokenType::IF:
                case BantuTokenType::WHILE:
                case BantuTokenType::FOR:
                case BantuTokenType::EACH:
                case BantuTokenType::DEF:
                case BantuTokenType::RETURN:
                case BantuTokenType::PRINT:
                case BantuTokenType::TRY:
                case BantuTokenType::THROW:
                case BantuTokenType::SWITCH:
                case BantuTokenType::CLASS:
                case BantuTokenType::CONST:
                case BantuTokenType::INCLUDE:
                case BantuTokenType::RBRACE:
                    return;
                default:
                    break;
            }
            advance();
        }
    }

    // ─── Helpers ───
    const Token& current() const { return tokens_[pos_]; }
    const Token& peek(int offset = 0) const { return tokens_[pos_ + offset]; }
    bool isAtEnd() const { return current().type == BantuTokenType::END_OF_FILE; }

    Token advance() {
        if (!isAtEnd()) pos_++;
        return tokens_[pos_ - 1];
    }

    bool check(BantuTokenType type) const { return !isAtEnd() && current().type == type; }

    bool match(BantuTokenType type) {
        if (check(type)) { advance(); return true; }
        return false;
    }

    Token expect(BantuTokenType type, const std::string& msg) {
        if (check(type)) return advance();
        ErrorHandler::throwSyntaxError(msg + " (got: '" + current().value + "')", current().line, current().col);
        return current();
    }

    // ─── Statement Parsing ───
    std::shared_ptr<ASTNode> parseStatement() {
        if (check(BantuTokenType::IF))       return parseIf();
        if (check(BantuTokenType::WHILE))    return parseWhile();
        if (check(BantuTokenType::FOR))      return parseFor();
        if (check(BantuTokenType::EACH))     return parseEach();
        if (check(BantuTokenType::DEF))      return parseFuncDecl();
        if (check(BantuTokenType::RETURN))   return parseReturn();
        if (check(BantuTokenType::PRINT))    return parsePrint();
        if (check(BantuTokenType::TRY))      return parseTryCatch();
        if (check(BantuTokenType::THROW))    return parseThrow();
        if (check(BantuTokenType::SWITCH))   return parseSwitch();
        if (check(BantuTokenType::BREAK))    { int l = current().line, c = current().col; advance(); match(BantuTokenType::SEMICOLON); return std::make_shared<BreakNode>(l, c); }
        if (check(BantuTokenType::CONTINUE)) { int l = current().line, c = current().col; advance(); match(BantuTokenType::SEMICOLON); return std::make_shared<ContinueNode>(l, c); }
        if (check(BantuTokenType::CLASS))    return parseClass();
        if (check(BantuTokenType::CONST))    return parseConstDecl();
        // v1.2.1: module include
        if (check(BantuTokenType::INCLUDE))  return parseInclude();

        // Type-annotated variable declarations: number, string, bool, list, dict
        if (check(BantuTokenType::TYPE_NUMBER) || check(BantuTokenType::TYPE_STRING) ||
            check(BantuTokenType::TYPE_BOOL)   || check(BantuTokenType::TYPE_LIST) ||
            check(BantuTokenType::TYPE_DICT)   || check(BantuTokenType::TYPE_ANY) ||
            check(BantuTokenType::TYPE_FUNC))  return parseTypeDecl();

        return parseExpressionStatement();
    }

    // v1.2.1 — include statement
    //   include "./routes.b";
    //   include "./controller.b" as ctrl;
    std::shared_ptr<ASTNode> parseInclude() {
        int line = current().line, col = current().col;
        advance(); // skip 'include'
        auto pathTok = expect(BantuTokenType::STRING, "Expected string path after 'include'");
        std::string alias;
        // optional `as <name>` clause
        if (check(BantuTokenType::IDENTIFIER) && current().value == "as") {
            advance(); // skip 'as'
            auto aliasTok = expect(BantuTokenType::IDENTIFIER, "Expected alias name after 'as'");
            alias = aliasTok.value;
        }
        match(BantuTokenType::SEMICOLON);
        return std::make_shared<IncludeNode>(pathTok.value, alias, line, col);
    }

    std::shared_ptr<ASTNode> parseIf() {
        int line = current().line, col = current().col;
        advance(); // skip 'if'
        expect(BantuTokenType::LPAREN, "Expected '(' after 'if'");
        auto condition = parseExpression();
        expect(BantuTokenType::RPAREN, "Expected ')' after condition");
        auto body = parseBlock();

        std::vector<std::shared_ptr<ASTNode>> elseBody;
        if (match(BantuTokenType::ELSE)) {
            elseBody = parseBlock();
        }

        return std::make_shared<IfNode>(std::move(condition), std::move(body), std::move(elseBody), line, col);
    }

    std::shared_ptr<ASTNode> parseWhile() {
        int line = current().line, col = current().col;
        advance(); // skip 'while'
        expect(BantuTokenType::LPAREN, "Expected '(' after 'while'");
        auto condition = parseExpression();
        expect(BantuTokenType::RPAREN, "Expected ')' after condition");
        auto body = parseBlock();
        return std::make_shared<WhileNode>(std::move(condition), std::move(body), line, col);
    }

    std::shared_ptr<ASTNode> parseFor() {
        int line = current().line, col = current().col;
        advance(); // skip 'for'

        // Two shapes, disambiguated by the token right after `for`:
        //   C-style:      for (init; cond; update) { ... }
        //   Python-style: for $x in <iter> { ... }
        //                 for $k, $v in $dict.items() { ... }
        if (check(BantuTokenType::LPAREN)) {
            advance(); // (
            auto init = parseStatement();
            match(BantuTokenType::SEMICOLON);
            auto condition = parseExpression();
            expect(BantuTokenType::SEMICOLON, "Expected ';' after for-condition");
            auto update = parseStatement();
            expect(BantuTokenType::RPAREN, "Expected ')' after for-update");
            auto body = parseBlock();
            return std::make_shared<ForNode>(std::move(init), std::move(condition), std::move(update), std::move(body), line, col);
        }

        // for-in form (reuses the EachNode machinery, optionally with 2 vars).
        match(BantuTokenType::DECLARATOR); // optional '$'
        auto var1 = expect(BantuTokenType::IDENTIFIER, "Expected loop variable after 'for'");
        std::string valueVar;
        if (match(BantuTokenType::COMMA)) {
            match(BantuTokenType::DECLARATOR); // optional '$'
            auto var2 = expect(BantuTokenType::IDENTIFIER, "Expected second loop variable after ','");
            valueVar = var2.value;
        }
        expect(BantuTokenType::IN, "Expected 'in' in for-loop");
        auto iterable = parseExpression();
        auto body = parseBlock();
        return std::make_shared<EachNode>(var1.value, std::move(iterable), std::move(body), line, col, valueVar);
    }

    std::shared_ptr<ASTNode> parseEach() {
        int line = current().line, col = current().col;
        advance(); // skip 'each'
        expect(BantuTokenType::LPAREN, "Expected '(' after 'each'");
        expect(BantuTokenType::DECLARATOR, "Expected '$' before variable name in each");
        auto varToken = expect(BantuTokenType::IDENTIFIER, "Expected variable name in each");
        // Optional 2nd variable: each ($k, $v in $dict) { ... }
        std::string valueVar;
        if (match(BantuTokenType::COMMA)) {
            match(BantuTokenType::DECLARATOR);
            auto var2 = expect(BantuTokenType::IDENTIFIER, "Expected second variable name after ','");
            valueVar = var2.value;
        }
        expect(BantuTokenType::IN, "Expected 'in' in each loop");
        auto iterable = parseExpression();
        expect(BantuTokenType::RPAREN, "Expected ')' after each iterable");
        auto body = parseBlock();
        return std::make_shared<EachNode>(varToken.value, std::move(iterable), std::move(body), line, col, valueVar);
    }

    std::shared_ptr<ASTNode> parseFuncDecl() {
        int line = current().line, col = current().col;
        advance(); // skip 'def'
        // Name is optional: `def name(...)` is a declaration, `def(...)` is an
        // anonymous function expression (name left empty).
        std::string fname;
        if (check(BantuTokenType::IDENTIFIER)) {
            fname = advance().value;
        }
        expect(BantuTokenType::LPAREN, "Expected '(' after function name");

        std::vector<std::string> params;
        if (!check(BantuTokenType::RPAREN)) {
            do {
                if (match(BantuTokenType::DECLARATOR)) { /* skip $ */ }
                auto param = expect(BantuTokenType::IDENTIFIER, "Expected parameter name");
                params.push_back(param.value);
            } while (match(BantuTokenType::COMMA));
        }
        expect(BantuTokenType::RPAREN, "Expected ')' after parameters");
        auto body = parseBlock();
        return std::make_shared<FuncDeclNode>(fname, std::move(params), std::move(body), line, col);
    }

    std::shared_ptr<ASTNode> parseReturn() {
        int line = current().line, col = current().col;
        advance(); // skip 'return'
        // Bare `return;` (or `return` at the end of a block) yields null.
        std::shared_ptr<ASTNode> value;
        if (check(BantuTokenType::SEMICOLON) || check(BantuTokenType::RBRACE) || isAtEnd()) {
            value = std::make_shared<NullNode>(line, col);
        } else {
            value = parseExpression();
        }
        match(BantuTokenType::SEMICOLON);
        return std::make_shared<ReturnNode>(std::move(value), line, col);
    }

    std::shared_ptr<ASTNode> parsePrint() {
        int line = current().line, col = current().col;
        advance(); // skip 'print'
        auto value = parseExpression();
        match(BantuTokenType::SEMICOLON);
        return std::make_shared<PrintNode>(std::move(value), line, col);
    }

    std::shared_ptr<ASTNode> parseTryCatch() {
        int line = current().line, col = current().col;
        advance(); // skip 'try'
        auto tryBody = parseBlock();
        expect(BantuTokenType::CATCH, "Expected 'catch' after try block");
        expect(BantuTokenType::LPAREN, "Expected '(' after 'catch'");
        match(BantuTokenType::DECLARATOR);
        auto catchVar = expect(BantuTokenType::IDENTIFIER, "Expected error variable name");
        expect(BantuTokenType::RPAREN, "Expected ')' after catch variable");
        auto catchBody = parseBlock();
        return std::make_shared<TryCatchNode>(std::move(tryBody), catchVar.value, std::move(catchBody), line, col);
    }

    // throw <expr>;  — raises a catchable value.
    std::shared_ptr<ASTNode> parseThrow() {
        int line = current().line, col = current().col;
        advance(); // skip 'throw'
        auto value = parseExpression();
        match(BantuTokenType::SEMICOLON);
        return std::make_shared<ThrowNode>(std::move(value), line, col);
    }

    // switch ($subject) { case <expr> { ... } case <expr> { ... } default { ... } }
    // Braces are mandatory (Bantu style); there is NO fallthrough — the first
    // matching case runs and control leaves the switch.
    std::shared_ptr<ASTNode> parseSwitch() {
        int line = current().line, col = current().col;
        advance(); // skip 'switch'
        expect(BantuTokenType::LPAREN, "Expected '(' after 'switch'");
        auto subject = parseExpression();
        expect(BantuTokenType::RPAREN, "Expected ')' after switch subject");
        expect(BantuTokenType::LBRACE, "Expected '{' to start switch body");

        std::vector<SwitchCase> cases;
        std::vector<std::shared_ptr<ASTNode>> defaultBody;
        bool hasDefault = false;

        while (!check(BantuTokenType::RBRACE) && !isAtEnd()) {
            if (match(BantuTokenType::CASE)) {
                SwitchCase sc;
                sc.value = parseExpression();
                sc.body  = parseBlock();
                cases.push_back(std::move(sc));
            } else if (match(BantuTokenType::DEFAULT)) {
                defaultBody = parseBlock();
                hasDefault = true;
            } else {
                ErrorHandler::throwSyntaxError("Expected 'case' or 'default' inside switch body",
                                               current().line, current().col);
            }
        }
        expect(BantuTokenType::RBRACE, "Expected '}' to close switch");
        return std::make_shared<SwitchNode>(std::move(subject), std::move(cases),
                                            std::move(defaultBody), hasDefault, line, col);
    }

    std::shared_ptr<ASTNode> parseClass() {
        int line = current().line, col = current().col;
        advance(); // skip 'class'
        auto nameToken = expect(BantuTokenType::IDENTIFIER, "Expected class name after 'class'");

        // Parse optional extends (single inheritance)
        std::string parentClass = "";
        if (match(BantuTokenType::EXTENDS)) {
            auto parentToken = expect(BantuTokenType::IDENTIFIER, "Expected parent class name after 'extends'");
            parentClass = parentToken.value;
        }

        // Parse optional implements (multiple inheritance)
        std::vector<std::string> implementsClasses;
        if (match(BantuTokenType::IMPLEMENTS)) {
            do {
                auto implToken = expect(BantuTokenType::IDENTIFIER, "Expected class name after 'implements'");
                implementsClasses.push_back(implToken.value);
            } while (match(BantuTokenType::COMMA));
        }

        // Parse class body
        expect(BantuTokenType::LBRACE, "Expected '{' to start class body");

        std::vector<std::shared_ptr<ASTNode>> body;
        while (!check(BantuTokenType::RBRACE) && !check(BantuTokenType::END_OF_FILE)) {
            // Parse method definitions (def name(...) { ... })
            if (check(BantuTokenType::DEF)) {
                body.push_back(parseFuncDecl());
            } else {
                // Parse property declarations or other statements
                body.push_back(parseStatement());
            }
        }

        expect(BantuTokenType::RBRACE, "Expected '}' to close class body");

        return std::make_shared<ClassDeclNode>(nameToken.value, parentClass,
            std::move(implementsClasses), std::move(body), line, col);
    }

    std::shared_ptr<ASTNode> parseConstDecl() {
        int line = current().line, col = current().col;
        advance(); // skip 'const'
        match(BantuTokenType::DECLARATOR);
        auto nameToken = expect(BantuTokenType::IDENTIFIER, "Expected variable name");
        expect(BantuTokenType::EQUALS, "Expected '=' in const declaration");
        auto init = parseExpression();
        match(BantuTokenType::SEMICOLON);
        return std::make_shared<VarDeclNode>("const", nameToken.value, std::move(init), line, col);
    }

    std::shared_ptr<ASTNode> parseTypeDecl() {
        int line = current().line, col = current().col;
        std::string typeAnnotation = current().value;
        advance(); // skip type keyword
        match(BantuTokenType::DECLARATOR);
        auto nameToken = expect(BantuTokenType::IDENTIFIER, "Expected variable name");
        expect(BantuTokenType::EQUALS, "Expected '=' in variable declaration");
        auto init = parseExpression();
        match(BantuTokenType::SEMICOLON);
        return std::make_shared<VarDeclNode>(typeAnnotation, nameToken.value, std::move(init), line, col);
    }

    std::shared_ptr<ASTNode> parseExpressionStatement() {
        auto expr = parseExpression();
        match(BantuTokenType::SEMICOLON);
        return expr;
    }

    // ─── Block Parsing ───
    std::vector<std::shared_ptr<ASTNode>> parseBlock() {
        std::vector<std::shared_ptr<ASTNode>> statements;
        expect(BantuTokenType::LBRACE, "Expected '{'");
        while (!check(BantuTokenType::RBRACE) && !isAtEnd()) {
            statements.push_back(parseStatement());
        }
        expect(BantuTokenType::RBRACE, "Expected '}'");
        return statements;
    }

    // ─── Expression Parsing (Precedence Climbing) ───
    std::shared_ptr<ASTNode> parseExpression() {
        return parseAssignment();
    }

    std::shared_ptr<ASTNode> parseAssignment() {
        auto expr = parseOr();

        if (match(BantuTokenType::EQUALS)) {
            auto value = parseAssignment();
            if (auto varNode = dynamic_cast<VariableNode*>(expr.get())) {
                return std::make_shared<AssignNode>(varNode->name, std::move(value), expr->line, expr->col);
            }
            // Handle $arr[$idx] = value
            if (auto idxNode = dynamic_cast<IndexAccessNode*>(expr.get())) {
                return std::make_shared<IndexAssignNode>(std::move(idxNode->object), std::move(idxNode->index), std::move(value), expr->line, expr->col);
            }
            // Handle $dict["key"] = value
            if (auto dotNode = dynamic_cast<DotAccessNode*>(expr.get())) {
                return std::make_shared<DictAssignNode>(std::move(dotNode->object), dotNode->property, std::move(value), expr->line, expr->col);
            }
            ErrorHandler::throwSyntaxError("Invalid assignment target", expr->line, expr->col);
        }

        if (match(BantuTokenType::PLUS_EQUALS) || match(BantuTokenType::MINUS_EQUALS) ||
            match(BantuTokenType::MULTIPLY_EQUALS) || match(BantuTokenType::DIVIDE_EQUALS)) {
            // Compound assignment: `x op= v`  ≡  `x = x op v`.
            // Bug fix (v1.3.0): map the compound token to its BASE arithmetic
            // operator. The old code passed PLUS_EQUALS straight into the
            // BinaryOpNode, which the evaluator rejected as "Unknown operator"
            // (and, inside a loop, spun forever because the counter never moved).
            BantuTokenType compound = tokens_[pos_ - 1].type;
            BantuTokenType baseOp = BantuTokenType::PLUS;
            if (compound == BantuTokenType::MINUS_EQUALS)         baseOp = BantuTokenType::MINUS;
            else if (compound == BantuTokenType::MULTIPLY_EQUALS) baseOp = BantuTokenType::MULTIPLY;
            else if (compound == BantuTokenType::DIVIDE_EQUALS)   baseOp = BantuTokenType::DIVIDE;

            auto value = parseAssignment();

            // Target: simple variable — $x op= v
            if (auto varNode = dynamic_cast<VariableNode*>(expr.get())) {
                auto read = std::make_shared<VariableNode>(varNode->name, varNode->line, varNode->col);
                auto binOp = std::make_shared<BinaryOpNode>(baseOp, read, std::move(value), expr->line, expr->col);
                return std::make_shared<AssignNode>(varNode->name, std::move(binOp), expr->line, expr->col);
            }
            // Target: list/dict index — $a[i] op= v
            if (auto idxNode = dynamic_cast<IndexAccessNode*>(expr.get())) {
                auto read = std::make_shared<IndexAccessNode>(idxNode->object, idxNode->index, expr->line, expr->col);
                auto binOp = std::make_shared<BinaryOpNode>(baseOp, read, std::move(value), expr->line, expr->col);
                return std::make_shared<IndexAssignNode>(idxNode->object, idxNode->index, std::move(binOp), expr->line, expr->col);
            }
            // Target: object property — $o.k op= v
            if (auto dotNode = dynamic_cast<DotAccessNode*>(expr.get())) {
                auto read = std::make_shared<DotAccessNode>(dotNode->object, dotNode->property, expr->line, expr->col);
                auto binOp = std::make_shared<BinaryOpNode>(baseOp, read, std::move(value), expr->line, expr->col);
                return std::make_shared<DictAssignNode>(dotNode->object, dotNode->property, std::move(binOp), expr->line, expr->col);
            }
            ErrorHandler::throwSyntaxError("Invalid compound-assignment target", expr->line, expr->col);
        }

        return expr;
    }

    std::shared_ptr<ASTNode> parseOr() {
        auto left = parseAnd();
        while (match(BantuTokenType::OR)) {
            auto right = parseAnd();
            left = std::make_shared<BinaryOpNode>(BantuTokenType::OR, std::move(left), std::move(right), current().line, current().col);
        }
        return left;
    }

    std::shared_ptr<ASTNode> parseAnd() {
        auto left = parseEquality();
        while (match(BantuTokenType::AND)) {
            auto right = parseEquality();
            left = std::make_shared<BinaryOpNode>(BantuTokenType::AND, std::move(left), std::move(right), current().line, current().col);
        }
        return left;
    }

    std::shared_ptr<ASTNode> parseEquality() {
        auto left = parseComparison();
        while (check(BantuTokenType::EQUALTO) || check(BantuTokenType::NOTEQUALTO)) {
            auto op = advance().type;
            auto right = parseComparison();
            left = std::make_shared<BinaryOpNode>(op, std::move(left), std::move(right), current().line, current().col);
        }
        return left;
    }

    std::shared_ptr<ASTNode> parseComparison() {
        auto left = parseAddition();
        while (check(BantuTokenType::GREATERTHAN) || check(BantuTokenType::GREATERTHANEQUAL) ||
               check(BantuTokenType::LESSTHAN) || check(BantuTokenType::LESSTHANEQUAL)) {
            auto op = advance().type;
            auto right = parseAddition();
            left = std::make_shared<BinaryOpNode>(op, std::move(left), std::move(right), current().line, current().col);
        }
        return left;
    }

    std::shared_ptr<ASTNode> parseAddition() {
        auto left = parseMultiplication();
        while (check(BantuTokenType::PLUS) || check(BantuTokenType::MINUS)) {
            auto op = advance().type;
            auto right = parseMultiplication();
            left = std::make_shared<BinaryOpNode>(op, std::move(left), std::move(right), current().line, current().col);
        }
        return left;
    }

    std::shared_ptr<ASTNode> parseMultiplication() {
        auto left = parseUnary();
        while (check(BantuTokenType::MULTIPLY) || check(BantuTokenType::DIVIDE) || check(BantuTokenType::MODULO)) {
            auto op = advance().type;
            auto right = parseUnary();
            left = std::make_shared<BinaryOpNode>(op, std::move(left), std::move(right), current().line, current().col);
        }
        return left;
    }

    std::shared_ptr<ASTNode> parseUnary() {
        if (check(BantuTokenType::NOT) || check(BantuTokenType::MINUS)) {
            auto op = advance().type;
            auto operand = parseUnary();
            return std::make_shared<UnaryOpNode>(op, std::move(operand), current().line, current().col);
        }
        return parseCall();
    }

    std::shared_ptr<ASTNode> parseCall() {
        auto expr = parsePrimary();

        while (true) {
            if (match(BantuTokenType::LPAREN)) {
                std::vector<std::shared_ptr<ASTNode>> args;
                if (!check(BantuTokenType::RPAREN)) {
                    do {
                        args.push_back(parseExpression());
                    } while (match(BantuTokenType::COMMA));
                }
                expect(BantuTokenType::RPAREN, "Expected ')' after arguments");
                expr = std::make_shared<CallNode>(std::move(expr), std::move(args), current().line, current().col);
            } else if (match(BantuTokenType::DOT)) {
                // Allow keywords as property names after '.'
                std::string propName;
                if (check(BantuTokenType::IDENTIFIER)) {
                    propName = advance().value;
                } else {
                    // Accept any keyword-like token as a property name
                    auto& tok = current();
                    // Check if it's a keyword that can be used as property name
                    switch (tok.type) {
                        case BantuTokenType::CHANNEL:
                        case BantuTokenType::SIGNAL:
                        case BantuTokenType::STUN:
                        case BantuTokenType::STREAM:
                        case BantuTokenType::BROADCAST:
                        case BantuTokenType::RELAY:
                        case BantuTokenType::CONNECT:
                        case BantuTokenType::PEER:
                        case BantuTokenType::ICE:
                        case BantuTokenType::CANDIDATE:
                        case BantuTokenType::ROOM:
                        case BantuTokenType::OFFER:
                        case BantuTokenType::ANSWER:
                        case BantuTokenType::DELETE:
                        case BantuTokenType::UPDATE:
                        case BantuTokenType::CREATE:
                        case BantuTokenType::NEW:
                        case BantuTokenType::DB:
                        case BantuTokenType::FETCH:
                        case BantuTokenType::FROM:
                            propName = advance().value;
                            break;
                        default:
                            ErrorHandler::throwSyntaxError("Expected property name after '.'", tok.line, tok.col);
                            break;
                    }
                }
                expr = std::make_shared<DotAccessNode>(std::move(expr), propName, current().line, current().col);
            } else if (match(BantuTokenType::LBRACKET)) {
                auto index = parseExpression();
                expect(BantuTokenType::RBRACKET, "Expected ']'");
                expr = std::make_shared<IndexAccessNode>(std::move(expr), std::move(index), current().line, current().col);
            } else {
                break;
            }
        }

        return expr;
    }

    std::shared_ptr<ASTNode> parsePrimary() {
        // Anonymous function expression:  def($a, $b) { ... }
        // (functions are first-class values, so `def` may appear in expression
        //  position — e.g. as a dict value or argument.)
        if (check(BantuTokenType::DEF)) {
            return parseFuncDecl();
        }
        // `func` used in expression/call position is the FFI builtin (the FUNC
        // keyword otherwise introduces a `func $x = ...` typed declaration at
        // statement level, which is handled before we ever get here).
        if (match(BantuTokenType::TYPE_FUNC)) {
            return std::make_shared<VariableNode>("func", tokens_[pos_-1].line, tokens_[pos_-1].col);
        }
        if (match(BantuTokenType::NUMBER)) {
            try { return std::make_shared<NumberNode>(std::stod(tokens_[pos_-1].value), tokens_[pos_-1].line, tokens_[pos_-1].col); }
            catch (...) { return std::make_shared<NumberNode>(0, tokens_[pos_-1].line, tokens_[pos_-1].col); }
        }
        if (match(BantuTokenType::STRING)) {
            return std::make_shared<StringNode>(tokens_[pos_-1].value, tokens_[pos_-1].line, tokens_[pos_-1].col);
        }
        if (match(BantuTokenType::TRUE))  return std::make_shared<BoolNode>(true, tokens_[pos_-1].line, tokens_[pos_-1].col);
        if (match(BantuTokenType::FALSE)) return std::make_shared<BoolNode>(false, tokens_[pos_-1].line, tokens_[pos_-1].col);
        if (match(BantuTokenType::NULL_T)) return std::make_shared<NullNode>(tokens_[pos_-1].line, tokens_[pos_-1].col);

        if (match(BantuTokenType::DECLARATOR)) {
            auto name = expect(BantuTokenType::IDENTIFIER, "Expected variable name after '$'");
            return std::make_shared<VariableNode>(name.value, name.line, name.col);
        }

        if (match(BantuTokenType::IDENTIFIER)) {
            return std::make_shared<VariableNode>(tokens_[pos_-1].value, tokens_[pos_-1].line, tokens_[pos_-1].col);
        }

        // Keywords that can be used as identifiers (global objects: db, fetch, http, json)
        if (match(BantuTokenType::DB)) {
            return std::make_shared<VariableNode>("db", tokens_[pos_-1].line, tokens_[pos_-1].col);
        }
        if (match(BantuTokenType::FETCH)) {
            return std::make_shared<VariableNode>("fetch", tokens_[pos_-1].line, tokens_[pos_-1].col);
        }

        // new ClassName(args) — instantiation
        if (match(BantuTokenType::NEW)) {
            int nLine = tokens_[pos_-1].line, nCol = tokens_[pos_-1].col;
            auto className = expect(BantuTokenType::IDENTIFIER, "Expected class name after 'new'");
            // The CallNode in parseCall will handle the (args) part
            // We return a VariableNode for the class name, and parseCall wraps it
            return std::make_shared<VariableNode>(className.value, nLine, nCol);
        }

        // super() call for parent constructor
        if (match(BantuTokenType::SUPER)) {
            int sLine = tokens_[pos_-1].line, sCol = tokens_[pos_-1].col;
            if (match(BantuTokenType::LPAREN)) {
                std::vector<std::shared_ptr<ASTNode>> args;
                if (!check(BantuTokenType::RPAREN)) {
                    do { args.push_back(parseExpression()); } while (match(BantuTokenType::COMMA));
                }
                expect(BantuTokenType::RPAREN, "Expected ')' after super() arguments");
                return std::make_shared<SuperNode>(std::move(args), sLine, sCol);
            }
            // super.method() — treat as variable "super" for dot access
            return std::make_shared<VariableNode>("super", sLine, sCol);
        }

        if (match(BantuTokenType::LPAREN)) {
            auto expr = parseExpression();
            expect(BantuTokenType::RPAREN, "Expected ')'");
            return expr;
        }

        if (match(BantuTokenType::LBRACKET)) {
            std::vector<std::shared_ptr<ASTNode>> elements;
            if (!check(BantuTokenType::RBRACKET)) {
                do { elements.push_back(parseExpression()); } while (match(BantuTokenType::COMMA));
            }
            expect(BantuTokenType::RBRACKET, "Expected ']'");
            return std::make_shared<ListNode>(std::move(elements), tokens_[pos_-1].line, tokens_[pos_-1].col);
        }

        if (match(BantuTokenType::LBRACE)) {
            std::vector<std::pair<std::string, std::shared_ptr<ASTNode>>> pairs;
            if (!check(BantuTokenType::RBRACE)) {
                do {
                    auto key = expect(BantuTokenType::STRING, "Expected string key in dict");
                    expect(BantuTokenType::COLON, "Expected ':' after key");
                    auto val = parseExpression();
                    pairs.emplace_back(key.value, std::move(val));
                } while (match(BantuTokenType::COMMA));
            }
            expect(BantuTokenType::RBRACE, "Expected '}'");
            return std::make_shared<DictNode>(std::move(pairs), tokens_[pos_-1].line, tokens_[pos_-1].col);
        }

        ErrorHandler::throwSyntaxError("Unexpected token: '" + current().value + "'", current().line, current().col);
        return std::make_shared<NullNode>(current().line, current().col);
    }
};
