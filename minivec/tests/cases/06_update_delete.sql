# 06_update_delete.sql -- UPDATE / DELETE row counts and visibility
--echo === update and delete ===
CREATE TABLE u (id BIGINT PRIMARY KEY, name VARCHAR(8), n BIGINT);
INSERT INTO u VALUES (1, 'a', 10), (2, 'b', 20), (3, 'c', 30);
UPDATE u SET name = 'z' WHERE id = 2;
SELECT * FROM u;
UPDATE u SET n = 0 WHERE n > 15;
SELECT * FROM u;
-- update matching nothing reports 0 rows affected
UPDATE u SET n = 1 WHERE id = 99;
DELETE FROM u WHERE id = 1;
SELECT * FROM u;
-- deleting the same row again reports 0 rows affected
DELETE FROM u WHERE id = 1;
-- primary key is free again after DELETE
INSERT INTO u VALUES (1, 'back', 7);
SELECT * FROM u ORDER BY id;
