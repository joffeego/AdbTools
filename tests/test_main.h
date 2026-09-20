// Minimal test harness.
//
// The project has no third-party dependency in its build, so rather than pull in
// a framework this is ~60 lines that register test functions and report
// failures with the expression, file and line. Enough for pure-logic tests and
// it keeps `cmake --build` dependency-free.
#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace adb::test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failureCount() {
    static int failures = 0;
    return failures;
}

inline const char*& currentCase() {
    static const char* name = "";
    return name;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

// Report a failure with the failing expression and its location.
inline void reportFailure(const char* expr, const char* file, int line, const std::string& extra) {
    ++failureCount();
    std::cerr << "  FAIL [" << currentCase() << "] " << file << ":" << line << "\n"
              << "       " << expr << "\n";
    if (!extra.empty()) std::cerr << "       -> " << extra << "\n";
    std::cout << "  FAIL [" << currentCase() << "] " << file << ":" << line << "\n"
              << "       " << expr << "\n";
    if (!extra.empty()) std::cout << "       -> " << extra << "\n";
}

// Index an element of a container that may be a temporary, without ever reading
// out of bounds.
//
// Indexing a function's return value directly - `f(...)[3]` - is what let an
// earlier version of these tests corrupt the heap instead of reporting a
// failure: a wrong expected size meant reading past the end of a vector that had
// already been destroyed, which surfaced as std::length_error in one runner and
// std::bad_alloc in another, at unrelated places in the suite.
//
// Using these accessors, a wrong size produces a normal "out of range" failure.
template <typename Container>
auto atOrEmpty(const Container& c, std::size_t index, const char* file, int line)
    -> const typename Container::value_type& {
    static const typename Container::value_type empty{};
    if (index >= c.size()) {
        reportFailure("index in range", file, line,
                      "index " + std::to_string(index) + " >= size " + std::to_string(c.size()));
        return empty;
    }
    return c[index];
}

// Same idea for std::string, which has operator[] but no value_type fallback.
inline char charAtOrZero(const std::string& s, std::size_t index, const char* file, int line) {
    if (index >= s.size()) {
        reportFailure("index in range", file, line,
                      "index " + std::to_string(index) + " >= size " + std::to_string(s.size()));
        return '\0';
    }
    return s[index];
}

template <typename T>
std::string describe(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

inline std::string describe(const std::string& value) { return "\"" + value + "\""; }

template <typename T>
std::string describe(const std::vector<T>& value) {
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i) out << ", ";
        out << value[i];
    }
    out << "]";
    return out.str();
}

inline std::string describe(bool value) { return value ? "true" : "false"; }

inline int runAll() {
    // Progress goes to stderr, unbuffered. std::cout is buffered and its
    // "last line before the crash" is exactly what gets lost when a case aborts
    // the process (which is how a std::bad_alloc on CI gave no indication of
    // which case it was). stderr is line-buffered/unbuffered enough to survive.
    std::cerr << "Running " << registry().size() << " test cases" << std::endl;
    std::cout << "Running " << registry().size() << " test cases" << std::endl;
    for (const Case& c : registry()) {
        currentCase() = c.name;
        std::cerr << "  ...  " << c.name << std::endl;
        const int before = failureCount();
        c.fn();
        const bool ok = (failureCount() == before);
        std::cerr << (ok ? "  ok   " : "  FAIL ") << c.name << std::endl;
        std::cout << (ok ? "  ok   " : "  FAIL ") << c.name << std::endl;
    }
    std::cout << std::endl;
    if (failureCount() == 0) {
        std::cout << "All tests passed (" << registry().size() << " cases)" << std::endl;
        return 0;
    }
    std::cout << failureCount() << " assertion(s) failed" << std::endl;
    return 1;
}

}  // namespace adb::test

#define ADB_TEST(name)                                                        \
    static void name();                                                       \
    static ::adb::test::Registrar adb_test_registrar_##name(#name, &name);     \
    static void name()

#define ADB_CHECK(expr)                                                       \
    do {                                                                      \
        if (!(expr)) ::adb::test::reportFailure(#expr, __FILE__, __LINE__, ""); \
    } while (0)

// Element access that never reads out of bounds. Prefer
//   ADB_CHECK_EQ(ADB_AT(rows, 0).first, std::string("x"))
// over
//   ADB_CHECK_EQ(rows[0].first, std::string("x"))
// so a wrong size is reported as a failure instead of corrupting the heap.
#define ADB_AT(container, index) \
    ::adb::test::atOrEmpty((container), static_cast<std::size_t>(index), __FILE__, __LINE__)

#define ADB_CHAR_AT(str, index) \
    ::adb::test::charAtOrZero((str), static_cast<std::size_t>(index), __FILE__, __LINE__)

#define ADB_CHECK_EQ(actual, expected)                                        \
    do {                                                                      \
        const auto& adb_actual_ = (actual);                                   \
        const auto& adb_expected_ = (expected);                               \
        if (!(adb_actual_ == adb_expected_)) {                                \
            ::adb::test::reportFailure(#actual " == " #expected, __FILE__, __LINE__, \
                                       ::adb::test::describe(adb_actual_) + " != " + \
                                           ::adb::test::describe(adb_expected_));    \
        }                                                                     \
    } while (0)
