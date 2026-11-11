//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  StringUtilTests.cpp
//
//  Test cases for the low-level string functions. These also cover the test harness itself and
//  the static link against es-core, so that a broken test target fails the build.
//

#include "TestHarness.h"

#include "utils/StringUtil.h"

TEST_CASE(stringToUpper)
{
    EXPECT(Utils::String::toUpper("EmulationStation") == "EMULATIONSTATION");
    EXPECT(Utils::String::toUpper("") == "");
}

TEST_CASE(stringStartsWith)
{
    EXPECT(Utils::String::startsWith("EmulationStation", "Emulation"));
    EXPECT(!Utils::String::startsWith("EmulationStation", "Station"));
}

TEST_CASE(stringDelimitedStringToVector)
{
    const std::vector<std::string> parsed {
        Utils::String::delimitedStringToVector("snake,case,insensitive", ",", false, true)};
    EXPECT(parsed.size() == 3);
    EXPECT(parsed.at(0) == "snake");
    EXPECT(parsed.at(2) == "insensitive");
}
