#pragma once
// Minimal, self-contained test harness. Deliberately not GoogleTest/Catch2:
// this project's testing philosophy (Phase 0 §5) is to prefer a small,
// fully-auditable custom harness over a dependency, for both property
// testing and ordinary unit testing. ~60 lines, no build-system surface
// area, no network fetch required to build.

#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

struct AssertionFailure : std::exception {
    std::string msg;
    explicit AssertionFailure(std::string m) : msg(std::move(m)) {}
    const char* what() const noexcept override { return msg.c_str(); }
};

inline int runAll() {
    int failed = 0;
    for (auto& tc : registry()) {
        try {
            tc.fn();
            std::cout << "[PASS] " << tc.name << "\n";
        } catch (const AssertionFailure& e) {
            std::cout << "[FAIL] " << tc.name << ": " << e.what() << "\n";
            ++failed;
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << tc.name << ": uncaught exception: " << e.what() << "\n";
            ++failed;
        }
    }
    std::cout << (registry().size() - failed) << "/" << registry().size() << " passed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace testing

#define LOB_CONCAT_(a, b) a##b
#define LOB_CONCAT(a, b) LOB_CONCAT_(a, b)

#define TEST_CASE(name)                                                                          \
    static void LOB_CONCAT(test_fn_, __LINE__)();                                                \
    static ::testing::Registrar LOB_CONCAT(test_reg_, __LINE__)(name,                            \
                                                                LOB_CONCAT(test_fn_, __LINE__)); \
    static void LOB_CONCAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            throw ::testing::AssertionFailure(std::string("CHECK failed: ") + #cond + " at " + \
                                              __FILE__ + ":" + std::to_string(__LINE__));      \
        }                                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                                \
    do {                                                                              \
        auto va = (a);                                                                \
        auto vb = (b);                                                                \
        if (!(va == vb)) {                                                            \
            throw ::testing::AssertionFailure(std::string("CHECK_EQ failed: ") + #a + \
                                              " != " + #b + " at " + __FILE__ + ":" + \
                                              std::to_string(__LINE__));              \
        }                                                                             \
    } while (0)

#define REQUIRE(cond) CHECK(cond)
