#pragma once
/**
 * Bantu Language - High-Performance Lexer/Tokenizer
 * All keywords in English, optimized with perfect hash lookup
 */

#include "types.hpp"
#include <cctype>
#include <unordered_map>

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

class Lexer {
public:
    explicit Lexer(const std::string& source) : source_(source), pos_(0), line_(1), col_(1) {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        tokens.reserve(source_.size() / 4); // Pre-allocate for performance

        while (pos_ < source_.size()) {
            skipWhitespace();
            if (pos_ >= source_.size()) break;

            char c = source_[pos_];

            // Block comments /* */
            if (c == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '*') {
                skipBlockComment();
                continue;
            }

            // Line comments //
            if (c == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '/') {
                skipLineComment();
                continue;
            }

            // Strings
            if (c == '\'' || c == '"') {
                tokens.push_back(readString());
                continue;
            }

            // Numbers
            if (std::isdigit(static_cast<unsigned char>(c))) {
                tokens.push_back(readNumber());
                continue;
            }

            // Variable declarator $
            if (c == '$') {
                tokens.emplace_back(BantuTokenType::DECLARATOR, "$", line_, col_);
                advance();
                // The word after `$` is ALWAYS a variable name — even when it
                // spells a reserved word (e.g. $db, $create, $list, $switch).
                // The sigil disambiguates variables from keywords, so read the
                // following word as a raw IDENTIFIER (skipping keyword lookup).
                // This lets any word be used as a variable name.
                if (pos_ < source_.size() &&
                    (std::isalpha(static_cast<unsigned char>(source_[pos_])) || source_[pos_] == '_')) {
                    int idLine = line_, idCol = col_;
                    std::string name;
                    while (pos_ < source_.size() &&
                           (std::isalnum(static_cast<unsigned char>(source_[pos_])) || source_[pos_] == '_')) {
                        name += source_[pos_];
                        advance();
                    }
                    tokens.emplace_back(BantuTokenType::IDENTIFIER, name, idLine, idCol);
                }
                continue;
            }

            // Identifiers / Keywords
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                tokens.push_back(readIdentifier());
                continue;
            }

            // Three-character operators
            if (pos_ + 2 < source_.size()) {
                std::string threeChar = source_.substr(pos_, 3);
                if (threeChar == "===") { tokens.emplace_back(BantuTokenType::EQUALTO, "===", line_, col_); advance(); advance(); advance(); continue; }
                if (threeChar == "!==") { tokens.emplace_back(BantuTokenType::NOTEQUALTO, "!==", line_, col_); advance(); advance(); advance(); continue; }
            }

            // Two-character operators
            if (pos_ + 1 < source_.size()) {
                std::string twoChar = source_.substr(pos_, 2);
                if (twoChar == "=>")  { tokens.emplace_back(BantuTokenType::FATARROW, "=>", line_, col_); advance(); advance(); continue; }
                if (twoChar == "==")  { tokens.emplace_back(BantuTokenType::EQUALTO, "==", line_, col_); advance(); advance(); continue; }
                if (twoChar == "!=")  { tokens.emplace_back(BantuTokenType::NOTEQUALTO, "!=", line_, col_); advance(); advance(); continue; }
                if (twoChar == ">=")  { tokens.emplace_back(BantuTokenType::GREATERTHANEQUAL, ">=", line_, col_); advance(); advance(); continue; }
                if (twoChar == "<=")  { tokens.emplace_back(BantuTokenType::LESSTHANEQUAL, "<=", line_, col_); advance(); advance(); continue; }
                if (twoChar == "&&")  { tokens.emplace_back(BantuTokenType::AND, "&&", line_, col_); advance(); advance(); continue; }
                if (twoChar == "||")  { tokens.emplace_back(BantuTokenType::OR, "||", line_, col_); advance(); advance(); continue; }
                if (twoChar == "+=")  { tokens.emplace_back(BantuTokenType::PLUS_EQUALS, "+=", line_, col_); advance(); advance(); continue; }
                if (twoChar == "-=")  { tokens.emplace_back(BantuTokenType::MINUS_EQUALS, "-=", line_, col_); advance(); advance(); continue; }
                if (twoChar == "*=")  { tokens.emplace_back(BantuTokenType::MULTIPLY_EQUALS, "*=", line_, col_); advance(); advance(); continue; }
                if (twoChar == "/=")  { tokens.emplace_back(BantuTokenType::DIVIDE_EQUALS, "/=", line_, col_); advance(); advance(); continue; }
                if (twoChar == "++")  { tokens.emplace_back(BantuTokenType::INCREMENT, "++", line_, col_); advance(); advance(); continue; }
                if (twoChar == "--")  { tokens.emplace_back(BantuTokenType::DECREMENT, "--", line_, col_); advance(); advance(); continue; }
            }

            // Single-character tokens
            BantuTokenType tt = charToTokenType(c);
            if (tt != BantuTokenType::UNRECOGNIZED) {
                tokens.emplace_back(tt, std::string(1, c), line_, col_);
                advance();
                continue;
            }

            ErrorHandler::throwSyntaxError("Unrecognized character: " + std::string(1, c), line_, col_);
            advance();
        }

