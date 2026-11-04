// Fuzzes opening an on-disk database: the main-file header / catalog / checkpoint readers behind
// Database::Database(path) (src/main/database.cpp, src/storage/checkpointer.cpp) and the WAL
// replay (WALReplayer::replay*, src/storage/wal/wal_replayer.cpp).
//
// Input layout:
//   byte 0      opts: bit 0 = WAL checksums OFF (the setting must match the one the WAL was
//               written with, so both kinds are reachable); bit 1 = throwOnWalReplayFailure.
//   bytes 1-4   little-endian length L of the main database file (clamped to what is left).
//   next L      the main database file `<scratch>/db` (not written when L == 0).
//   the rest    the WAL `<scratch>/db.wal` (not written when empty).
// A WAL carries the database ID of its main-file header, so the two travel together in one input;
// the seeds are real files written by the engine. Each input gets a fresh scratch dir under
// $TMPDIR, removed afterwards, so nothing carries over between inputs.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "common/exception/exception.h"
#include "internal_check.h"
#include "main/connection.h"
#include "main/database.h"

namespace {

namespace fs = std::filesystem;

std::string makeScratchDir() {
    const char* dir = getenv("TMPDIR");
    std::string tmpl = std::string(dir && *dir ? dir : "/tmp") + "/lbug_wal_XXXXXX";
    if (mkdtemp(tmpl.data()) == nullptr) {
        abort();
    }
    return tmpl;
}

void writeFile(const std::string& path, const uint8_t* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.close();
    if (!out) {
        abort();
    }
}

void drain(lbug::main::Connection& conn, const char* query) {
    auto result = conn.query(query);
    for (auto* r = result.get(); r != nullptr;
         r = r->hasNextQueryResult() ? r->getNextQueryResult() : nullptr) {
        if (!r->isSuccess()) {
            fuzz::abortOnInternal(r->getErrorMessage());
        } else {
            while (r->hasNext()) {
                r->getNext();
            }
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 5) {
        return -1;
    }
    const uint8_t opts = data[0];
    const bool checksums = !(opts & 1);
    const bool throwOnReplayFailure = opts & 2;
    size_t mainLen = static_cast<size_t>(data[1]) | (static_cast<size_t>(data[2]) << 8) |
                     (static_cast<size_t>(data[3]) << 16) | (static_cast<size_t>(data[4]) << 24);
    const uint8_t* rest = data + 5;
    const size_t restLen = size - 5;
    if (mainLen > restLen) {
        mainLen = restLen;
    }
    const std::string dir = makeScratchDir();
    const std::string db = dir + "/db";
    if (mainLen > 0) {
        writeFile(db, rest, mainLen);
    }
    if (restLen > mainLen) {
        writeFile(db + ".wal", rest + mainLen, restLen - mainLen);
    }
    try {
        lbug::main::SystemConfig config(/*bufferPoolSize=*/64ull << 20, /*maxNumThreads=*/1,
            /*enableCompression=*/true, /*readOnly=*/false, /*maxDBSize=*/1ull << 30,
            /*autoCheckpoint=*/false, /*checkpointThreshold=*/16777216,
            /*forceCheckpointOnClose=*/false, throwOnReplayFailure, checksums);
        lbug::main::Database database(db, config);
        lbug::main::Connection conn(&database);
        drain(conn, "MATCH (n) RETURN n");
        drain(conn, "MATCH (a)-[r]->(b) RETURN a, r, b");
    } catch (const lbug::common::Exception& e) {
        fuzz::abortOnInternal(e.what());
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    return 0;
}
