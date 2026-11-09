# 09_directives.sql -- exercises the harness directives themselves:
#                     --source, --delimiter, --sorted_result, --replace_column
--echo === sourced setup ===
--source inc/setup_items.sql
SELECT * FROM items;
--sorted_result
SELECT name FROM items;
--echo === delimiter switch ===
--delimiter //
CREATE TABLE d (id BIGINT PRIMARY KEY, name VARCHAR(8))//
INSERT INTO d VALUES (1, 'x')//
INSERT INTO d VALUES (2, 'y')//
SELECT * FROM d ORDER BY id//
--delimiter ;
SELECT id FROM d ORDER BY id;
--echo === replace_column keeps volatile columns stable ===
--replace_column 1 <id>
SELECT id, name FROM d ORDER BY id;
