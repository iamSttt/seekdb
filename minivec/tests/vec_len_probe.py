import subprocess, tempfile, os, shutil, sys

BIN = "./minivec"
dims = [4, 8, 16, 32, 64, 96, 128]
for d in dims:
    data = tempfile.mkdtemp()
    vec = ",".join(["0.5"] * d)
    sql = "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(%d));\nINSERT INTO t VALUES (1,'[%s]');\nSELECT id FROM t;\n" % (d, vec)
    p = subprocess.run([BIN, "--data", data], input=sql.encode(), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    out = p.stdout.decode().strip().splitlines()
    ok = any(line.strip() == "1" for line in out)
    err = [l for l in out if l.startswith("ERROR")]
    print("dim=%3d ok=%s %s" % (d, ok, err[0][:80] if err else ""))
    shutil.rmtree(data)
