#!/usr/bin/env python3
"""Self-contained acceptance test suite for the course DBMS.

Each test owns a fresh temporary data directory, so they never share state.
SELECT JSON goes to stdout; status text is suppressed with --quiet; errors
go to stderr. Tests check stdout lines (exact-match) and optionally a list
of substrings that must appear in stderr.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import textwrap
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional


ROOT = Path(__file__).resolve().parent.parent
TESTS_DIR = Path(__file__).resolve().parent

def _find_build_dir() -> Path:
    for name in ("build", "build-wsl"):
        p = ROOT / name
        prog = p / "prog"
        if prog.exists() or (p / "prog.exe").exists():
            return p
    return ROOT / "build"

BUILD = _find_build_dir()
PROG         = BUILD / "prog"
PROG_SERVER  = BUILD / "prog-server"
PROG_CLIENT  = BUILD / "prog-client"
TEST_BULK_BUILD_GUARD = BUILD / "test_bulk_build_guard"

_BSTD_MAGIC = 0x42535444
TEST_BULK_BUILD_GUARD = BUILD / "test_bulk_build_guard"

_BSTD_MAGIC = 0x42535444


# ---------------------------------------------------------------------------
# Tiny test framework
# ---------------------------------------------------------------------------

@dataclass
class Case:
    name: str
    sql: str
    expected_stdout: List[str] = field(default_factory=list)
    expect_stderr_contains: List[str] = field(default_factory=list)
    expect_no_stderr_errors: bool = False
    pre_sql: Optional[str] = None     # executed in the same data dir first
    reuse_data: bool = False          # don't wipe the data dir before running


GREEN = "\033[32m"
RED   = "\033[31m"
DIM   = "\033[2m"
END   = "\033[0m"


def run_prog(sql: str, data_dir: Path, *, script: Optional[Path] = None) -> subprocess.CompletedProcess:
    cmd = [str(PROG), "--data", str(data_dir), "--quiet"]
    if script is not None:
        cmd.append(str(script))
        return subprocess.run(cmd, text=True, capture_output=True, timeout=30)
    return subprocess.run(cmd, input=sql, text=True, capture_output=True, timeout=30)


def assert_json_lines(lines: List[str]) -> Optional[str]:
    """Every non-empty stdout line from SELECT must be valid JSON."""
    for line in lines:
        if not line:
            continue
        try:
            json.loads(line)
        except json.JSONDecodeError as e:
            return f"stdout line is not valid JSON: {line!r} ({e})"
    return None


def run_case(case: Case, data_dir: Path) -> tuple[bool, str]:
    if case.pre_sql is not None:
        pre = run_prog(case.pre_sql, data_dir)
        if pre.returncode != 0:
            return False, f"pre_sql failed: {pre.stderr.strip()}"

    result = run_prog(case.sql, data_dir)

    if result.returncode != 0:
        return False, f"prog exited with {result.returncode}: {result.stderr.strip()}"

    actual_lines = [l for l in result.stdout.splitlines() if l != ""]
    if actual_lines != case.expected_stdout:
        diff = []
        diff.append("    expected stdout:")
        for l in case.expected_stdout: diff.append(f"      {l}")
        diff.append("    actual stdout:")
        for l in actual_lines: diff.append(f"      {l}")
        if result.stderr:
            diff.append("    stderr:")
            for l in result.stderr.splitlines(): diff.append(f"      {l}")
        return False, "\n" + "\n".join(diff)

    json_err = assert_json_lines(actual_lines)
    if json_err:
        return False, json_err

    for needle in case.expect_stderr_contains:
        if needle not in result.stderr:
            return False, f"missing expected stderr substring: {needle!r}\nstderr was:\n{result.stderr}"

    if case.expect_no_stderr_errors and "ERROR" in result.stderr:
        return False, f"unexpected ERROR in stderr:\n{result.stderr}"

    return True, ""


# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

CASES: List[Case] = []


def add(case: Case) -> None:
    CASES.append(case)


# 1. CREATE/USE/INSERT/SELECT/DROP basic happy-path
add(Case(
    name="01 - basic CRUD lifecycle",
    sql=textwrap.dedent("""
        CREATE DATABASE d1;
        USE d1;
        CREATE TABLE t (id int INDEXED, name string NOT_NULL);
        INSERT INTO t (id, name) VALUE (1, "a"), (2, "b");
        SELECT * FROM t;
        DROP TABLE t;
        DROP DATABASE d1;
    """),
    expected_stdout=[
        '[{"id":1,"name":"a"},{"id":2,"name":"b"}]',
    ],
    expect_no_stderr_errors=True,
))

# 2. All comparison operators on int
add(Case(
    name="02 - integer comparisons ==, !=, <, >, <=, >=",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (x int);
        INSERT INTO t (x) VALUE (1),(2),(3),(4),(5);
        SELECT x FROM t WHERE x == 3;
        SELECT x FROM t WHERE x != 3;
        SELECT x FROM t WHERE x < 3;
        SELECT x FROM t WHERE x > 3;
        SELECT x FROM t WHERE x <= 3;
        SELECT x FROM t WHERE x >= 3;
    """),
    expected_stdout=[
        '[{"x":3}]',
        '[{"x":1},{"x":2},{"x":4},{"x":5}]',
        '[{"x":1},{"x":2}]',
        '[{"x":4},{"x":5}]',
        '[{"x":1},{"x":2},{"x":3}]',
        '[{"x":3},{"x":4},{"x":5}]',
    ],
))

