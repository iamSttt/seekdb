# 01_basic_crud.sql -- create / insert / select, plus SHOW TABLES
--echo === create / insert / select ===
CREATE TABLE items (id BIGINT PRIMARY KEY, name VARCHAR(16), v VECTOR(3));
INSERT INTO items VALUES (1, 'apple', [1,0,0]);
INSERT INTO items VALUES (2, 'banana', [0,1,0]), (3, 'cherry', [0,0,1]);
SHOW TABLES;
SELECT * FROM items;
SELECT id, name FROM items ORDER BY id;
SELECT name FROM items WHERE id = 2;
