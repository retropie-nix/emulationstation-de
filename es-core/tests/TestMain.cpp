//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  TestMain.cpp
//
//  Runs the test cases registered with TEST_CASE and returns a non-zero exit code if any
//  expectation failed, which makes ctest fail the build.
//

#include "TestHarness.h"

#include <cstdio>

namespace TestHarness
{
    namespace
    {
        std::size_t sFailureCount {0};
    }

    std::vector<TestCase>& testCases()
    {
        static std::vector<TestCase> cases;
        return cases;
    }

    void recordFailure(const char* file, int line, const std::string& expression)
    {
        ++sFailureCount;
        std::printf("    FAILED: %s:%i: %s\n", file, line, expression.c_str());
    }

    std::size_t failureCount()
    {
        return sFailureCount;
    }

} // namespace TestHarness

int main()
{
    std::size_t failedCases {0};

    for (const TestHarness::TestCase& testCase : TestHarness::testCases()) {
        const std::size_t failuresBefore {TestHarness::failureCount()};
        testCase.function();
        if (TestHarness::failureCount() == failuresBefore) {
            std::printf("-- PASS: %s\n", testCase.name.c_str());
        }
        else {
            std::printf("-- FAIL: %s\n", testCase.name.c_str());
            ++failedCases;
        }
    }

    std::printf("-- %zu test case(s), %zu failed\n", TestHarness::testCases().size(), failedCases);

    return failedCases == 0 ? 0 : 1;
}