# 3. Lexicographic string comparison
add(Case(
    name="03 - lexicographic string comparison",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (s string);
        INSERT INTO t (s) VALUE ("apple"),("banana"),("Apple"),("ZZ");
        SELECT s FROM t WHERE s < "banana";
        SELECT s FROM t WHERE s > "apple";
    """),
    expected_stdout=[
        '[{"s":"apple"},{"s":"Apple"},{"s":"ZZ"}]',
        '[{"s":"banana"}]',
    ],
))

# 4. BETWEEN — closed interval [lo, hi] per main_task.txt
add(Case(
    name="04 - BETWEEN is closed [lo, hi]",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (x int);
        INSERT INTO t (x) VALUE (9),(10),(15),(19),(20),(21);
        SELECT x FROM t WHERE x BETWEEN 10 AND 20;
    """),
    expected_stdout=[
        '[{"x":10},{"x":15},{"x":19},{"x":20}]',
    ],
))

# 5. LIKE regex
add(Case(
    name="05 - LIKE as full-match regex",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (s string);
        INSERT INTO t (s) VALUE ("alpha"),("beta"),("ALPHA"),("a1b2c3");
        SELECT s FROM t WHERE s LIKE "a.*";
        SELECT s FROM t WHERE s LIKE "[a-z][0-9].*";
    """),
    expected_stdout=[
        '[{"s":"alpha"},{"s":"a1b2c3"}]',
        '[{"s":"a1b2c3"}]',
    ],
))

# 6. NOT_NULL violation
add(Case(
    name="06 - NOT_NULL rejects NULL on partial INSERT",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (a int, b int NOT_NULL);
        INSERT INTO t (a) VALUE (1);
        SELECT * FROM t;
    """),
    expected_stdout=[
        '[]',
    ],
    expect_stderr_contains=["NULL not allowed in column 'b'"],
))

