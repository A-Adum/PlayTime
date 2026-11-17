#include "test_harness.h"
#include <iostream>

namespace test {

static std::vector<TestCase>& getRegistryInternal() {
    static std::vector<TestCase> s_tests;
    return s_tests;
}

std::vector<TestCase>& registry() {
    return getRegistryInternal();
}

AutoReg::AutoReg(const std::string& name, std::function<void()> fn) {
    registry().push_back({name, std::move(fn)});
}

static int s_currentFailures = 0;

void recordFailure(const char* file, int line, const std::string& expr, const std::string& details) {
    ++s_currentFailures;
    std::cout << "  FAIL: " << file << ":" << line << ": " << expr;
    if (!details.empty()) {
        std::cout << " (" << details << ")";
    }
    std::cout << "\n";
}

int currentTestFailures() {
    return s_currentFailures;
}

void resetTestFailures() {
    s_currentFailures = 0;
}

}  // namespace test

int main() {
    auto& tests = test::registry();
    std::cout << "========================================" << std::endl;
    std::cout << " Running " << tests.size() << " PlayTime automated tests" << std::endl;
    std::cout << "========================================" << std::endl;

    int passed = 0;
    int failed = 0;

    for (const auto& t : tests) {
        test::resetTestFailures();
        std::cout << "[ RUN      ] " << t.name << std::endl;
        try {
            t.fn();
        } catch (const std::exception& e) {
            test::recordFailure(__FILE__, __LINE__, "Unhandled exception", e.what());
        } catch (...) {
            test::recordFailure(__FILE__, __LINE__, "Unhandled non-std exception");
        }

        if (test::currentTestFailures() == 0) {
            std::cout << "[       OK ] " << t.name << std::endl;
            ++passed;
        } else {
            std::cout << "[  FAILED  ] " << t.name << " (" << test::currentTestFailures() << " failure(s))" << std::endl;
            ++failed;
        }
    }

    std::cout << "========================================" << std::endl;
    std::cout << " Test Summary: " << passed << " passed, " << failed << " failed, " << tests.size() << " total." << std::endl;
    std::cout << "========================================" << std::endl;

    return failed == 0 ? 0 : 1;
}
