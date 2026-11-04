#pragma once
// Turns ladybug's internal-invariant failures into crashes the fuzzer can see.
//
// A failed DASSERT/ASSERT or an UNREACHABLE_CODE throws common::InternalException with the message
// `Assertion failed in file "<__FILE__>" on line <N>: <condition>` (assertFailureInternal,
// src/include/common/assert.h). Connection::query catches every std::exception (src/main/client_context.cpp)
// and returns its text as an error result, so without this check a broken invariant looks the same as a
// rejected query. The build keeps DASSERTs on (no -DNDEBUG in mayhem/build.sh).
//
// The match is anchored on purpose. Error messages echo the offending input after a fixed prefix
// ("Parser exception: Invalid input <...>", "Copy exception: ... Line/record containing the error: '...'"),
// so a substring search let the fuzzer plant the marker text in its input and crash the harness. Only
// InternalException leaves its message unprefixed, so a message that STARTS with the assertion format,
// naming a source file under the image's /mayhem/ tree, is an assertion that really failed.
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <string_view>

namespace fuzz {

inline bool isInternalAssertion(std::string_view msg) {
    static const std::regex kAssertion(R"(Assertion failed in file "/mayhem/[^"]+" on line [0-9]+: )");
    return std::regex_search(msg.begin(), msg.end(), kAssertion,
        std::regex_constants::match_continuous);
}

inline void abortOnInternal(std::string_view msg) {
    if (isInternalAssertion(msg)) {
        std::fprintf(stderr, "ladybug internal assertion: %.*s\n", static_cast<int>(msg.size()),
            msg.data());
        std::abort();
    }
}

} // namespace fuzz
