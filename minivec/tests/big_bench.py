import random, subprocess, tempfile, shutil, time, sys, os, resource

BIN = "./minivec"
N = int(sys.argv[1]) if len(sys.argv) > 1 else 100000
BATCH = int(sys.argv[2]) if len(sys.argv) > 2 else 100
DIM = 128

random.seed(7)
data = tempfile.mkdtemp()
parts = ["CREATE TABLE big (id BIGINT PRIMARY KEY, v VECTOR(%d));" % DIM, "BEGIN;"]
for i in range(0, N, BATCH):
    rows = ",".join(
        "(%d,'[%s]')" % (j, ",".join("%.3f" % random.random() for _ in range(DIM)))
        for j in range(i, min(i + BATCH, N))
    )
    parts.append("INSERT INTO big VALUES %s;" % rows)
parts.append("COMMIT;")
q = ",".join("0.5" for _ in range(DIM))
parts.append("SELECT id, L2_DISTANCE(v, '[%s]') AS d FROM big ORDER BY d LIMIT 3;" % q)
sql = "\n".join(parts) + "\n"

t0 = time.time()
p = subprocess.run([BIN, "--data", data], input=sql.encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
elapsed = time.time() - t0
out = p.stdout.decode().strip().splitlines()
errors = [l for l in out if l.startswith("ERROR")]
rss_kb = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
print("rows=%d batch=%d elapsed=%.2fs peak_rss=%.1f MB errors=%d" % (N, BATCH, elapsed, rss_kb / 1024.0, len(errors)))
for e in errors[:3]:
    print("  ", e[:120])
print("  tail:", " | ".join(out[-4:]))
print("  data dir:", subprocess.run(["du", "-sh", data], stdout=subprocess.PIPE).stdout.decode().split()[0])
shutil.rmtree(data)
