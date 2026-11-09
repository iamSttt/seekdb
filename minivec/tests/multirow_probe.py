import subprocess, tempfile, shutil

BIN = "./minivec"

def run(name, sql):
    d = tempfile.mkdtemp()
    p = subprocess.run([BIN, "--data", d], input=sql.encode(), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    out = p.stdout.decode().strip().splitlines()
    errs = [l for l in out if l.startswith("ERROR")]
    rows = [l for l in out if l and not l.startswith("ERROR") and l.split("\t")[0].isdigit()]
    print("%-28s errors=%d rows=%d %s" % (name, len(errs), len(rows), errs[0][:80] if errs else ""))
    shutil.rmtree(d)

def mk(rows, dim):
    vec = ",".join("0.5" for _ in range(dim))
    body = ",".join("(%d,'[%s]')" % (i, vec) for i in range(rows))
    return "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(%d));\nINSERT INTO t VALUES %s;\nSELECT id FROM t;\n" % (dim, body)

run("2 rows x 2 dim", mk(2, 2))
run("2 rows x 128 dim", mk(2, 128))
run("10 rows x 2 dim", mk(10, 2))
run("10 rows x 128 dim", mk(10, 128))
run("100 rows x 2 dim", mk(100, 2))
run("100 rows x 128 dim", mk(100, 128))
