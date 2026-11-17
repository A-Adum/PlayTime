#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pt {

// Minimal JSON DOM (UTF-8). Objects keep insertion order for stable exports.
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int v) : type_(Type::Number), num_((double)v), int_(v), isInt_(true) {}
    Json(int64_t v) : type_(Type::Number), num_((double)v), int_(v), isInt_(true) {}
    Json(double v) : type_(Type::Number), num_(v), int_((int64_t)v) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Json(const char* s) : type_(Type::String), str_(s) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }
    bool isNumber() const { return type_ == Type::Number; }

    bool asBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    int64_t asInt(int64_t def = 0) const { return type_ == Type::Number ? (isInt_ ? int_ : (int64_t)num_) : def; }
    double asDouble(double def = 0) const { return type_ == Type::Number ? num_ : def; }
    const std::string& asString() const;

    // Arrays
    void push(Json v) { items_.push_back(std::move(v)); }
    size_t size() const { return type_ == Type::Array ? items_.size() : members_.size(); }
    const Json& operator[](size_t i) const;
    const std::vector<Json>& items() const { return items_; }

    // Objects
    Json& set(std::string key, Json v);
    const Json& operator[](std::string_view key) const;  // returns null value if missing
    bool has(std::string_view key) const;
    const std::vector<std::pair<std::string, Json>>& members() const { return members_; }

    std::string dump(bool pretty = false) const;
    // Throws std::runtime_error on malformed input.
    static Json parse(std::string_view text);

private:
    void dumpTo(std::string& out, bool pretty, int indent) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    int64_t int_ = 0;
    bool isInt_ = false;
    std::string str_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

}  // namespace pt
