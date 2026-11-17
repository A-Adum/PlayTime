#pragma once
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();

struct AutoReg {
    AutoReg(const std::string& name, std::function<void()> fn);
};

void recordFailure(const char* file, int line, const std::string& expr, const std::string& details = "");
int currentTestFailures();
void resetTestFailures();

template <typename T>
std::string toString(const T& val) {
    if constexpr (std::is_same_v<std::decay_t<T>, std::wstring>) {
        std::string s;
        for (wchar_t c : val) s.push_back(c < 128 ? static_cast<char>(c) : '?');
        return "\"" + s + "\"";
    } else if constexpr (std::is_same_v<std::decay_t<T>, std::wstring_view>) {
        std::string s;
        for (wchar_t c : val) s.push_back(c < 128 ? static_cast<char>(c) : '?');
        return "\"" + s + "\"";
    } else if constexpr (std::is_same_v<std::decay_t<T>, const wchar_t*>) {
        if (!val) return "null";
        std::string s;
        for (const wchar_t* p = val; *p; ++p) s.push_back(*p < 128 ? static_cast<char>(*p) : '?');
        return "\"" + s + "\"";
    } else if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
        return "\"" + val + "\"";
    } else if constexpr (std::is_same_v<std::decay_t<T>, std::string_view>) {
        return "\"" + std::string(val) + "\"";
    } else if constexpr (std::is_same_v<std::decay_t<T>, const char*>) {
        return val ? "\"" + std::string(val) + "\"" : "null";
    } else if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
        return val ? "true" : "false";
    } else {
        std::ostringstream oss;
        oss << val;
        return oss.str();
    }
}

}  // namespace test

#define TEST(name) \
    static void test_fn_##name(); \
    static ::test::AutoReg reg_##name(#name, test_fn_##name); \
    static void test_fn_##name()

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            ::test::recordFailure(__FILE__, __LINE__, #expr); \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        auto _va = (a); \
        auto _vb = (b); \
        if (!(_va == _vb)) { \
            std::string _detail = ::test::toString(_va) + " != " + ::test::toString(_vb); \
            ::test::recordFailure(__FILE__, __LINE__, #a " == " #b, _detail); \
        } \
    } while (0)

#define CHECK_NE(a, b) \
    do { \
        auto _va = (a); \
        auto _vb = (b); \
        if (!(_va != _vb)) { \
            std::string _detail = ::test::toString(_va) + " == " + ::test::toString(_vb); \
            ::test::recordFailure(__FILE__, __LINE__, #a " != " #b, _detail); \
        } \
    } while (0)

#define CHECK_THROWS(expr) \
    do { \
        bool _threw = false; \
        try { \
            (void)(expr); \
        } catch (...) { \
            _threw = true; \
        } \
        if (!_threw) { \
            ::test::recordFailure(__FILE__, __LINE__, "Expected exception from: " #expr); \
        } \
    } while (0)
