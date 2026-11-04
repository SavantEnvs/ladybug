// Fuzzes the CSV reader through COPY FROM and LOAD FROM. The first input byte selects the reader
// options (header / delimiter / quote / escape / parallel); the rest is the file.
#include <cstddef>
#include <cstdint>
#include <string>

#include "common/exception/exception.h"
#include "db_common.h"
#include "file_target.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) {
        return -1;
    }
    uint8_t opts = data[0];
    static const char delims[] = {',', ';', '|', '\t'};
    static const char quotes[] = {'"', '\''};
    static const char escapes[] = {'\\', '"'};
    std::string options = std::string("header=") + ((opts & 1) ? "true" : "false") +
                          ", delim='" + (delims[(opts >> 1) & 3] == '\t' ? "\\t" : std::string(1, delims[(opts >> 1) & 3])) +
                          "', quote='" + quotes[(opts >> 3) & 1] + "', escape='" +
                          (escapes[(opts >> 4) & 1] == '\\' ? "\\\\" : "\"") +
                          "', parallel=" + ((opts & 0x20) ? "true" : "false");
    try {
        fuzz::TempFile file(data + 1, size - 1, ".csv");
        fuzz::Session session;
        session.run("CREATE NODE TABLE T(id INT64, name STRING, v DOUBLE, flag BOOL, d DATE, "
                    "tags INT64[], PRIMARY KEY(id))");
        session.run("COPY T FROM '" + file.path() + "' (" + options + ")");
        session.run("LOAD FROM '" + file.path() + "' (" + options + ") RETURN *");
    } catch (const lbug::common::Exception& e) {
        fuzz::abortOnInternal(e.what());
        return 0;
    }
    return 0;
}