        tokens.emplace_back(BantuTokenType::END_OF_FILE, "", line_, col_);
        return tokens;
    }

private:
    std::string source_;
    size_t pos_;
    int line_;
    int col_;

    void advance() {
        if (pos_ < source_.size()) {
            if (source_[pos_] == '\n') { line_++; col_ = 1; }
            else { col_++; }
            pos_++;
        }
    }

    void skipWhitespace() {
        while (pos_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[pos_]))) {
            advance();
        }
    }

    void skipLineComment() {
        while (pos_ < source_.size() && source_[pos_] != '\n') advance();
    }

    void skipBlockComment() {
        advance(); advance(); // skip /*
        while (pos_ + 1 < source_.size()) {
            if (source_[pos_] == '*' && source_[pos_ + 1] == '/') {
                advance(); advance(); // skip */
                return;
            }
            advance();
        }
        ErrorHandler::throwSyntaxError("Unterminated block comment", line_, col_);
    }

    Token readString() {
        char quote = source_[pos_];
        int startLine = line_, startCol = col_;
        advance();
        std::string value;
        while (pos_ < source_.size() && source_[pos_] != quote) {
            if (source_[pos_] == '\\') {
                advance();
                if (pos_ < source_.size()) {
                    switch (source_[pos_]) {
                        case 'n': value += '\n'; break;
                        case 't': value += '\t'; break;
                        case 'r': value += '\r'; break;
                        case '\\': value += '\\'; break;
                        case '\'': value += '\''; break;
                        case '"': value += '"'; break;
                        case '0': value += '\0'; break;
                        default: value += source_[pos_]; break;
                    }
                }
            } else {
                value += source_[pos_];
            }
            advance();
        }
        if (pos_ < source_.size()) advance();
        return Token(BantuTokenType::STRING, value, startLine, startCol);
    }

    Token readNumber() {
        int startLine = line_, startCol = col_;
        std::string value;
        bool hasDecimal = false;
        while (pos_ < source_.size() && (std::isdigit(static_cast<unsigned char>(source_[pos_])) || source_[pos_] == '.')) {
            if (source_[pos_] == '.') {
                if (hasDecimal) break;
                // Check if next char is a digit (avoid treating 5.toString as number)
                if (pos_ + 1 < source_.size() && !std::isdigit(static_cast<unsigned char>(source_[pos_ + 1]))) break;
                hasDecimal = true;
            }
            value += source_[pos_];
            advance();
        }
        return Token(BantuTokenType::NUMBER, value, startLine, startCol);
    }

    Token readIdentifier() {
        int startLine = line_, startCol = col_;
        std::string value;
        while (pos_ < source_.size() && (std::isalnum(static_cast<unsigned char>(source_[pos_])) || source_[pos_] == '_')) {
            value += source_[pos_];
            advance();
        }
        BantuTokenType tt = keywordToTokenType(value);
        return Token(tt, value, startLine, startCol);
    }

    static BantuTokenType charToTokenType(char c) {
        switch (c) {
            case '+': return BantuTokenType::PLUS;
            case '-': return BantuTokenType::MINUS;
            case '*': return BantuTokenType::MULTIPLY;
            case '/': return BantuTokenType::DIVIDE;
            case '%': return BantuTokenType::MODULO;
            case '=': return BantuTokenType::EQUALS;
            case '>': return BantuTokenType::GREATERTHAN;
            case '<': return BantuTokenType::LESSTHAN;
            case '!': return BantuTokenType::NOT;
            case '(': return BantuTokenType::LPAREN;
            case ')': return BantuTokenType::RPAREN;
            case '{': return BantuTokenType::LBRACE;
            case '}': return BantuTokenType::RBRACE;
            case '[': return BantuTokenType::LBRACKET;
            case ']': return BantuTokenType::RBRACKET;
            case ':': return BantuTokenType::COLON;
            case '|': return BantuTokenType::PIPE;
            case ',': return BantuTokenType::COMMA;
            case ';': return BantuTokenType::SEMICOLON;
            case '.': return BantuTokenType::DOT;
            default: return BantuTokenType::UNRECOGNIZED;
        }
    }

    static BantuTokenType keywordToTokenType(const std::string& word) {
        static const std::unordered_map<std::string, BantuTokenType> keywords = {
            // Control flow
            {"if",       BantuTokenType::IF},
            {"else",     BantuTokenType::ELSE},
            {"while",    BantuTokenType::WHILE},
            {"for",      BantuTokenType::FOR},
            {"each",     BantuTokenType::EACH},
            {"in",       BantuTokenType::IN},
            {"switch",   BantuTokenType::SWITCH},
            {"case",     BantuTokenType::CASE},
            {"default",  BantuTokenType::DEFAULT},
            {"break",    BantuTokenType::BREAK},
            {"continue", BantuTokenType::CONTINUE},
            {"throw",    BantuTokenType::THROW},

            // Functions
            {"def",      BantuTokenType::DEF},
            {"return",   BantuTokenType::RETURN},

            // I/O
            {"print",    BantuTokenType::PRINT},
            {"read",     BantuTokenType::READ},
            {"db",       BantuTokenType::DB},
            {"fetch",    BantuTokenType::FETCH},
            {"await",    BantuTokenType::AWAIT},

            // Modifiers
            {"const",    BantuTokenType::CONST},
            {"private",  BantuTokenType::PRIVATE},
            {"public",   BantuTokenType::PUBLIC},
            {"from",     BantuTokenType::FROM},

            // Error handling
            {"try",      BantuTokenType::TRY},
            {"catch",    BantuTokenType::CATCH},

            // Object operations
            {"new",      BantuTokenType::NEW},
            {"create",   BantuTokenType::CREATE},
            {"delete",   BantuTokenType::DELETE},
            {"update",   BantuTokenType::UPDATE},
            {"calc",     BantuTokenType::CALC},

            // Class
            {"class",     BantuTokenType::CLASS},
            {"extends",   BantuTokenType::EXTENDS},
            {"implements",BantuTokenType::IMPLEMENTS},
            {"super",     BantuTokenType::SUPER},

            // Module
            {"import",   BantuTokenType::IMPORT},
            {"export",   BantuTokenType::EXPORT},
            // v1.2.1: module include (Bantu-style imports)
            {"include",  BantuTokenType::INCLUDE},

            // Real-Time (sua framework)
            {"channel",   BantuTokenType::CHANNEL},
            {"signal",    BantuTokenType::SIGNAL},
            {"stun",      BantuTokenType::STUN},
            {"stream",    BantuTokenType::STREAM},
            {"broadcast", BantuTokenType::BROADCAST},
            {"relay",     BantuTokenType::RELAY},
            {"connect",   BantuTokenType::CONNECT},
            {"peer",      BantuTokenType::PEER},
            {"ice",       BantuTokenType::ICE},
            {"candidate", BantuTokenType::CANDIDATE},
            {"room",      BantuTokenType::ROOM},
            {"offer",     BantuTokenType::OFFER},
            {"answer",    BantuTokenType::ANSWER},

            // Values
            {"true",     BantuTokenType::TRUE},
            {"false",    BantuTokenType::FALSE},
            {"null",     BantuTokenType::NULL_T},

            // Type annotations
            {"number",   BantuTokenType::TYPE_NUMBER},
            {"string",   BantuTokenType::TYPE_STRING},
            {"bool",     BantuTokenType::TYPE_BOOL},
            {"list",     BantuTokenType::TYPE_LIST},
            {"dict",     BantuTokenType::TYPE_DICT},
            {"any",      BantuTokenType::TYPE_ANY},
            {"func",     BantuTokenType::TYPE_FUNC},
        };
        auto it = keywords.find(word);
        if (it != keywords.end()) return it->second;
        return BantuTokenType::IDENTIFIER;
    }
};
