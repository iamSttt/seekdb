#!/usr/bin/env bash
# Reproducible validation entry, invoked by the temporary candidate CI branch.
set -euo pipefail
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
./minivec/test_distance_simd --bench
bash minivec/tools/compare_jsonl.sh
