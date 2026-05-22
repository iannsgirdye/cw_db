-- Basic smoke test covering every supported statement and additional
-- task features (DEFAULT, AND/OR/parens, aggregates).

CREATE DATABASE company;
USE company;

CREATE TABLE employees (
    id     int INDEXED,
    name   string NOT_NULL,
    dept   string DEFAULT "general",
    salary int NOT_NULL DEFAULT 50000
);

INSERT INTO employees (id, name, dept, salary) VALUE
    (1, "Alice",  "eng",   120000),
    (2, "Bob",    "eng",    95000),
    (3, "Carol",  "ops",    80000),
    (4, "Dave",   "ops",    65000),
    (5, "Erin",   "ops",   110000);

-- Defaults: dept and salary missing.
INSERT INTO employees (id, name) VALUE (6, "Frank");

SELECT * FROM employees;

-- WHERE with logical operators and parentheses.
SELECT name AS who, salary FROM employees
WHERE (dept == "ops" AND salary >= 80000) OR name == "Alice";

-- BETWEEN (closed interval [lo, hi] per assignment).
SELECT name, salary FROM employees WHERE salary BETWEEN 80000 AND 100000;

-- LIKE: regex.
SELECT name FROM employees WHERE name LIKE "^[ABE].*";

-- Aggregates over a filtered set.
SELECT COUNT(*) AS n, SUM(salary) AS total, AVG(salary) AS avg
FROM employees WHERE dept == "ops";

-- UPDATE with WHERE.
UPDATE employees SET salary = 70000 WHERE name == "Dave";
SELECT name, salary FROM employees WHERE name == "Dave";

-- DELETE.
DELETE FROM employees WHERE salary < 60000;
SELECT * FROM employees;

DROP TABLE employees;
DROP DATABASE company;
