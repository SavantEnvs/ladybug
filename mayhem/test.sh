#!/usr/bin/env bash
#
# mayhem/test.sh — RUN this repo's OWN functional test suite (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

PASSED=0 FAILED=0 SKIPPED=0

# 1) A subset of upstream's own gtest suites, built by build.sh in the unsanitized build/test tree.
#    Fixtures come from dataset/ (build.sh copies the pinned submodule in); temporary databases go to
#    $TMPDIR (TestHelper uses std::filesystem::temp_directory_path()).
#
#    The subset covers the fuzzed surfaces (CSV and Parquet COPY / LOAD FROM, the Cypher parser and
#    binder, query execution) and runs in about 80 s on 8 cores, inside rlenv's 180 s grading timeout.
#    Left out, with the reason (measured at upstream 71f38adf2):
#    - e2e `transaction` (78.6 s), `copy` (41.3 s), `dml_node`, `dml_rel`, `function`, `issue`, `ddl`, and the
#      api_test / c_api_test binaries: they pass, but together they take about 4 more minutes.
#    - e2e `extension`: INSTALL downloads from extension.ladybugdb.com, which an air-gapped image cannot reach.
#    - e2e `dictionary_bug` (orb383_relationship_projection_obfuscated.AnonymousParquetDeleteReload): fails
#      upstream in this build; the MATCH returns no row where the test expects `0|REL_A`.
#    - storage, transaction and optimizer unit binaries: not built, to keep the image build time down.
GT="$SRC/build/test/test"
UNIT=(common/types_test binder/binder_test processor/processor_test planner/planner_tests copy/copy_tests)
E2E_DIRS=(csv load_from reader cast exceptions tck match filter projection arithmetic nested_types read_list
  common demo_db)

# run_gtest <label> <cmd...>: runs one gtest binary and folds its own summary lines into the totals. A
# binary that is missing, crashes before its summary, or runs zero tests counts as one failure.
run_gtest() {
  local label="$1"; shift
  if [ ! -x "$1" ]; then
    echo "MISSING $label: $1 not built by mayhem/build.sh" >&2; FAILED=$(( FAILED + 1 )); return
  fi
  local out p f s
  # From $TMPDIR, which the runner can write: some upstream tests create files relative to the cwd
  # (vfs_test.cpp makes a literal `~/tmp/...`), and rlenv runs test.sh where /mayhem is read-only.
  out=$(cd "${TMPDIR:-/tmp}" && timeout 600 "$@" 2>&1)
  p=$(printf '%s\n' "$out" | sed -n 's/^\[  PASSED  \] \([0-9]*\) tests\{0,1\}\..*/\1/p' | tail -1)
  f=$(printf '%s\n' "$out" | sed -n 's/^\[  FAILED  \] \([0-9]*\) tests\{0,1\}, listed below.*/\1/p' | tail -1)
  s=$(printf '%s\n' "$out" | sed -n 's/^\[  SKIPPED \] \([0-9]*\) tests\{0,1\}, listed below.*/\1/p' | tail -1)
  p=${p:-0} f=${f:-0} s=${s:-0}
  if [ -z "$(printf '%s\n' "$out" | grep -m1 '^\[==========\] [0-9]* tests\{0,1\} from .* ran\.')" ] || [ $(( p + f )) -eq 0 ]; then
    printf '%s\n' "$out" | tail -20 >&2
    echo "BROKEN $label: no gtest summary or zero tests ran" >&2
    f=$(( f + 1 ))
  fi
  printf '%-28s passed=%-5s failed=%-3s skipped=%s\n' "$label" "$p" "$f" "$s"
  [ "$f" -eq 0 ] || printf '%s\n' "$out" | grep -E '^\[  FAILED  \] [^0-9]' >&2
  PASSED=$(( PASSED + p )) FAILED=$(( FAILED + f )) SKIPPED=$(( SKIPPED + s ))
}

# vfs_test.cpp hardcodes /tmp/dbHome, /tmp/test2, /tmp/mydb and /tmp/mydb.g1 and leaves /tmp/dbHome and
# /tmp/mydb.g1 behind. /tmp is sticky, so residue from one uid makes the next uid's run fail
# (VirtualFileSystemDeleteFilesEdgeCases, VirtualFileSystemDeleteExtensionlessDbGraphFiles). Remove it
# before and after, so a run as the image user does not break a later run as the rlenv runner.
VFS_RESIDUE=(/tmp/dbHome /tmp/test2 /tmp/mydb /tmp/mydb.g1)
rm -rf "${VFS_RESIDUE[@]}" 2>/dev/null || true
for u in "${UNIT[@]}"; do run_gtest "${u##*/}" "$GT/$u"; done
rm -rf "${VFS_RESIDUE[@]}" 2>/dev/null || true
# e2e_test reads the directory under test/test_files from argv[1] (test/runner/e2e_test.cpp main()).
for d in "${E2E_DIRS[@]}"; do run_gtest "e2e:$d" "$GT/runner/e2e_test" "$d"; done

# 2) Known-answer oracle (mayhem/oracle_test.cpp), built by build.sh with the normal flags and linked to
#    the unsanitized liblbug.a. Output is parsed rather than trusting the exit status.
EXPECTED=22
RUNNER=build/test/oracle_test
if [ ! -x "$RUNNER" ]; then
  echo "missing $RUNNER: mayhem/build.sh did not build the oracle" >&2
  FAILED=$(( FAILED + 1 ))
else
  OUT=$(timeout 900 "$RUNNER" 2>&1)
  printf '%s\n' "$OUT"
  passed=$(printf '%s\n' "$OUT" | grep -c '^PASS ')
  failed=$(printf '%s\n' "$OUT" | grep -c '^FAIL ')
  if [ "$passed" -ne "$EXPECTED" ] && [ "$failed" -eq 0 ]; then
    echo "expected $EXPECTED passing checks, saw $passed" >&2
    failed=$(( EXPECTED - passed ))
  fi
  PASSED=$(( PASSED + passed )) FAILED=$(( FAILED + failed ))
fi

emit_ctrf "ladybug-gtest+oracle" "$PASSED" "$FAILED" "$SKIPPED"