# 7. INDEXED uniqueness + auto NOT_NULL
add(Case(
    name="07 - INDEXED enforces uniqueness and NOT_NULL",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (id int INDEXED, x int);
        INSERT INTO t (id, x) VALUE (1, 100);
        INSERT INTO t (id, x) VALUE (1, 200);
        INSERT INTO t (x) VALUE (300);
        SELECT * FROM t;
    """),
    expected_stdout=[
        '[{"id":1,"x":100}]',
    ],
    expect_stderr_contains=[
        "Duplicate value in indexed column 'id'",
        "NULL not allowed in column 'id'",
    ],
))

# 8. DEFAULT values
add(Case(
    name="08 - DEFAULT used when column omitted",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (
            id int INDEXED,
            tag string DEFAULT "auto",
            n   int    NOT_NULL DEFAULT 42
        );
        INSERT INTO t (id) VALUE (1);
        INSERT INTO t (id, tag) VALUE (2, "manual");
        INSERT INTO t (id, n) VALUE (3, 7);
        SELECT * FROM t;
    """),
    expected_stdout=[
        '[{"id":1,"tag":"auto","n":42},{"id":2,"tag":"manual","n":42},{"id":3,"tag":"auto","n":7}]',
    ],
))

# 9. DEFAULT type mismatch is rejected at parse time
add(Case(
    name="09 - DEFAULT literal must match column type",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (a int DEFAULT "oops");
        SELECT * FROM t;
    """),
    expected_stdout=[
    ],
    expect_stderr_contains=[
        "DEFAULT literal type does not match column 'a'",
        "Unknown table 't'",
    ],
))

# 10. AND/OR precedence and parens (task 11)
add(Case(
    name="10 - AND/OR precedence with parentheses",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (a int, b int, c int);
        INSERT INTO t (a,b,c) VALUE
            (1,1,1),(1,2,2),(2,1,3),(2,2,4),(3,3,5);
        SELECT a,b,c FROM t WHERE a == 1 OR a == 2 AND b == 2;
        SELECT a,b,c FROM t WHERE (a == 1 OR a == 2) AND b == 2;
    """),
    expected_stdout=[
        # AND binds tighter than OR -> rows where a==1, OR (a==2 AND b==2)
        '[{"a":1,"b":1,"c":1},{"a":1,"b":2,"c":2},{"a":2,"b":2,"c":4}]',
        # parens override -> rows where (a in {1,2}) AND b==2
        '[{"a":1,"b":2,"c":2},{"a":2,"b":2,"c":4}]',
    ],
))

# 11. Aggregates SUM/COUNT/AVG (task 12) — incl. COUNT(*) and COUNT(col)
#     COUNT(col) must skip NULLs.
add(Case(
    name="11 - SUM/COUNT/AVG and COUNT(*) vs COUNT(col)",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (id int, v int);
        INSERT INTO t (id, v) VALUE (1, 10), (2, 20), (3, 30), (4, 40);
        INSERT INTO t (id) VALUE (5);
        SELECT COUNT(*) AS n FROM t;
        SELECT COUNT(v) AS n FROM t;
        SELECT SUM(v) AS s FROM t;
        SELECT AVG(v) AS a FROM t;
        SELECT COUNT(*) AS n FROM t WHERE v > 20;
    """),
    expected_stdout=[
        '[{"n":5}]',
        '[{"n":4}]',
        '[{"s":100}]',
        '[{"a":25}]',
        '[{"n":2}]',
    ],
))

# 12. SELECT aliases
add(Case(
    name="12 - SELECT column AS alias",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (id int, name string);
        INSERT INTO t (id, name) VALUE (1, "ann");
        SELECT id AS who, name AS label FROM t;
    """),
    expected_stdout=[
        '[{"who":1,"label":"ann"}]',
    ],
))

