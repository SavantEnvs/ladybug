// Known-answer functional checks for the embedded engine, built with the project's normal flags.
// Each check prints "PASS <name>" or "FAIL <name> ..."; mayhem/test.sh counts them.
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "main/connection.h"
#include "main/database.h"
#include "main/query_result.h"
#include "processor/result/flat_tuple.h"
#include "common/types/value/value.h"

using namespace lbug::main;

static int gPassed = 0, gFailed = 0;

static std::string cell(Connection& conn, const std::string& query) {
    auto r = conn.query(query);
    if (!r->isSuccess()) {
        return "ERROR: " + r->getErrorMessage();
    }
    if (!r->hasNext()) {
        return "<no rows>";
    }
    return r->getNext()->getValue(0)->toString();
}

static std::string column(Connection& conn, const std::string& query) {
    auto r = conn.query(query);
    if (!r->isSuccess()) {
        return "ERROR: " + r->getErrorMessage();
    }
    std::string out;
    while (r->hasNext()) {
        if (!out.empty()) {
            out += ",";
        }
        out += r->getNext()->getValue(0)->toString();
    }
    return out;
}

static void expect(const std::string& name, const std::string& got, const std::string& want) {
    if (got == want) {
        gPassed++;
        printf("PASS %s\n", name.c_str());
    } else {
        gFailed++;
        printf("FAIL %s: got '%s' want '%s'\n", name.c_str(), got.c_str(), want.c_str());
    }
}

static void expectError(const std::string& name, const std::string& got, const std::string& needle) {
    bool ok = got.rfind("ERROR: ", 0) == 0 && got.find(needle) != std::string::npos;
    expect(name, ok ? "ok" : got, "ok");
}

static void seed(Connection& c) {
    const char* stmts[] = {
        "CREATE NODE TABLE Person(id INT64, name STRING, age INT64, score DOUBLE, PRIMARY KEY(id))",
        "CREATE REL TABLE Knows(FROM Person TO Person, since INT64)",
        "CREATE (:Person {id: 1, name: 'Alice', age: 30, score: 1.5})",
        "CREATE (:Person {id: 2, name: 'Bob', age: 25, score: 2.5})",
        "CREATE (:Person {id: 3, name: 'Carol', age: 41, score: 3.5})",
        "MATCH (a:Person {id: 1}), (b:Person {id: 2}) CREATE (a)-[:Knows {since: 2020}]->(b)",
        "MATCH (a:Person {id: 2}), (b:Person {id: 3}) CREATE (a)-[:Knows {since: 2021}]->(b)",
    };
    for (auto* s : stmts) {
        auto r = c.query(s);
        if (!r->isSuccess()) {
            printf("FAIL setup '%s': %s\n", s, r->getErrorMessage().c_str());
            gFailed++;
        }
    }
}

int main() {
    namespace fs = std::filesystem;
    Database db(":memory:");
    Connection conn(&db);
    seed(conn);

    expect("count_nodes", cell(conn, "MATCH (p:Person) RETURN count(*)"), "3");
    expect("sum_age", cell(conn, "MATCH (p:Person) RETURN sum(p.age)"), "96");
    expect("avg_score", cell(conn, "MATCH (p:Person) RETURN avg(p.score)"), "2.500000");
    expect("one_hop", column(conn, "MATCH (a:Person {name: 'Alice'})-[:Knows]->(b) RETURN b.name"),
        "Bob");
    expect("two_hop",
        column(conn, "MATCH (a:Person {name: 'Alice'})-[:Knows*2..2]->(c) RETURN c.name"), "Carol");
    expect("order_by_desc", column(conn, "MATCH (p:Person) RETURN p.name ORDER BY p.age DESC"),
        "Carol,Alice,Bob");
    expect("rel_property", column(conn, "MATCH (:Person)-[k:Knows]->(:Person) RETURN k.since ORDER BY k.since"),
        "2020,2021");
    expect("arithmetic", cell(conn, "RETURN 2 + 3 * 4"), "14");
    expect("string_fn", cell(conn, "RETURN upper('abc') + lower('DEF')"), "ABCdef");
    expect("list_sort", cell(conn, "RETURN list_sort([3, 1, 2])"), "[1,2,3]");
    expect("where_filter",
        column(conn, "MATCH (p:Person) WHERE p.age > 28 RETURN p.name ORDER BY p.name"), "Alice,Carol");

    cell(conn, "MATCH (p:Person {id: 2}) SET p.age = 26");
    expect("set_property", cell(conn, "MATCH (p:Person {id: 2}) RETURN p.age"), "26");
    cell(conn, "MATCH (p:Person {id: 3}) DETACH DELETE p");
    expect("detach_delete", cell(conn, "MATCH (p:Person) RETURN count(*)"), "2");

    expectError("parser_error", cell(conn, "MATCH (a RETURN a"), "Parser exception");
    expectError("binder_error", cell(conn, "MATCH (n:NoSuchTable) RETURN n"), "Binder exception");
    expectError("pk_violation",
        cell(conn, "CREATE (:Person {id: 1, name: 'Dup', age: 1, score: 0.0})"), "primary key");

    fs::path dir = fs::temp_directory_path() / ("lbug_oracle_" + std::to_string(getpid()));
    fs::create_directories(dir);
    {
        std::ofstream(dir / "people.csv") << "id,name,age,score\n10,Dan,50,0.5\n11,Eve,60,1.5\n12,\"Fay, Jr\",70,2.5\n";
        Database fileDb((dir / "db").string());
        Connection c(&fileDb);
        c.query("CREATE NODE TABLE P(id INT64, name STRING, age INT64, score DOUBLE, PRIMARY KEY(id))");
        auto copy = c.query("COPY P FROM '" + (dir / "people.csv").string() + "' (header=true)");
        expect("copy_csv_ok", copy->isSuccess() ? "ok" : copy->getErrorMessage(), "ok");
        expect("copy_csv_count", cell(c, "MATCH (p:P) RETURN count(*)"), "3");
        expect("copy_csv_quoted", cell(c, "MATCH (p:P {id: 12}) RETURN p.name"), "Fay, Jr");
        expect("copy_csv_sum", cell(c, "MATCH (p:P) RETURN sum(p.age)"), "180");
        expect("load_from_csv",
            cell(c, "LOAD WITH HEADERS (id INT64, name STRING, age INT64, score DOUBLE) FROM '" +
                        (dir / "people.csv").string() + "' (header=true) WHERE age > 55 RETURN count(*)"),
            "2");
    }
    {
        Database reopened((dir / "db").string());
        Connection c(&reopened);
        expect("persist_reopen", cell(c, "MATCH (p:P) RETURN sum(p.age)"), "180");
    }
    fs::remove_all(dir);

    printf("RESULT passed=%d failed=%d\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
