CREATE DATABASE shop;
USE shop;
CREATE TABLE products (
    sku   string INDEXED,
    name  string NOT_NULL,
    stock int    NOT_NULL DEFAULT 0
);
INSERT INTO products (sku, name, stock) VALUE
    ("A-001", "Apple",  100),
    ("B-002", "Banana", 200),
    ("C-003", "Cherry", 50);
SELECT * FROM shop.products;
