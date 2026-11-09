import subprocess, tempfile, shutil

BIN = "./minivec"

def run(name, sql):
    d = tempfile.mkdtemp()
    p = subprocess.run([BIN, "--data", d], input=sql.encode(), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    out = p.stdout.decode().strip().splitlines()
    errs = [l for l in out if l.startswith("ERROR")]
    ok = [l for l in out if l.strip().isdigit()]
    print("%-30s bytes=%-7d errors=%d inserted=%d %s" % (name, len(sql), len(errs), len(ok), errs[0][:70] if errs else ""))
    shutil.rmtree(d)

def mk(rows, dim, prec="%.3f", txn=False):
    vec = ",".join(prec % (0.111) for _ in range(dim))
    body = ",".join("(%d,'[%s]')" % (i, vec) for i in range(rows))
    head = "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(%d));\n" % dim
    if txn:
        head += "BEGIN;\n"
    tail = "COMMIT;\n" if txn else ""
    return head + "INSERT INTO t VALUES %s;\n" % body + tail + "SELECT id FROM t;\n"

for rows in (10, 50, 100, 150, 200, 400):
    run("no-txn %d rows x128" % rows, mk(rows, 128))
run("txn 100 rows x128", mk(100, 128, txn=True))
