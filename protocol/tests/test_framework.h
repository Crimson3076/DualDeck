#pragma once

// Minimal dependency-free test harness. The protocol library intentionally
// avoids pulling in a third-party test framework so `protocol/` can be
// built and tested in isolation with nothing but CMake + a C++20 compiler.

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace melonds_remote::test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int g_failures = 0;

inline void reportFailure(const char* file, int line, const std::string& expr) {
    std::fprintf(stderr, "  FAILED: %s:%d: %s\n", file, line, expr.c_str());
    ++g_failures;
}

inline int runAll() {
    int total = 0;
    for (auto& tc : registry()) {
        std::printf("[ RUN  ] %s\n", tc.name.c_str());
        int before = g_failures;
        tc.fn();
        ++total;
        if (g_failures == before) {
            std::printf("[  OK  ] %s\n", tc.name.c_str());
        } else {
            std::printf("[ FAIL ] %s\n", tc.name.c_str());
        }
    }
    std::printf("\n%d test case(s) run, %d assertion failure(s)\n", total, g_failures);
    return g_failures == 0 ? 0 : 1;
}

} // namespace melonds_remote::test

#define MDR_TEST(name)                                                          \
    static void mdr_test_##name();                                              \
    static ::melonds_remote::test::Registrar mdr_registrar_##name(              \
        #name, mdr_test_##name);                                                \
    static void mdr_test_##name()

#define MDR_CHECK(expr)                                                          \
    do {                                                                        \
        if (!(expr)) {                                                          \
            ::melonds_remote::test::reportFailure(__FILE__, __LINE__, #expr);    \
        }                                                                        \
    } while (0)

#define MDR_CHECK_EQ(a, b) MDR_CHECK((a) == (b))
