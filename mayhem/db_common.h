#pragma once
// Shared by the in-process harnesses: a fresh, tiny, single-threaded in-memory database per input.
#include <memory>
#include <string>

#include "internal_check.h"
#include "main/connection.h"
#include "main/database.h"

namespace fuzz {

struct Session {
    std::unique_ptr<lbug::main::Database> db;
    std::unique_ptr<lbug::main::Connection> conn;

    Session() {
        lbug::main::SystemConfig config(/*bufferPoolSize=*/64ull << 20, /*maxNumThreads=*/1,
            /*enableCompression=*/true, /*readOnly=*/false, /*maxDBSize=*/1ull << 30);
        db = std::make_unique<lbug::main::Database>(":memory:", config);
        conn = std::make_unique<lbug::main::Connection>(db.get());
    }

    // Query failures are expected for malformed input and reported through the result, not thrown.
    // A failed internal assertion is reported the same way, so it is told apart by its text and aborts.
    void run(const std::string& query) {
        auto result = conn->query(query);
        for (auto* r = result.get(); r != nullptr; r = r->hasNextQueryResult() ? r->getNextQueryResult() : nullptr) {
            if (!r->isSuccess()) {
                abortOnInternal(r->getErrorMessage());
            } else {
                while (r->hasNext()) {
                    r->getNext();
                }
            }
        }
    }
};

} // namespace fuzz
