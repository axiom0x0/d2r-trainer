#include "config.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

// Minimal JSON parsing, handles only the flat structure of config.json

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n\"");
    size_t end   = s.find_last_not_of(" \t\r\n\"");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

static uintptr_t parse_hex(const std::string& s) {
    return std::stoull(trim(s), nullptr, 16);
}

// Recursive-descent JSON subset parser (strings, arrays, nested objects).
// Good enough for our config, not a general-purpose JSON library.

struct Token {
    enum Type { STRING, LBRACE, RBRACE, LBRACKET, RBRACKET, COLON, COMMA, END };
    Type type;
    std::string value;
};

class Lexer {
public:
    explicit Lexer(const std::string& input) : src(input), pos(0) {}

    Token next() {
        skip_whitespace();
        if (pos >= src.size()) return {Token::END, ""};

        char c = src[pos];
        switch (c) {
            case '{': pos++; return {Token::LBRACE, "{"};
            case '}': pos++; return {Token::RBRACE, "}"};
            case '[': pos++; return {Token::LBRACKET, "["};
            case ']': pos++; return {Token::RBRACKET, "]"};
            case ':': pos++; return {Token::COLON, ":"};
            case ',': pos++; return {Token::COMMA, ","};
            case '"': return read_string();
            default:
                throw std::runtime_error(std::string("Unexpected char: ") + c);
        }
    }

    Token peek() {
        size_t saved = pos;
        Token t = next();
        pos = saved;
        return t;
    }

private:
    std::string src;
    size_t pos;

    void skip_whitespace() {
        while (pos < src.size() && std::isspace(src[pos])) pos++;
    }

    Token read_string() {
        pos++; // skip opening "
        std::string val;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\') { pos++; }
            val += src[pos++];
        }
        pos++; // skip closing "
        return {Token::STRING, val};
    }
};

class ConfigParser {
public:
    explicit ConfigParser(const std::string& json) : lex(json) {}

    TrainerConfig parse() {
        TrainerConfig cfg;
        expect(Token::LBRACE);

        while (lex.peek().type != Token::RBRACE) {
            std::string key = expect(Token::STRING).value;
            expect(Token::COLON);

            if (key == "process_name") {
                cfg.process_name = expect(Token::STRING).value;
            } else if (key == "module") {
                cfg.module = expect(Token::STRING).value;
            } else if (key == "cheats") {
                cfg.cheats = parse_cheats();
            }

            if (lex.peek().type == Token::COMMA) lex.next();
        }
        expect(Token::RBRACE);
        return cfg;
    }

private:
    Lexer lex;

    Token expect(Token::Type type) {
        Token t = lex.next();
        if (t.type != type) {
            throw std::runtime_error("Unexpected token in config");
        }
        return t;
    }

    std::vector<CheatEntry> parse_cheats() {
        std::vector<CheatEntry> cheats;
        expect(Token::LBRACKET);

        while (lex.peek().type != Token::RBRACKET) {
            cheats.push_back(parse_cheat_entry());
            if (lex.peek().type == Token::COMMA) lex.next();
        }
        expect(Token::RBRACKET);
        return cheats;
    }

    CheatEntry parse_cheat_entry() {
        CheatEntry entry;
        expect(Token::LBRACE);

        while (lex.peek().type != Token::RBRACE) {
            std::string key = expect(Token::STRING).value;
            expect(Token::COLON);

            if (key == "name") {
                entry.name = expect(Token::STRING).value;
            } else if (key == "base_offset") {
                entry.base_offset = parse_hex(expect(Token::STRING).value);
            } else if (key == "offsets") {
                entry.offsets = parse_hex_array();
            } else if (key == "type") {
                entry.type = expect(Token::STRING).value;
            } else if (key == "god") {
                std::string val = expect(Token::STRING).value;
                entry.god = (val == "true" || val == "1" || val == "yes");
            } else if (key == "god_value") {
                entry.god_value = expect(Token::STRING).value;
            }

            if (lex.peek().type == Token::COMMA) lex.next();
        }
        expect(Token::RBRACE);
        return entry;
    }

    std::vector<uintptr_t> parse_hex_array() {
        std::vector<uintptr_t> arr;
        expect(Token::LBRACKET);

        while (lex.peek().type != Token::RBRACKET) {
            arr.push_back(parse_hex(expect(Token::STRING).value));
            if (lex.peek().type == Token::COMMA) lex.next();
        }
        expect(Token::RBRACKET);
        return arr;
    }
};

TrainerConfig load_config(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open config: " + path);
    }

    std::ostringstream ss;
    ss << file.rdbuf();

    ConfigParser parser(ss.str());
    return parser.parse();
}

size_t type_size(const std::string& type) {
    if (type == "int8")  return 1;
    if (type == "int16") return 2;
    if (type == "int32") return 4;
    if (type == "int64") return 8;
    if (type == "float") return 4;
    if (type == "double") return 8;
    return 4;
}
