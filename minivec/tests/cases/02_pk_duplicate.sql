# 02_pk_duplicate.sql -- duplicate primary key must be rejected with 1062
--echo === primary key uniqueness ===
CREATE TABLE pk_t (id BIGINT PRIMARY KEY, name VARCHAR(8));
INSERT INTO pk_t VALUES (1, 'one');
INSERT INTO pk_t VALUES (2, 'two');
--error 1062
INSERT INTO pk_t VALUES (1, 'dup');
-- the failed insert must not have changed anything
SELECT * FROM pk_t;
SELECT id, name FROM pk_t ORDER BY id;
-- multi-row insert where the second row clashes: 1062, first row still applied
--error 1062
INSERT INTO pk_t VALUES (3, 'three'), (2, 'dup2');
SELECT id, name FROM pk_t ORDER BY id;
