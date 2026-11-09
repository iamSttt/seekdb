# 07_txn_commit_rollback.sql -- transaction boundaries
--echo === rollback ===
CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(8));
INSERT INTO t VALUES (1, 'keep');
BEGIN;
INSERT INTO t VALUES (2, 'gone');
ROLLBACK;
SELECT * FROM t ORDER BY id;
--echo === commit ===
BEGIN;
INSERT INTO t VALUES (3, 'kept');
UPDATE t SET name = 'kept!' WHERE id = 3;
COMMIT;
SELECT * FROM t ORDER BY id;
--echo === rollback undoes UPDATE and DELETE too ===
BEGIN;
DELETE FROM t WHERE id = 1;
UPDATE t SET name = 'tmp' WHERE id = 3;
ROLLBACK;
SELECT * FROM t ORDER BY id;
--echo === duplicate key inside a transaction, then rollback ===
BEGIN;
INSERT INTO t VALUES (4, 'four');
--error 1062
INSERT INTO t VALUES (1, 'dup');
ROLLBACK;
SELECT * FROM t ORDER BY id;