# 13. UPDATE rewrites rows; SET expr can reference other columns
add(Case(
    name="13 - UPDATE uses WHERE and may reference columns",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (id int INDEXED, x int, y int);
        INSERT INTO t (id, x, y) VALUE (1, 5, 10), (2, 7, 20), (3, 9, 30);
        UPDATE t SET y = x WHERE id == 2;
        SELECT id, x, y FROM t;
    """),
    expected_stdout=[
        '[{"id":1,"x":5,"y":10},{"id":2,"x":7,"y":7},{"id":3,"x":9,"y":30}]',
    ],
))

# 14. DELETE with WHERE; remaining rows preserve insertion order
add(Case(
    name="14 - DELETE with WHERE",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (id int INDEXED, x int);
        INSERT INTO t (id, x) VALUE (1, 10), (2, 20), (3, 30), (4, 40);
        DELETE FROM t WHERE x BETWEEN 15 AND 35;
        SELECT id FROM t;
    """),
    expected_stdout=[
        '[{"id":1},{"id":4}]',
    ],
))

# 15. db.table form works without USE
add(Case(
    name="15 - fully-qualified database.table reference",
    sql=textwrap.dedent("""
        CREATE DATABASE d;
        CREATE TABLE d.t (id int);
        INSERT INTO d.t (id) VALUE (1),(2);
        SELECT * FROM d.t;
        DROP TABLE d.t;
        DROP DATABASE d;
    """),
    expected_stdout=[
        '[{"id":1},{"id":2}]',
    ],
))

# 16. Case-insensitivity + mixed case rejected by lexer (per spec: "смешение
#     регистров в одном слове не допускается" — so even 'Create' is invalid).
add(Case(
    name="16 - case-insensitive keywords; any mixed case is a lex error",
    sql=textwrap.dedent("""
        create DATABASE d;
        USE d;
        CREATE TABLE t (id int);
        insert into t (id) value (1);
        SELECT * FROM t;
        CrEaTe TABLE bad (id int);
        Create TABLE alsobad (id int);
    """),
    expected_stdout=[
        '[{"id":1}]',
    ],
    expect_stderr_contains=[
        "Mixed case in keyword 'CrEaTe'",
        "Mixed case in keyword 'Create'",
    ],
))

# 17. Identifier rules: a name may not start with a digit.
add(Case(
    name="17 - identifier may not start with a digit",
    sql=textwrap.dedent("""
        CREATE DATABASE 1bad;
    """),
    expect_stderr_contains=["Parse error", "INT_LIT"],
))

# 18. Multi-line statements and comments
add(Case(
    name="18 - multi-line statement, line and block comments",
    sql=textwrap.dedent("""
        CREATE DATABASE d;
        USE d;
        /* block
           comment */
        CREATE TABLE t (
            id int INDEXED,
            -- inline trailing comment
            name string NOT_NULL
        );
        INSERT INTO t (id, name) VALUE (1, "x");
        SELECT * FROM t;
    """),
    expected_stdout=[
        '[{"id":1,"name":"x"}]',
    ],
))

# 19. Process recovers from a series of bad statements (must not crash).
add(Case(
    name="19 - no crash on a series of bad statements",
    sql=textwrap.dedent("""
        USE nope;
        SELECT * FROM also_nope;
        CREATE TABLE bad_no_db (id int);
        CREATE DATABASE d;
        USE d;
        CREATE TABLE t (id int INDEXED, s string);
        INSERT INTO t (id, s) VALUE (1, "hello");
        SELECT * FROM t WHERE name == 1;
        SELECT * FROM t WHERE s LIKE "[unterm";
        SELECT no_such FROM t;
        SELECT id FROM t;
    """),
    expected_stdout=[
        '[{"id":1}]',
    ],
    expect_stderr_contains=[
        "Unknown database 'nope'",
        "No database selected",
        "Unknown column 'name'",
        "Bad regex in LIKE",
        "Unknown column 'no_such'",
    ],
))

# 20. Empty result set returns []
add(Case(
    name="20 - SELECT with no matching rows returns []",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (id int);
        INSERT INTO t (id) VALUE (1),(2);
        SELECT * FROM t WHERE id == 999;
    """),
    expected_stdout=[
        '[]',
    ],
))

# 21. NULL semantics in WHERE (excluded)
add(Case(
    name="21 - WHERE on NULL is unknown -> row excluded",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (id int, v int);
        INSERT INTO t (id, v) VALUE (1, 5);
        INSERT INTO t (id) VALUE (2);
        SELECT id FROM t WHERE v == 5;
        SELECT id FROM t WHERE v != 5;
    """),
    expected_stdout=[
        '[{"id":1}]',
        '[]',
    ],
))

