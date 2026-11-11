//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  TestHarness.h
//
//  Minimal test case registration and assertion macros for the test executables.
//
//  A translation unit registers a test case with TEST_CASE(name) { ... } and reports failed
//  expectations with EXPECT(expression). TestMain.cpp runs every registered test case.
//

#ifndef ES_CORE_TESTS_TEST_HARNESS_H
#define ES_CORE_TESTS_TEST_HARNESS_H

#include <cstddef>
#include <string>
#include <vector>

namespace TestHarness
{
    struct TestCase {
        std::string name;
        void (*function)();
    };

    // The registered test cases, in the order of registration.
    std::vector<TestCase>& testCases();

    // Reports a failed expectation of the test case that is currently running.
    void recordFailure(const char* file, int line, const std::string& expression);

    // The number of failed expectations recorded so far.
    std::size_t failureCount();

    struct Registrar {
        Registrar(const char* name, void (*function)())
        {
            testCases().push_back({name, function});
        }
    };

} // namespace TestHarness

// Declares and defines a test case. Used at file scope, like a function definition.
#define TEST_CASE(name)                                                                              \
    static void name();                                                                              \
    static const ::TestHarness::Registrar name##TestRegistrar(#name, name);                           \
    static void name()

#define EXPECT(expression)                                                                           \
    do {                                                                                             \
        if (!(expression))                                                                           \
            ::TestHarness::recordFailure(__FILE__, __LINE__, #expression);                            \
    } while (false)

#endif // ES_CORE_TESTS_TEST_HARNESS_H
