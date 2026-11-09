import subprocess, tempfile, shutil

BIN = "./minivec"
tests = [
    ("id=0", "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));\nINSERT INTO t VALUES (0,'[0.5,0.5]');\nSELECT id FROM t;\n"),
    ("id=1", "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));\nINSERT INTO t VALUES (1,'[0.5,0.5]');\nSELECT id FROM t;\n"),
    ("id=-1", "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));\nINSERT INTO t VALUES (-1,'[0.5,0.5]');\nSELECT id FROM t;\n"),
    ("no-col-list", "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));\nINSERT INTO t VALUES 0,'[0.5,0.5]';\nSELECT id FROM t;\n"),
]
for name, sql in tests:
    d = tempfile.mkdtemp()
    p = subprocess.run([BIN, "--data", d], input=sql.encode(), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    print(name, "->", p.stdout.decode().replace("\n", " | ").strip()[:160])
    shutil.rmtree(d)
