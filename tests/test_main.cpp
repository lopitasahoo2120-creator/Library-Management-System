// ---------------------------------------------------------------------------
// test_main.cpp - test runner entry point.
//
// The tests must not touch the real data/ directory, so the whole suite runs
// with the working directory set to a scratch folder.  All paths in Config.h
// are relative, which makes that a complete isolation strategy.
//
// Environment
//   LIBRARY_TEST_FILTER=<substring>   run only tests whose name contains it
// ---------------------------------------------------------------------------

#include "test_harness.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path makeScratchDir() {
    const fs::path dir =
        fs::temp_directory_path() / "library_capstone_tests";

    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "data", ec);
    fs::create_directories(dir / "logs", ec);
    return dir;
}

struct SuiteResult {
    std::string title;
    int         failures = 0;
};

void runSuite(const std::string& group, const std::string& title) {
    testing::Registry& registry = testing::Registry::instance(group);

    const std::size_t before = registry.failures().size();
    registry.run(title);
    const std::size_t after = registry.failures().size();

    if (after > before) {
        std::cout << "\n  Failures in '" << title << "':\n";
        for (std::size_t i = before; i < after; ++i) {
            std::cout << "    - " << registry.failures()[i].test << "\n      "
                      << registry.failures()[i].detail << "\n";
        }
    }
}

std::size_t totalFailures() {
    std::size_t total = 0;
    for (const std::string& group : testing::Registry::groups()) {
        total += testing::Registry::instance(group).failures().size();
    }
    return total;
}

} // namespace

int main() {
    std::cout << "\n"
              << "============================================================\n"
              << "  LIBRARY MANAGEMENT SYSTEM - AUTOMATED TEST SUITE\n"
              << "============================================================\n";

    const fs::path scratch = makeScratchDir();
    if (!fs::exists(scratch)) {
        std::cerr << "FATAL: could not create the scratch directory\n";
        return 2;
    }

    std::cout << "  Scratch directory: " << scratch.string() << "\n";
    fs::current_path(scratch);
    // No admin.dat is seeded on purpose: the first Authentication instance
    // must provision the documented default account exactly as it would on a
    // fresh machine.

    runSuite("default", "UNIT AND FUNCTIONAL TESTS");
    runSuite("system", "LINUX SYSTEM PROGRAMMING TESTS");

    const std::size_t failures = totalFailures();
    std::cout << "\n================================================================\n"
              << "  TOTAL: " << failures << " failure(s)\n"
              << "================================================================\n";

    return failures == 0 ? 0 : 1;
}