-- End-to-end exercise of every feature mandated by ОСНОВНОЕ ЗАДАНИЕ.txt
-- and the easy half of ДОПОЛНИТЕЛЬНЫЕ ЗАДАНИЯ.txt.

CREATE DATABASE library;
USE library;

-- DEFAULT for non-NOT_NULL column, NOT_NULL, INDEXED.
CREATE TABLE books (
    id        int    INDEXED,
    title     string NOT_NULL,
    author    string NOT_NULL,
    year      int    DEFAULT 2000,
    note      string
);

INSERT INTO books (id, title, author, year, note) VALUE
    (1, "The Pragmatic Programmer", "Hunt",     1999, "classic"),
    (2, "Effective C++",            "Meyers",   2005, "guidelines"),
    (3, "Database Internals",       "Petrov",   2019, "B-trees"),
    (4, "Modern C++ Design",        "Alexandrescu", 2001, "templates"),
    (5, "Designing Data-Intensive Applications", "Kleppmann", 2017, "systems");

-- partial INSERT - dept will get the default
INSERT INTO books (id, title, author) VALUE (6, "Refactoring", "Fowler");

-- fully qualified table name
SELECT * FROM library.books;

-- column aliases
SELECT title AS Name, year AS Y FROM books;

-- WHERE with AND/OR + parentheses (task 11)
SELECT title, year FROM books
WHERE (year >= 2010 AND author != "Kleppmann") OR title LIKE "Effective.*";

-- BETWEEN [a, b] (both bounds inclusive, per main_task.txt)
SELECT title FROM books WHERE year BETWEEN 2001 AND 2010;

-- LIKE regex over string column
SELECT title FROM books WHERE author LIKE "M[a-z]+s";

-- Aggregates (task 12)
SELECT COUNT(*) AS total, SUM(year) AS sum_year, AVG(year) AS avg_year FROM books;
SELECT COUNT(note) AS with_note FROM books;

-- UPDATE with WHERE; the indexed column id is preserved
UPDATE books SET note = "must-read" WHERE year >= 2017;
SELECT title, note FROM books WHERE note == "must-read";

-- DELETE using indexed equality (uses B*+-tree lookup)
DELETE FROM books WHERE id == 6;

-- Range delete using indexed column
DELETE FROM books WHERE id BETWEEN 4 AND 100;

SELECT id, title FROM books;

DROP TABLE books;
DROP DATABASE library;