# 22. Index-backed range scan returns rows in key order
add(Case(
    name="22 - INDEXED range scan returns rows ordered by key",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (id int INDEXED, x int);
        INSERT INTO t (id, x) VALUE
            (50, 1),(10, 2),(40, 3),(20, 4),(30, 5);
        SELECT id FROM t WHERE id BETWEEN 15 AND 45;
        SELECT id FROM t WHERE id >= 30;
    """),
    expected_stdout=[
        '[{"id":20},{"id":30},{"id":40}]',
        '[{"id":30},{"id":40},{"id":50}]',
    ],
))

# 23. INSERT without column list uses schema order
add(Case(
    name="23 - INSERT without column list uses schema order",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (a int, b string, c int);
        INSERT INTO t VALUE (1, "x", 7);
        SELECT * FROM t;
    """),
    expected_stdout=[
        '[{"a":1,"b":"x","c":7}]',
    ],
))

# 24. Mixed type in compare -> unknown, row excluded
add(Case(
    name="24 - cross-type comparison yields UNKNOWN",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (id int, s string);
        INSERT INTO t (id, s) VALUE (1, "a");
        SELECT * FROM t WHERE id == "a";
        SELECT * FROM t WHERE s == 1;
    """),
    expected_stdout=[
        '[]',
        '[]',
    ],
))

# 25. Variables on either side of an operator (col op lit AND lit op col).
add(Case(
    name="25 - constant on the left of a comparison",
    sql=textwrap.dedent("""
        CREATE DATABASE d; USE d;
        CREATE TABLE t (x int);
        INSERT INTO t (x) VALUE (1),(2),(3),(4);
        SELECT x FROM t WHERE 2 <= x;
        SELECT x FROM t WHERE 3 == x;
    """),
    expected_stdout=[
        '[{"x":2},{"x":3},{"x":4}]',
        '[{"x":3}]',
    ],
))

# 26. Persistence across process boundary using INDEXED column.
add(Case(
    name="26 - persistence across restart, index reloaded",
    pre_sql=textwrap.dedent("""
        CREATE DATABASE p; USE p;
        CREATE TABLE t (id int INDEXED, name string NOT_NULL);
        INSERT INTO t (id, name) VALUE (10, "ten"),(20, "twenty"),(30, "thirty");
    """),
    sql=textwrap.dedent("""
        USE p;
        SELECT * FROM t;
        SELECT name FROM t WHERE id == 20;
        SELECT id FROM t WHERE id BETWEEN 15 AND 100;
    """),
    expected_stdout=[
        '[{"id":10,"name":"ten"},{"id":20,"name":"twenty"},{"id":30,"name":"thirty"}]',
        '[{"name":"twenty"}]',
        '[{"id":20},{"id":30}]',
    ],
    reuse_data=True,
))


# ---------------------------------------------------------------------------
# Server / log / telemetry checks
# ---------------------------------------------------------------------------

def test_client_server_and_logs() -> tuple[bool, str]:
    """Run prog-server, drive it with prog-client, validate the access log
    contains the required fields and that the server prints telemetry."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        data_dir = tmp / "data"
        log_file = tmp / "access.log"
        port = _free_port()

        srv = subprocess.Popen(
            [str(PROG_SERVER), "--port", str(port),
             "--data", str(data_dir), "--log", str(log_file)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            if not _wait_for_port(port, timeout=5):
                return False, "server did not start in time"

            sql = textwrap.dedent("""
                CREATE DATABASE s; USE s;
                CREATE TABLE t (id int INDEXED, x int);
                INSERT INTO t (id, x) VALUE (1, 100), (2, 200);
                SELECT * FROM t;
                SELECT * FROM t WHERE id == 1;
                BAD STATEMENT;
            """)
            client = subprocess.run(
                [str(PROG_CLIENT), "--port", str(port)],
                input=sql, text=True, capture_output=True, timeout=10,
            )
            if client.returncode != 0:
                return False, f"client exit {client.returncode}: {client.stderr}"

            # The client echoes every server response verbatim (status text
            # for DDL/DML, JSON for SELECT). Only the JSON lines need to be
            # byte-exact for this test.
            json_lines = [l for l in client.stdout.splitlines() if l.startswith("[")]
            expected = [
                '[{"id":1,"x":100},{"id":2,"x":200}]',
                '[{"id":1,"x":100}]',
            ]
            if json_lines != expected:
                return False, f"client JSON lines mismatch:\n  want: {expected}\n  got : {json_lines}"
            if "ERROR" not in client.stderr:
                return False, "client should have surfaced the bad statement error"

            time.sleep(0.3)
            log_lines = log_file.read_text().splitlines()
            if len(log_lines) < 6:
                return False, f"expected >=6 log lines, got {len(log_lines)}"
            required_keys = {
                "request_id", "client_id", "handler_id",
                "started_at", "ended_at", "duration_us",
                "status", "status_text", "request",
            }
            for line in log_lines:
                entry = json.loads(line)
                missing = required_keys - entry.keys()
                if missing:
                    return False, f"log entry missing keys {missing}: {line}"
            statuses = {json.loads(l)["status"] for l in log_lines}
            if 200 not in statuses or 400 not in statuses:
                return False, f"expected both 200 and 400 statuses, got {statuses}"
        finally:
            srv.terminate()
            try:
                stdout, stderr = srv.communicate(timeout=6)
            except subprocess.TimeoutExpired:
                srv.kill()
                stdout, stderr = srv.communicate()

        if "[telemetry]" not in stderr:
            return False, "server did not print telemetry to stderr"
        m = re.search(r"\[telemetry\] rps=(\S+) avg10m=(\S+) max10m=(\S+) "
                      r"lat10s_us=(\S+) err1m=(\S+) total=(\d+) "
                      r"\(errors=(\d+)\)", stderr)
        if not m:
            return False, f"telemetry line not in expected format:\n{stderr}"
        total = int(m.group(6))
        errors = int(m.group(7))
        if total < 6:
            return False, f"telemetry total={total} (expected >= 6)"
        if errors < 1:
            return False, f"telemetry errors={errors} (expected >= 1)"

    return True, ""


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _pack_u32(x: int) -> bytes:
    return struct.pack("<I", x)


def _pack_u64(x: int) -> bytes:
    return struct.pack("<Q", x)


def _pack_string(s: str) -> bytes:
    b = s.encode("utf-8")
    return _pack_u32(len(b)) + b


def _pack_value_null() -> bytes:
    return bytes([0])


def _pack_value_int(x: int) -> bytes:
    return bytes([1]) + struct.pack("<q", x)


def write_table_dat_v1(
    path: Path,
    table_name: str,
    columns: list[tuple[str, int, int]],
    records: list[tuple[int, list[bytes]]],
    next_id: int,
) -> None:
    """Write format-v1 table.dat. columns: (name, type, flags)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as f:
        f.write(_pack_u32(_BSTD_MAGIC))
        f.write(_pack_u32(1))
        f.write(_pack_string(table_name))
        f.write(_pack_u32(len(columns)))
        for name, dtype, flags in columns:
            f.write(_pack_string(name))
            f.write(struct.pack("<BB", dtype, flags))
        f.write(_pack_u64(next_id))
        f.write(_pack_u64(len(records)))
        for rid, values in records:
            f.write(_pack_u64(rid))
            for blob in values:
                f.write(blob)


def test_bulk_build_guard() -> tuple[bool, str]:
    """test_bulk_build_guard binary + v1 load with NULL in INDEXED column."""
    if not TEST_BULK_BUILD_GUARD.exists():
        return False, f"missing {TEST_BULK_BUILD_GUARD}; rebuild after adding tests"

    unit = subprocess.run(
        [str(TEST_BULK_BUILD_GUARD)],
        text=True,
        capture_output=True,
        timeout=30,
    )
    if unit.returncode != 0:
        return False, (unit.stderr or unit.stdout or "unit test failed").strip()

    with tempfile.TemporaryDirectory() as tmp:
        data = Path(tmp) / "data"
        db_dir = data / "bulk_v1"
        db_dir.mkdir(parents=True)
        write_table_dat_v1(
            db_dir / "t.dat",
            "t",
            [("id", 1, 2), ("x", 1, 0)],
            [(1, [_pack_value_null(), _pack_value_int(10)])],
            next_id=2,
        )
        load = subprocess.run(
            [str(PROG), "--data", str(data), "--quiet"],
            input="USE bulk_v1;\nSELECT * FROM t;\n",
            text=True,
            capture_output=True,
            timeout=30,
        )
        if load.returncode == 0:
            return False, "expected load failure for v1 table with NULL in INDEXED column"
        err = load.stderr
        if "NULL in record id" not in err and "Cannot bulk-build index" not in err:
            return False, f"missing bulk NULL migration error:\n{err}"

    return True, ""


def _pack_u32(x: int) -> bytes:
    return struct.pack("<I", x)


def _pack_u64(x: int) -> bytes:
    return struct.pack("<Q", x)


def _pack_string(s: str) -> bytes:
    b = s.encode("utf-8")
    return _pack_u32(len(b)) + b


def _pack_value_null() -> bytes:
    return bytes([0])


def _pack_value_int(x: int) -> bytes:
    return bytes([1]) + struct.pack("<q", x)


def write_table_dat_v1(
    path: Path,
    table_name: str,
    columns: list[tuple[str, int, int]],
    records: list[tuple[int, list[bytes]]],
    next_id: int,
) -> None:
    """Write format-v1 table.dat. columns: (name, type, flags)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as f:
        f.write(_pack_u32(_BSTD_MAGIC))
        f.write(_pack_u32(1))
        f.write(_pack_string(table_name))
        f.write(_pack_u32(len(columns)))
        for name, dtype, flags in columns:
            f.write(_pack_string(name))
            f.write(struct.pack("<BB", dtype, flags))
        f.write(_pack_u64(next_id))
        f.write(_pack_u64(len(records)))
        for rid, values in records:
            f.write(_pack_u64(rid))
            for blob in values:
                f.write(blob)


def test_bulk_build_guard() -> tuple[bool, str]:
    """test_bulk_build_guard binary + v1 load with NULL in INDEXED column."""
    if not TEST_BULK_BUILD_GUARD.exists():
        return False, f"missing {TEST_BULK_BUILD_GUARD}; rebuild after adding tests"

    unit = subprocess.run(
        [str(TEST_BULK_BUILD_GUARD)],
        text=True,
        capture_output=True,
        timeout=30,
    )
    if unit.returncode != 0:
        return False, (unit.stderr or unit.stdout or "unit test failed").strip()

    with tempfile.TemporaryDirectory() as tmp:
        data = Path(tmp) / "data"
        db_dir = data / "bulk_v1"
        db_dir.mkdir(parents=True)
        write_table_dat_v1(
            db_dir / "t.dat",
            "t",
            [("id", 1, 2), ("x", 1, 0)],
            [(1, [_pack_value_null(), _pack_value_int(10)])],
            next_id=2,
        )
        load = subprocess.run(
            [str(PROG), "--data", str(data), "--quiet"],
            input="USE bulk_v1;\nSELECT * FROM t;\n",
            text=True,
            capture_output=True,
            timeout=30,
        )
        if load.returncode == 0:
            return False, "expected load failure for v1 table with NULL in INDEXED column"
        err = load.stderr
        if "NULL in record id" not in err and "Cannot bulk-build index" not in err:
            return False, f"missing bulk NULL migration error:\n{err}"

    return True, ""


def _wait_for_port(port: int, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def test_sql_scripts() -> tuple[bool, str]:
    """Run the example SQL files shipped under tests/."""
    scripts = [
        ("28 - tests/demo.sql", TESTS_DIR / "demo.sql", None, False),
        ("29 - tests/full_features.sql", TESTS_DIR / "full_features.sql", None, False),
        ("30 - tests/persist1+2", None, "persist", False),
        ("31 - tests/errors.sql", TESTS_DIR / "errors.sql", None, True),
    ]
    for label, path, mode, expect_errors in scripts:
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp)
            if mode == "persist":
                p1 = TESTS_DIR / "persist1.sql"
                p2 = TESTS_DIR / "persist2.sql"
                if not p1.exists() or not p2.exists():
                    return False, f"missing persist scripts in {TESTS_DIR}"
                r1 = run_prog("", data, script=p1)
                if r1.returncode != 0:
                    return False, f"{label}: persist1 exit {r1.returncode}: {r1.stderr}"
                r2 = run_prog("", data, script=p2)
                result = r2
            else:
                if path is None or not path.exists():
                    return False, f"missing script for {label}: {path}"
                result = run_prog("", data, script=path)

            if result.returncode != 0:
                return False, f"{label}: prog exit {result.returncode}: {result.stderr}"

            lines = [l for l in result.stdout.splitlines() if l]
            for line in lines:
                err = assert_json_lines([line])
                if err:
                    return False, f"{label}: {err}"

            if expect_errors and "ERROR" not in result.stderr:
                return False, f"{label}: expected errors on stderr, got none"
            if not expect_errors and "ERROR: Parse error" in result.stderr:
                return False, f"{label}: unexpected parse error:\n{result.stderr}"
    return True, ""


def main() -> int:
    if not PROG.exists():
        print(f"{RED}prog not found at {PROG}; build first.{END}")
        return 2

    n_pass = n_fail = 0

    for case in CASES:
        with tempfile.TemporaryDirectory() as tmp:
            ok, msg = run_case(case, Path(tmp))
        status = f"{GREEN}PASS{END}" if ok else f"{RED}FAIL{END}"
        print(f"  {status}  {case.name}")
        if ok:
            n_pass += 1
        else:
            n_fail += 1
            print(f"{DIM}{msg}{END}")

    # client-server / log / telemetry suite
    if PROG_SERVER.exists() and PROG_CLIENT.exists():
        ok, msg = test_client_server_and_logs()
        status = f"{GREEN}PASS{END}" if ok else f"{RED}FAIL{END}"
        print(f"  {status}  27 - client-server + access log + telemetry")
        if ok:
            n_pass += 1
        else:
            n_fail += 1
            print(f"{DIM}{msg}{END}")
    else:
        print(f"  {RED}SKIP{END}  27 - client-server (binaries missing)")

    ok, msg = test_sql_scripts()
    status = f"{GREEN}PASS{END}" if ok else f"{RED}FAIL{END}"
    print(f"  {status}  28-31 - tests/*.sql example scripts")
    if ok:
        n_pass += 1
    else:
        n_fail += 1
        print(f"{DIM}{msg}{END}")

    ok, msg = test_bulk_build_guard()
    status = f"{GREEN}PASS{END}" if ok else f"{RED}FAIL{END}"
    print(f"  {status}  32 - bulk_build_guard (unit + v1 NULL migration)")
    if ok:
        n_pass += 1
    else:
        n_fail += 1
        print(f"{DIM}{msg}{END}")

    print()
    print(f"{n_pass} passed, {n_fail} failed")
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
