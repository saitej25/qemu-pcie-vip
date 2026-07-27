// Minimal, dependency-free C++ test harness. GoogleTest is not
// available on this system (no local install, and FetchContent would
// require network access this environment does not guarantee), so
// tests are plain functions registered into a static list and run by a
// tiny main() that reports PASS/FAIL per case and a summary.
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace mini_ics::test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

struct AssertionFailure {
    std::string message;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        Registry().push_back({name, std::move(fn)});
    }
};

#define MINI_ICS_TEST(name)                                                    \
    static void MiniIcsTest_##name();                                          \
    static ::mini_ics::test::Registrar MiniIcsTestRegistrar_##name(            \
        #name, MiniIcsTest_##name);                                            \
    static void MiniIcsTest_##name()

#define MINI_ICS_CHECK(cond)                                                   \
    do {                                                                       \
        if (!(cond)) {                                                        \
            std::ostringstream oss;                                           \
            oss << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond;   \
            throw ::mini_ics::test::AssertionFailure{oss.str()};              \
        }                                                                      \
    } while (0)

#define MINI_ICS_CHECK_EQ(a, b)                                                \
    do {                                                                       \
        if (!((a) == (b))) {                                                  \
            std::ostringstream oss;                                           \
            oss << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " #a     \
                << " != " #b;                                                 \
            throw ::mini_ics::test::AssertionFailure{oss.str()};              \
        }                                                                      \
    } while (0)

inline int RunAll(const std::string& suite_name) {
    int passed = 0, failed = 0;
    for (auto& tc : Registry()) {
        std::printf("[ RUN      ] %s.%s\n", suite_name.c_str(), tc.name.c_str());
        try {
            tc.fn();
            std::printf("[       OK ] %s.%s\n", suite_name.c_str(), tc.name.c_str());
            ++passed;
        } catch (const AssertionFailure& e) {
            std::printf("[  FAILED  ] %s.%s: %s\n", suite_name.c_str(), tc.name.c_str(),
                        e.message.c_str());
            ++failed;
        } catch (const std::exception& e) {
            std::printf("[  FAILED  ] %s.%s: unexpected exception: %s\n", suite_name.c_str(),
                        tc.name.c_str(), e.what());
            ++failed;
        }
    }
    std::printf("-----\n%d passed, %d failed, %d total\n", passed, failed, passed + failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace mini_ics::test
