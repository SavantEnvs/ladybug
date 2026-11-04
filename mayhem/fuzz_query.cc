// Fuzzes the whole engine (parser, binder, planner, optimizer, processor, storage) by running the
// fuzzer-controlled text as Cypher against a small pre-populated in-memory graph.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include "common/exception/exception.h"
#include "db_common.h"
#include "parser/explain_statement.h"
#include "parser/parser.h"

static const char* kSetup[] = {
    "CREATE NODE TABLE Person(id INT64, name STRING, age INT64, score DOUBLE, tags STRING[], "
    "PRIMARY KEY(id))",
    "CREATE NODE TABLE City(name STRING, population INT64, PRIMARY KEY(name))",
    "CREATE REL TABLE Knows(FROM Person TO Person, since INT64)",
    "CREATE REL TABLE LivesIn(FROM Person TO City)",
    "CREATE (:Person {id: 1, name: 'Alice', age: 30, score: 1.5, tags: ['a', 'b']})",
    "CREATE (:Person {id: 2, name: 'Bob', age: 25, score: 2.5, tags: ['b']})",
    "CREATE (:Person {id: 3, name: 'Carol', age: 41, score: 3.5, tags: []})",
    "CREATE (:City {name: 'Waterloo', population: 100000})",
    "MATCH (a:Person {id: 1}), (b:Person {id: 2}) CREATE (a)-[:Knows {since: 2020}]->(b)",
    "MATCH (a:Person {id: 2}), (b:Person {id: 3}) CREATE (a)-[:Knows {since: 2021}]->(b)",
    "MATCH (a:Person {id: 1}), (c:City) CREATE (a)-[:LivesIn]->(c)",
};

static std::string gScratch;

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    const char* base = getenv("TMPDIR");
    std::string tmpl = std::string(base && *base ? base : "/tmp") + "/lbug_query_XXXXXX";
    if (!mkdtemp(tmpl.data())) {
        abort();
    }
    gScratch = tmpl;
    setenv("HOME", gScratch.c_str(), 1);
    return 0;
}

// Each input runs with cwd = the scratch dir (restored afterwards, so relative corpus/repro paths
// given on the command line keep working) and the dir is emptied when the input is done.
class ScratchCwd {
public:
    ScratchCwd() : saved_(open(".", O_RDONLY | O_DIRECTORY)) {
        if (saved_ < 0 || chdir(gScratch.c_str()) != 0) {
            abort();
        }
    }
    ~ScratchCwd() {
        if (fchdir(saved_) != 0) {
            abort();
        }
        close(saved_);
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(gScratch, ec)) {
            std::filesystem::remove_all(entry.path(), ec);
        }
    }
    ScratchCwd(const ScratchCwd&) = delete;
    ScratchCwd& operator=(const ScratchCwd&) = delete;

private:
    int saved_;
};

// Allow-list by parsed statement type: COPY ... TO, EXPORT DATABASE, ATTACH and INSTALL/UNINSTALL
// write outside the scratch dir (cwd, absolute paths, the shared extension dir), so they are not run.
static bool isAllowed(const lbug::parser::Statement& stmt) {
    using lbug::common::StatementType;
    switch (stmt.getStatementType()) {
    case StatementType::QUERY:
    case StatementType::CREATE_TABLE:
    case StatementType::DROP:
    case StatementType::ALTER:
    case StatementType::ANALYZE:
    case StatementType::COPY_FROM:
    case StatementType::STANDALONE_CALL:
    case StatementType::STANDALONE_CALL_FUNCTION:
    case StatementType::CREATE_MACRO:
    case StatementType::TRANSACTION:
    case StatementType::IMPORT_DATABASE:
    case StatementType::DETACH_DATABASE:
    case StatementType::USE_DATABASE:
    case StatementType::CREATE_SEQUENCE:
    case StatementType::CREATE_INDEX:
    case StatementType::CREATE_TYPE:
    case StatementType::CREATE_GRAPH:
    case StatementType::USE_GRAPH:
        return true;
    case StatementType::EXPLAIN:
        return isAllowed(*stmt.constCast<lbug::parser::ExplainStatement>().getStatementToExplain());
    default:
        return false;
    }
}

static bool allStatementsAllowed(const std::string& query) {
    try {
        for (const auto& stmt : lbug::parser::Parser::parseQuery(query)) {
            if (!isAllowed(*stmt)) {
                return false;
            }
        }
    } catch (const std::exception&) {
        // Unparseable: it fails in the parser when run below and executes nothing.
    }
    return true;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string query(reinterpret_cast<const char*>(data), size);
    if (!allStatementsAllowed(query)) {
        return -1;
    }
    ScratchCwd scratch;
    try {
        fuzz::Session session;
        for (auto* stmt : kSetup) {
            session.run(stmt);
        }
        session.run(query);
    } catch (const lbug::common::Exception&) {
        return 0;
    }
    return 0;
}
