#!/usr/bin/env bash
# Reproducible validation entry, invoked by the temporary candidate CI branch.
set -euo pipefail
lscpu
python3 minivec/tools/validate_material.py
make -C minivec -j2 test
g++ -O2 -std=c++17 -fsanitize=undefined -fno-sanitize-recover=all -Iminivec/src minivec/tests/test_distance_simd.cpp -o /tmp/distance-ubsan
/tmp/distance-ubsan
g++ -O2 -std=c++17 -fsanitize=undefined -fno-sanitize-recover=all -Iminivec/src minivec/tests/test_quantized_bound.cpp -o /tmp/quantized-ubsan
/tmp/quantized-ubsan
g++ -O2 -std=c++17 -pthread -DMVDB_FLAT_INDEX_MEMORY_BYTES=1 -DMVDB_SPILL_BYTES=1 -Iminivec/src minivec/tests/test_flat_index.cpp minivec/src/db.cpp minivec/src/value.cpp minivec/src/flat_index.cpp -o /tmp/flat-spill
/tmp/flat-spill
g++ -O2 -std=c++17 -pthread -DMVDB_DISABLE_NORM_ORDER -Iminivec/src minivec/tests/test_flat_index.cpp minivec/src/db.cpp minivec/src/value.cpp minivec/src/flat_index.cpp -o /tmp/flat-original-order
/tmp/flat-original-order
MVDB_PROTOCOL=sql python3 minivec/tools/harness.py --binary minivec/minivec run minivec/tests/cases
python3 protocol_check/run.py --command '["./minivec/minivec","--data","{data_dir}"]' --handshake-only --output protocol-results/source-handshake
python3 protocol_check/run.py --command '["./minivec/minivec","--data","{data_dir}"]' --output protocol-results/source-public
python3 minivec/tests/json_protocol.py --binary minivec/minivec
docker build --platform linux/amd64 -t minivec:jsonl .
docker run --rm --network none --mount "type=bind,src=$PWD,dst=/project,readonly" --workdir /tmp gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818 sh -c 'g++ -O3 -std=c++17 -I/project/minivec/src /project/minivec/tests/test_sql.cpp /project/minivec/src/sql.cpp /project/minivec/src/value.cpp -o /tmp/test-sql && /tmp/test-sql && g++ -O3 -std=c++17 -I/project/minivec/src /project/minivec/tests/test_quantized_bound.cpp -o /tmp/test-quantized && /tmp/test-quantized'
python3 protocol_check/run.py --image minivec:jsonl --handshake-only --output protocol-results/image-handshake
python3 protocol_check/run.py --image minivec:jsonl --output protocol-results/image-public
docker image inspect minivec:jsonl --format '{{.Id}}'
image_container=$(docker create minivec:jsonl)
docker cp "$image_container:/minivec" /tmp/minivec-image-binary
docker rm "$image_container"
python3 minivec/tests/topk_bounds.py --binary /tmp/minivec-image-binary
python3 minivec/tests/json_protocol.py --binary /tmp/minivec-image-binary
g++ -O3 -std=c++17 -Iminivec/src minivec/tests/test_distance_simd.cpp -o /tmp/distance-tree-reduce
/tmp/distance-tree-reduce --bench
g++ -O3 -std=c++17 -Iminivec/src minivec/tests/profile_query.cpp minivec/src/db.cpp minivec/src/exec.cpp minivec/src/sql.cpp minivec/src/value.cpp minivec/src/flat_index.cpp -o /tmp/profile-query
/tmp/profile-query
g++ -O3 -std=c++17 -DMVDB_SIMD_DOUBLE_REDUCTION -Iminivec/src minivec/tests/test_distance_simd.cpp -o /tmp/distance-double-reduce
/tmp/distance-double-reduce
/tmp/distance-double-reduce --bench
bash minivec/tools/compare_jsonl.sh
python3 minivec/tools/profile_jsonl.py --image minivec:jsonl-previous --output protocol-results/path-profile-previous.json
python3 minivec/tools/profile_jsonl.py --image minivec:jsonl --output protocol-results/path-profile-candidate.json
