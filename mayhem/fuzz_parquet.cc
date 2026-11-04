// Fuzzes the Parquet reader (thrift metadata, page decoding, compression codecs) through LOAD FROM
// and COPY FROM on a fuzzer-controlled .parquet file.
#include <cstddef>
#include <cstdint>
#include <string>

#include "common/exception/exception.h"
#include "db_common.h"
#include "file_target.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    try {
        fuzz::TempFile file(data, size, ".parquet");
        fuzz::Session session;
        session.run("LOAD FROM '" + file.path() + "' RETURN *");
        session.run("CREATE NODE TABLE T(id INT64, name STRING, PRIMARY KEY(id))");
        session.run("COPY T FROM '" + file.path() + "'");
    } catch (const lbug::common::Exception&) {
        return 0;
    }
    return 0;
}
