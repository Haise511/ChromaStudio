#include "project_io/Json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <stdexcept>

namespace chroma::json {

// ── Value ────────────────────────────────────────────────────────────────────

Value Value::boolean(bool b) {
    Value v;
    v.type_ = Type::Bool;
    v.bool_ = b;
    return v;
}

Value Value::number(double n) {
    Value v;
    v.type_ = Type::Number;
    v.number_ = n;
    return v;
}

Value Value::string(std::string s) {
    Value v;
    v.type_ = Type::String;
    v.string_ = std::move(s);
    return v;
}

Value Value::array() {
    Value v;
    v.type_ = Type::Array;
    return v;
}

Value Value::object() {
    Value v;
    v.type_ = Type::Object;
    return v;
}

namespace {
[[noreturn]] void typeError(const char* expected) { throw std::runtime_error(std::string("json: expected ") + expected); }
}  // namespace

bool Value::asBool() const {
    if (type_ != Type::Bool) typeError("a boolean");
    return bool_;
}

double Value::asNumber() const {
    if (type_ != Type::Number) typeError("a number");
    return number_;
}

const std::string& Value::asString() const {
    if (type_ != Type::String) typeError("a string");
    return string_;
}

std::optional<std::int64_t> Value::asInteger(std::int64_t lo, std::int64_t hi) const {
    if (type_ != Type::Number || !std::isfinite(number_) || number_ != std::floor(number_)) return std::nullopt;
    if (number_ < static_cast<double>(lo) || number_ > static_cast<double>(hi)) return std::nullopt;
    return static_cast<std::int64_t>(number_);
}

const std::vector<Value>& Value::items() const {
    if (type_ != Type::Array) typeError("an array");
    return items_;
}

Value& Value::push(Value v) {
    if (type_ != Type::Array) typeError("an array");
    items_.push_back(std::move(v));
    return items_.back();
}

const std::vector<std::string>& Value::keys() const {
    if (type_ != Type::Object) typeError("an object");
    return keys_;
}

const Value* Value::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) return &items_[i];
    }
    return nullptr;
}

Value& Value::set(std::string key, Value v) {
    if (type_ != Type::Object) typeError("an object");
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) return items_[i] = std::move(v);
    }
    keys_.push_back(std::move(key));
    items_.push_back(std::move(v));
    return items_.back();
}

// ── Parser ───────────────────────────────────────────────────────────────────

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Value document() {
        skipSpace();
        Value v = value(0);
        skipSpace();
        if (pos_ != text_.size()) error("unexpected data after the document");
        return v;
    }

