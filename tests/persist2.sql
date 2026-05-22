USE shop;
SELECT * FROM products;
SELECT name, stock FROM products WHERE sku == "B-002";
SELECT sku FROM products WHERE sku BETWEEN "A" AND "C";
INSERT INTO products (sku, name, stock) VALUE ("D-004", "Date", 10);
SELECT name FROM products WHERE sku == "D-004";
