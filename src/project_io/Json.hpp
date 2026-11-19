#pragma once
// ChromaStudio Engine · project_io · strict JSON (RFC 8259) for the project manifest.
// Parsing rejects duplicate keys, invalid UTF-8, control characters, leading zeros and nesting
// deeper than 128. Numbers are doubles (exact for the integers a manifest holds).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chroma::json {

class Value {
public:
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Value() = default;
    static Value null() { return Value(); }
    static Value boolean(bool b);
    static Value number(double n);
    static Value string(std::string s);
    static Value array();
    static Value object();

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool isNull() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool isBool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool isNumber() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool isString() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool isArray() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool isObject() const noexcept { return type_ == Type::Object; }

    [[nodiscard]] bool asBool() const;
    [[nodiscard]] double asNumber() const;
    [[nodiscard]] const std::string& asString() const;
    /// The number if it is an integer within [lo, hi], otherwise nullopt.
    [[nodiscard]] std::optional<std::int64_t> asInteger(std::int64_t lo, std::int64_t hi) const;

    // Arrays
    [[nodiscard]] const std::vector<Value>& items() const;
    Value& push(Value v);

    // Objects (insertion order is kept)
    [[nodiscard]] const std::vector<std::string>& keys() const;
    [[nodiscard]] const Value* find(std::string_view key) const;
    Value& set(std::string key, Value v);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Value> items_;       // array elements, or object values
    std::vector<std::string> keys_;  // object keys, parallel to items_
};

/// Throws std::runtime_error with the byte offset of the problem.
Value parse(std::string_view text);

/// Pretty-printed with two-space indentation; throws for non-finite numbers.
std::string serialize(const Value& value);

}  // namespace chroma::json
