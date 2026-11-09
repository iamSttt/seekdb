# 08_restart_persistence.sql -- data must survive a process restart
-- the (unpublished) official driver may do the same; --restart kills the
# server and respawns it on the SAME data directory.
--echo === before restart ===
CREATE TABLE p (id BIGINT PRIMARY KEY, name VARCHAR(8), v VECTOR(2));
INSERT INTO p VALUES (1, 'one', [1,2]), (2, 'two', [3,4]);
BEGIN;
INSERT INTO p VALUES (3, 'three', [5,6]);
COMMIT;
SELECT * FROM p ORDER BY id;
--restart
--echo === after restart ===
SHOW TABLES;
SELECT * FROM p ORDER BY id;
-- an uncommitted row must NOT survive: it was rolled back before the restart
SELECT id, L2_DISTANCE(v, [0,0]) AS d FROM p ORDER BY d;
-- writes after the restart are durable too
INSERT INTO p VALUES (4, 'four', [0,0]);
--restart
--echo === after second restart ===
SELECT * FROM p ORDER BY id;
-- deleting then restarting keeps the delete
DELETE FROM p WHERE id = 4;
--restart
SELECT id, name FROM p ORDER BY id;
