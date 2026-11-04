// Fuzzes the NPY reader (NpyReader: parseHeader / parseType / validate / readBlock,
// src/processor/operator/persistent/reader/npy/npy_reader.cpp) through COPY ... BY COLUMN and
// LOAD FROM.
//
// Input layout: byte 0 picks the type of the column the file is copied into (each type
// NpyReader::parseType accepts, as a scalar and as a fixed-size array, so validate() and both
// readBlock() branches are reached); bytes 1-2 (little endian, mod 8192, +1) are the row count of
// the INT64 primary-key column the harness writes beside it (BY COLUMN copies one file per column,
// and validate() checks the files agree on rows; up to 8192 rows spans several 2048-row blocks);
// the rest is the .npy file.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/exception/exception.h"
#include "db_common.h"
#include "file_target.h"

namespace {

// A valid little-endian INT64 .npy holding 0..rows-1.
std::vector<uint8_t> idColumn(uint32_t rows) {
    std::string header = "{'descr': '<i8', 'fortran_order': False, 'shape': (" +
                         std::to_string(rows) + ",), }";
    while ((10 + header.size() + 1) % 16 != 0) {
        header += ' ';
    }
    header += '\n';
    std::vector<uint8_t> out = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0,
        static_cast<uint8_t>(header.size() & 0xff), static_cast<uint8_t>(header.size() >> 8)};
    out.insert(out.end(), header.begin(), header.end());
    for (int64_t i = 0; i < rows; i++) {
        uint8_t le[8];
        std::memcpy(le, &i, sizeof(le));
        out.insert(out.end(), le, le + sizeof(le));
    }
    return out;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 3) {
        return -1;
    }
    static const char* const kTypes[] = {"INT64", "INT32", "INT16", "DOUBLE", "FLOAT",
        "INT64[3]", "DOUBLE[3]", "FLOAT[2]", "INT32[4]", "INT16[1]"};
    const std::string type = kTypes[data[0] % (sizeof(kTypes) / sizeof(kTypes[0]))];
    const uint32_t rows = 1 + ((data[1] | (data[2] << 8)) % 8192);
    try {
        const std::vector<uint8_t> ids = idColumn(rows);
        fuzz::TempFile idFile(ids.data(), ids.size(), ".npy");
        fuzz::TempFile file(data + 3, size - 3, ".npy");
        fuzz::Session session;
        session.run("CREATE NODE TABLE T(id INT64, v " + type + ", PRIMARY KEY(id))");
        session.run("COPY T FROM ('" + idFile.path() + "', '" + file.path() + "') BY COLUMN");
        session.run("MATCH (t:T) RETURN t.id, t.v");
        session.run("LOAD FROM '" + file.path() + "' RETURN *");
    } catch (const lbug::common::Exception& e) {
        fuzz::abortOnInternal(e.what());
        return 0;
    }
    return 0;
}
