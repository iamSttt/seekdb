# 03_varchar_utf8.sql -- VARCHAR(n) counts UTF-8 *characters*, not bytes
--echo === varchar utf8 length ===
CREATE TABLE utf8_t (id BIGINT PRIMARY KEY, s VARCHAR(4));
-- 4 CJK characters = 4 code points / 12 bytes: must be accepted
INSERT INTO utf8_t VALUES (1, '中文字符');
-- 1 code point, 4 bytes: must be accepted edge case
INSERT INTO utf8_t VALUES (2, '中');
-- empty string is allowed
INSERT INTO utf8_t VALUES (3, '');
SELECT id, s FROM utf8_t ORDER BY id;
-- 5 code points / 15 bytes: must be rejected as out of range
--error 1264
INSERT INTO utf8_t VALUES (4, '中文字符号');
-- mixed ascii + CJK, still 4 code points
INSERT INTO utf8_t VALUES (5, 'ab中字');
SELECT id, s FROM utf8_t ORDER BY id;
