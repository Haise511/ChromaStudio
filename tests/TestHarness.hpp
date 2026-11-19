#pragma once
// Minimal self-contained test harness (the build stays offline: no external test framework).

#include <cmath>
#include <cstdio>
#include <exception>
#include <format>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace test {

struct Case {
    const char* name;
    void (*run)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, void (*run)()) { registry().push_back({name, run}); }
};

inline void fail(const char* file, int line, const std::string& what) {
    std::printf("    FAIL %s:%d: %s\n", file, line, what.c_str());
    ++failures();
}

/// No modal windows on a shared desktop: CRT asserts, run-time checks and faults are printed and end
/// the run with a failure code instead of waiting for someone to press a button.
inline void quietFailures() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, [](int type, char* message, int*) -> int {
        if (type == _CRT_WARN) return FALSE;
        std::fprintf(stderr, "CRT: %s\n", message ? message : "?");
        std::fflush(stdout);
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 3);
        return TRUE;
    });
#endif
#endif
}

inline int runAll() {
    quietFailures();
    int failedCases = 0;
    for (const Case& c : registry()) {
        const int before = failures();
        std::printf("[ RUN  ] %s\n", c.name);
        try {
            c.run();
        } catch (const std::exception& error) {
            fail(__FILE__, __LINE__, std::format("unexpected exception: {}", error.what()));
        }
        const bool ok = failures() == before;
        if (!ok) ++failedCases;
        std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", c.name);
    }
    std::printf("%zu cases, %d failed, %d failed checks\n", registry().size(), failedCases, failures());
    return failures() == 0 ? 0 : 1;
}

}  // namespace test

#define TEST_CASE(name)                                               \
    static void name();                                               \
    static const ::test::Registrar name##Registrar(#name, &name);     \
    static void name()

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) ::test::fail(__FILE__, __LINE__, #condition);      \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                                                              \
    do {                                                                                                     \
        const double actualValue = (actual);                                                                 \
        const double expectedValue = (expected);                                                             \
        if (!(std::abs(actualValue - expectedValue) <= (tolerance))) {                                       \
            ::test::fail(__FILE__, __LINE__,                                                                 \
                         std::format("{} = {} (expected {} ± {})", #actual, actualValue, expectedValue,      \
                                     static_cast<double>(tolerance)));                                       \
        }                                                                                                    \
    } while (0)

#define CHECK_THROWS(expression)                                                       \
    do {                                                                               \
        bool thrown = false;                                                           \
        try {                                                                          \
            (void)(expression);                                                        \
        } catch (...) {                                                                \
            thrown = true;                                                             \
        }                                                                              \
        if (!thrown) ::test::fail(__FILE__, __LINE__, "expected an exception: " #expression); \
    } while (0)
