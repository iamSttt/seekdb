# 09_directives.sql -- exercises the harness directives themselves:
#                     --source, --delimiter, --sorted_result, --replace_column
#
# NOTE: with the current src/main.cpp the server buffers stdin in 64 KiB blocks,
# so the harness runs in "bulk" feed mode and cannot attribute output to single
# statements; --sorted_result / --replace_column are then parsed but not applied
# (the harness warns). This case still proves --source and --delimiter work.
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
