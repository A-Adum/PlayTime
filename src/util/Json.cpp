#include "util/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace pt {

namespace {
const Json kNull;
const std::string kEmpty;

struct Parser {
    std::string_view s;
    size_t i = 0;
    int depth = 0;

    [[noreturn]] void fail(const char* what) {
        throw std::runtime_error(std::string("JSON parse error: ") + what + " at offset " + std::to_string(i));
    }
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool lit(std::string_view w) {
        if (s.substr(i, w.size()) == w) {
            i += w.size();
            return true;
        }
        return false;
    }
    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    uint32_t hex4() {
        if (i + 4 > s.size()) fail("bad unicode escape");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else fail("bad hex digit");
        }
        return v;
    }
    std::string str() {
        if (s[i] != '"') fail("expected string");
        ++i;
        std::string out;
        while (true) {
            if (i >= s.size()) fail("unterminated string");
            char c = s[i++];
            if (c == '"') break;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) fail("bad escape");
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        uint32_t lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("bad escape");
            }
        }
        return out;
    }
    Json value() {
        if (++depth > 256) fail("nesting too deep");
        ws();
        if (i >= s.size()) fail("unexpected end");
        Json result;
        char c = s[i];
        if (c == '{') {
            ++i;
            result = Json::object();
            ws();
            if (i < s.size() && s[i] == '}') ++i;
            else
                while (true) {
                    ws();
                    std::string k = str();
                    ws();
                    if (i >= s.size() || s[i] != ':') fail("expected ':'");
                    ++i;
                    result.set(std::move(k), value());
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; break; }
                    fail("expected ',' or '}'");
                }
        } else if (c == '[') {
            ++i;
            result = Json::array();
            ws();
            if (i < s.size() && s[i] == ']') ++i;
            else
                while (true) {
                    result.push(value());
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; break; }
                    fail("expected ',' or ']'");
                }
        } else if (c == '"') {
            result = Json(str());
        } else if (lit("true")) {
            result = Json(true);
        } else if (lit("false")) {
            result = Json(false);
        } else if (lit("null")) {
            result = Json();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = i;
            bool isFloat = false;
            if (s[i] == '-') ++i;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                    s[i] == '+' || s[i] == '-')) {
                if (s[i] == '.' || s[i] == 'e' || s[i] == 'E') isFloat = true;
                ++i;
            }
            std::string num(s.substr(start, i - start));
            if (isFloat) result = Json(std::strtod(num.c_str(), nullptr));
            else result = Json((int64_t)std::strtoll(num.c_str(), nullptr, 10));
        } else {
            fail("unexpected character");
        }
        --depth;
        return result;
    }
};

void escapeTo(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}
}  // namespace

const std::string& Json::asString() const { return type_ == Type::String ? str_ : kEmpty; }

const Json& Json::operator[](size_t i) const {
    return (type_ == Type::Array && i < items_.size()) ? items_[i] : kNull;
}

Json& Json::set(std::string key, Json v) {
    for (auto& m : members_)
        if (m.first == key) {
            m.second = std::move(v);
            return m.second;
        }
    members_.emplace_back(std::move(key), std::move(v));
    return members_.back().second;
}

const Json& Json::operator[](std::string_view key) const {
    if (type_ == Type::Object)
        for (auto& m : members_)
            if (m.first == key) return m.second;
    return kNull;
}

bool Json::has(std::string_view key) const {
    if (type_ != Type::Object) return false;
    for (auto& m : members_)
        if (m.first == key) return true;
    return false;
}

void Json::dumpTo(std::string& out, bool pretty, int indent) const {
    auto nl = [&](int ind) {
        if (pretty) {
            out += '\n';
            out.append((size_t)ind * 2, ' ');
        }
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number:
            if (isInt_) out += std::to_string(int_);
            else if (!std::isfinite(num_)) out += "null";
            else {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.17g", num_);
                out += buf;
            }
            break;
        case Type::String: escapeTo(out, str_); break;
        case Type::Array:
            out += '[';
            for (size_t k = 0; k < items_.size(); ++k) {
                if (k) out += ',';
                nl(indent + 1);
                items_[k].dumpTo(out, pretty, indent + 1);
            }
            if (!items_.empty()) nl(indent);
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (size_t k = 0; k < members_.size(); ++k) {
                if (k) out += ',';
                nl(indent + 1);
                escapeTo(out, members_[k].first);
                out += pretty ? ": " : ":";
                members_[k].second.dumpTo(out, pretty, indent + 1);
            }
            if (!members_.empty()) nl(indent);
            out += '}';
            break;
    }
}

std::string Json::dump(bool pretty) const {
    std::string out;
    dumpTo(out, pretty, 0);
    return out;
}

Json Json::parse(std::string_view text) {
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text.remove_prefix(3);
    Parser p{text};
    Json v = p.value();
    p.ws();
    if (p.i != text.size()) p.fail("trailing characters");
    return v;
}

}  // namespace pt