private:
    static constexpr int kMaxDepth = 128;

    [[noreturn]] void error(const char* what) const {
        throw std::runtime_error(std::format("json: {} at byte {}", what, pos_));
    }

    void skipSpace() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool consume(std::string_view literal) {
        if (text_.substr(pos_, literal.size()) != literal) return false;
        pos_ += literal.size();
        return true;
    }

    Value value(int depth) {
        if (depth > kMaxDepth) error("nesting too deep");
        if (pos_ >= text_.size()) error("unexpected end");
        const char c = text_[pos_];
        if (c == '{') return object(depth);
        if (c == '[') return array(depth);
        if (c == '"') return Value::string(string());
        if (consume("true")) return Value::boolean(true);
        if (consume("false")) return Value::boolean(false);
        if (consume("null")) return Value::null();
        return numberValue();
    }

    Value object(int depth) {
        ++pos_;
        Value v = Value::object();
        skipSpace();
        if (consume("}")) return v;
        for (;;) {
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '"') error("expected a key");
            std::string key = string();
            if (v.find(key)) error("duplicate key");
            skipSpace();
            if (!consume(":")) error("expected ':'");
            skipSpace();
            v.set(std::move(key), value(depth + 1));
            skipSpace();
            if (consume("}")) return v;
            if (!consume(",")) error("expected ',' or '}'");
        }
    }

    Value array(int depth) {
        ++pos_;
        Value v = Value::array();
        skipSpace();
        if (consume("]")) return v;
        for (;;) {
            skipSpace();
            v.push(value(depth + 1));
            skipSpace();
            if (consume("]")) return v;
            if (!consume(",")) error("expected ',' or ']'");
        }
    }

    Value numberValue() {
        const std::size_t start = pos_;
        const auto digit = [&] { return pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9'; };
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        if (!digit()) error("invalid value");
        if (text_[pos_] == '0') {
            ++pos_;
            if (digit()) error("leading zero");
        } else {
            while (digit()) ++pos_;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (!digit()) error("digit expected after '.'");
            while (digit()) ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (!digit()) error("digit expected in exponent");
            while (digit()) ++pos_;
        }
        double n = 0.0;
        const auto [end, ec] = std::from_chars(text_.data() + start, text_.data() + pos_, n);
        if (ec != std::errc() || end != text_.data() + pos_ || !std::isfinite(n)) error("number out of range");
        return Value::number(n);
    }

    unsigned hex4() {
        if (pos_ + 4 > text_.size()) error("truncated \\u escape");
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                error("invalid \\u escape");
            }
        }
        return v;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // One UTF-8 sequence copied verbatim after validation.
    void copyUtf8(std::string& out) {
        const auto lead = static_cast<unsigned char>(text_[pos_]);
        std::size_t n = 0;
        unsigned cp = 0;
        if ((lead & 0xE0) == 0xC0) {
            n = 1;
            cp = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            n = 2;
            cp = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            n = 3;
            cp = lead & 0x07u;
        } else {
            error("invalid UTF-8");
        }
        if (pos_ + n >= text_.size()) error("truncated UTF-8");
        for (std::size_t k = 1; k <= n; ++k) {
            const auto c = static_cast<unsigned char>(text_[pos_ + k]);
            if ((c & 0xC0) != 0x80) error("invalid UTF-8");
            cp = (cp << 6) | (c & 0x3Fu);
        }
        const unsigned minimum = n == 1 ? 0x80u : n == 2 ? 0x800u : 0x10000u;
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) error("invalid UTF-8");
        out.append(text_.substr(pos_, n + 1));
        pos_ += n + 1;
    }

    std::string string() {
        ++pos_;  // opening quote
        std::string out;
        for (;;) {
            if (pos_ >= text_.size()) error("unterminated string");
            const auto c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c < 0x20) error("control character in string");
            if (c >= 0x80) {
                copyUtf8(out);
                continue;
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                ++pos_;
                continue;
            }
            ++pos_;
            if (pos_ >= text_.size()) error("unterminated escape");
            const char e = text_[pos_++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned cp = hex4();
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (!consume("\\u")) error("lone high surrogate");
                    const unsigned low = hex4();
                    if (low < 0xDC00 || low > 0xDFFF) error("invalid low surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    error("lone low surrogate");
                }
                appendUtf8(out, cp);
                break;
            }
            default: error("invalid escape");
            }
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

// ── Serializer ───────────────────────────────────────────────────────────────

void writeString(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                out += std::format("\\u{:04x}", c);
            } else {
                out.push_back(ch);  // UTF-8 passes through
            }
        }
    }
    out.push_back('"');
}

void writeValue(std::string& out, const Value& v, int indent) {
    const auto newline = [&](int level) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(level) * 2, ' ');
    };
    switch (v.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
    case Value::Type::Number: {
        const double n = v.asNumber();
        if (!std::isfinite(n)) throw std::runtime_error("json: non-finite number");
        if (n == std::floor(n) && std::abs(n) < 9007199254740992.0) {
            out += std::format("{}", static_cast<std::int64_t>(n));
        } else {
            out += std::format("{}", n);  // shortest round-trip form
        }
        break;
    }
    case Value::Type::String: writeString(out, v.asString()); break;
    case Value::Type::Array: {
        const auto& items = v.items();
        if (items.empty()) {
            out += "[]";
            break;
        }
        // Arrays of scalars stay on one line; arrays of containers get one element per line.
        const bool flat = std::none_of(items.begin(), items.end(),
                                       [](const Value& x) { return x.isArray() || x.isObject(); });
        out.push_back('[');
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i) out += flat ? ", " : ",";
            if (!flat) newline(indent + 1);
            writeValue(out, items[i], indent + 1);
        }
        if (!flat) newline(indent);
        out.push_back(']');
        break;
    }
    case Value::Type::Object: {
        const auto& keys = v.keys();
        if (keys.empty()) {
            out += "{}";
            break;
        }
        out.push_back('{');
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (i) out.push_back(',');
            newline(indent + 1);
            writeString(out, keys[i]);
            out += ": ";
            writeValue(out, *v.find(keys[i]), indent + 1);
        }
        newline(indent);
        out.push_back('}');
        break;
    }
    }
}

}  // namespace

Value parse(std::string_view text) { return Parser(text).document(); }

std::string serialize(const Value& value) {
    std::string out;
    writeValue(out, value, 0);
    out.push_back('\n');
    return out;
}

}  // namespace chroma::json
