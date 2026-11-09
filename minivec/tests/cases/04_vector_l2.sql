# 04_vector_l2.sql -- VECTOR storage + L2_DISTANCE ordering
--echo === vector insert and L2 ordering ===
CREATE TABLE vec_t (id BIGINT PRIMARY KEY, v VECTOR(2));
INSERT INTO vec_t VALUES (1, [3,4]), (2, [0,0]), (3, [1,1]), (4, [6,8]);
-- distances from [0,0]: 0, 1.414..., 5, 10
SELECT id, L2_DISTANCE(v, [0,0]) AS d FROM vec_t ORDER BY d;
SELECT id, L2_DISTANCE(v, [0,0]) AS d FROM vec_t ORDER BY d DESC;
-- dimension mismatch is an error, not silent padding
--error 1264
SELECT L2_DISTANCE(v, [0,0,0]) FROM vec_t;
-- distance on a non-vector column is rejected
--error 1235
SELECT L2_DISTANCE(id, [0,0]) FROM vec_t;
-- vector column round-trips through * without loss
SELECT * FROM vec_t;
