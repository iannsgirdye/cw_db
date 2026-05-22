-- Each statement here is wrong; the program must keep running and emit a
-- descriptive error for every one.

USE no_such_db;
SELECT * FROM nope;
CREATE DATABASE good;
USE good;
CREATE TABLE t (id int INDEXED, name string NOT_NULL);

-- duplicate INDEXED
INSERT INTO t (id, name) VALUE (1, "a"), (1, "b");

-- NOT_NULL violated by missing value with no default
INSERT INTO t (id) VALUE (2);

-- type mismatch
INSERT INTO t (id, name) VALUE ("x", "y");

-- unknown column
SELECT no_such FROM t;

-- mixed-case keyword (must be reported by the lexer)
CrEaTe TABLE bad (id int);

-- forgotten ';' at the end -> handled by main on EOF, this line is fine.
SELECT * FROM t;

-- malformed regex
SELECT * FROM t WHERE name LIKE "[";
DROP DATABASE good;
