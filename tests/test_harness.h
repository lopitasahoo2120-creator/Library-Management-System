// ---------------------------------------------------------------------------
// test_harness.h - tiny self-contained test framework.
//
// Deliberately dependency-free: the specification forbids external libraries,
// and a capstone project should not hide behind a testing framework.  Each
// test is a named function returning void; CHECK() records a failure instead of
// aborting, so one broken expectation does not hide the rest of the results.
// ---------------------------------------------------------------------------
#ifndef LIBRARY_TEST_HARNESS_H
#define LIBRARY_TEST_HARNESS_H

#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct Failure {
    std::string test;
    std::string detail;
};

class Registry {
public:
    // Tests are split into independent groups ("default" and "system") so each
    // suite runs exactly its own tests instead of the whole list twice.
    static Registry& instance(const std::string& group = "default") {
        static std::map<std::string, Registry> registry;
        return registry[group];
    }

    static std::vector<std::string> groups() {
        static std::map<std::string, Registry> registry;
        std::vector<std::string> names;
        for (const auto& entry : registry) names.push_back(entry.first);
        return names;
    }

    void add(const std::string& name, std::function<void()> fn) {
        tests_.push_back({name, std::move(fn)});
    }

    // Runs every registered test; returns the number of failures.
    int run(const std::string& sectionTitle) {
        std::cout << "\n================================================================\n"
                  << "  " << sectionTitle << '\n'
                  << "================================================================\n";

        // Optional filter: LIBRARY_TEST_FILTER=BookSerialize runs one test.
        const char* envFilter = std::getenv("LIBRARY_TEST_FILTER");
        const std::string filter = envFilter ? envFilter : "";

        Registry* const previous = active();
        active() = this;

        size_t passed = 0;
        size_t ran = 0;
        const size_t before = failures_.size();

        for (const auto& t : tests_) {
            if (!filter.empty() && t.name.find(filter) == std::string::npos) {
                continue;
            }
            ++ran;
            const size_t mark = failures_.size();
            std::cout << "  [ RUN  ] " << t.name << std::endl;
            try {
                t.fn();
            } catch (const std::exception& e) {
                failures_.push_back({t.name, std::string("threw: ") + e.what()});
            } catch (...) {
                failures_.push_back({t.name, "threw an unknown exception"});
            }

            if (failures_.size() == mark) {
                ++passed;
                std::cout << "  [  OK  ] " << t.name << "\n\n";
            } else {
                std::cout << "  [ FAIL ] " << t.name << "\n\n";
            }
        }

        const size_t failed = failures_.size() - before;
        active() = previous;
        std::cout << "  --- " << sectionTitle << ": " << passed << " passed, "
                  << failed << " failed (" << ran << " test(s) run) ---\n";
        return static_cast<int>(failed);
    }

    const std::vector<Failure>& failures() const { return failures_; }

    // The registry currently executing, so the CHECK macros record into the
    // right group without having to be aware of groups at all.
    static Registry*& active() {
        static Registry* current = nullptr;
        return current;
    }

    // Called by the CHECK macros to record a failure.
    void record(const std::string& test, const std::string& detail) {
        failures_.push_back({test, detail});
    }

private:
    struct TestCase {
        std::string            name;
        std::function<void()>  fn;
    };

    std::vector<TestCase>    tests_;
    std::vector<Failure>     failures_;
};

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn,
              const std::string& group = "default") {
        Registry::instance(group).add(name, std::move(fn));
    }
};

// Formats a value for a failure message without requiring operator<< on it.
template <typename T>
std::string show(const T& value) {
    std::ostringstream os;
    os << value;
    return os.str();
}

inline std::string show(bool value) { return value ? "true" : "false"; }
inline std::string show(const std::string& value) { return "\"" + value + "\""; }

} // namespace testing

// Registers a unit/functional test at static-initialisation time.
#define TEST(name)                                                             \
    static void name();                                                        \
    static testing::Registrar registrar_##name(#name, name);                   \
    static void name()

// Registers a Linux system-programming test in its own group.
#define SYSTEM_TEST(name)                                                      \
    static void name();                                                        \
    static testing::Registrar registrar_##name(#name, name, "system");         \
    static void name()

// Records a failure when the condition is false.
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            ::testing::Registry::active()->record(                            \
                __func__, std::string("CHECK failed: ") + #condition +         \
                            "  (at " + __FILE__ + ":" +                        \
                            std::to_string(__LINE__) + ")");                   \
        }                                                                      \
    } while (false)

// Failure with an explicit message and the real values.
#define CHECK_EQ(actual, expected)                                             \
    do {                                                                       \
        const auto a_ = (actual);                                              \
        const auto e_ = (expected);                                            \
        if (!(a_ == e_)) {                                                     \
            ::testing::Registry::active()->record(                            \
                __func__, std::string("expected ") + #actual + " == " +       \
                            #expected + "\n           actual   = " +           \
                            ::testing::show(a_) + "\n           expected = " + \
                            ::testing::show(e_) + "   (at " + __FILE__ + ":" + \
                            std::to_string(__LINE__) + ")");                   \
        }                                                                      \
    } while (false)

// Asserts that `stmt` throws std::invalid_argument.
#define CHECK_THROWS(stmt)                                                     \
    do {                                                                       \
        bool threw_ = false;                                                   \
        try {                                                                  \
            stmt;                                                              \
        } catch (const std::invalid_argument&) {                               \
            threw_ = true;                                                     \
        } catch (...) {                                                        \
        }                                                                      \
        if (!threw_) {                                                         \
            ::testing::Registry::active()->record(                            \
                __func__, std::string("expected std::invalid_argument from ") + \
                            #stmt + "   (at " + __FILE__ + ":" +               \
                            std::to_string(__LINE__) + ")");                   \
        }                                                                      \
    } while (false)

#endif // LIBRARY_TEST_HARNESS_H