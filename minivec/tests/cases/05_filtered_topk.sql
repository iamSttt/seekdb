# 05_filtered_topk.sql -- WHERE + ORDER BY distance + LIMIT (the TopK path)
--echo === filtered top-k ===
CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), v VECTOR(2));
INSERT INTO docs VALUES (1, 'a', [1,0]), (2, 'a', [0,2]), (3, 'b', [9,9]);
INSERT INTO docs VALUES (4, 'a', [1,1]), (5, 'b', [0,0]);
-- all rows, nearest to [0,0]
SELECT id, category, L2_DISTANCE(v, [0,0]) AS d FROM docs ORDER BY d;
-- filtered: only category 'a', nearest two
SELECT id, category, L2_DISTANCE(v, [0,0]) AS d FROM docs WHERE category = 'a' ORDER BY d LIMIT 2;
-- LIMIT larger than the result set
SELECT id, L2_DISTANCE(v, [0,0]) AS d FROM docs WHERE category = 'b' ORDER BY d LIMIT 10;
-- LIMIT 0 and a predicate that matches nothing
SELECT id FROM docs WHERE category = 'zzz' ORDER BY id;
-- numeric predicate on the primary key
SELECT id, category FROM docs WHERE id >= 4 ORDER BY id;
