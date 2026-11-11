#!/usr/bin/env bash
# Reproducible validation entry, invoked by the temporary candidate CI branch.
set -euo pipefail
lscpu
python3 minivec/tools/validate_material.py
make -C minivec -j2 test
g++ -O2 -std=c++17 -fsanitize=undefined -fno-sanitize-recover=all -Iminivec/src minivec/tests/test_distance_simd.cpp -o /tmp/distance-ubsan
/tmp/distance-ubsan
g++ -O2 -std=c++17 -pthread -DMVDB_FLAT_INDEX_MEMORY_BYTES=1 -DMVDB_SPILL_BYTES=1 -Iminivec/src minivec/tests/test_flat_index.cpp minivec/src/db.cpp minivec/src/value.cpp minivec/src/flat_index.cpp -o /tmp/flat-spill
/tmp/flat-spill
MVDB_PROTOCOL=sql python3 minivec/tools/harness.py --binary minivec/minivec run minivec/tests/cases
python3 protocol_check/run.py --command '["./minivec/minivec","--data","{data_dir}"]' --handshake-only --output protocol-results/source-handshake
python3 protocol_check/run.py --command '["./minivec/minivec","--data","{data_dir}"]' --output protocol-results/source-public
python3 minivec/tests/json_protocol.py --binary minivec/minivec
docker build --platform linux/amd64 -t minivec:jsonl .
python3 protocol_check/run.py --image minivec:jsonl --handshake-only --output protocol-results/image-handshake
python3 protocol_check/run.py --image minivec:jsonl --output protocol-results/image-public
docker image inspect minivec:jsonl --format '{{.Id}}'
g++ -O3 -std=c++17 -Iminivec/src minivec/tests/test_distance_simd.cpp -o /tmp/distance-tree-reduce
/tmp/distance-tree-reduce --bench
g++ -O3 -std=c++17 -Iminivec/src minivec/tests/profile_query.cpp minivec/src/db.cpp minivec/src/exec.cpp minivec/src/sql.cpp minivec/src/value.cpp minivec/src/flat_index.cpp -o /tmp/profile-query
/tmp/profile-query
g++ -O3 -std=c++17 -DMVDB_SIMD_DOUBLE_REDUCTION -Iminivec/src minivec/tests/test_distance_simd.cpp -o /tmp/distance-double-reduce
/tmp/distance-double-reduce
/tmp/distance-double-reduce --bench
bash minivec/tools/compare_jsonl.sh
