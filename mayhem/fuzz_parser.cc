// Fuzzes the Cypher front-end (ANTLR4 parser + transformer) on arbitrary query text.
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

#include "internal_check.h"
#include "parser/parser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view query(reinterpret_cast<const char*>(data), size);
    try {
        lbug::parser::Parser::parseQuery(query);
    } catch (const std::exception& e) {
        // Connection::query catches std::exception around the parser (e.g. antlr4's UTF-8 errors).
        fuzz::abortOnInternal(e.what());
        return 0;
    }
    return 0;
}
