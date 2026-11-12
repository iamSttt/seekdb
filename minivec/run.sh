#!/usr/bin/env bash
# run.sh -- local case runner: build if needed, then drive the case.
#
#   ./run.sh tests/cases/01_basic_crud.sql     # compare against .result
#   ./run.sh tests/cases                        # every case in the directory
#   ./run.sh tests/cases/05_filtered_topk.sql --allow-errors --dump /tmp/out
#   ./run.sh --record tests/cases/01_basic_crud.sql
#
# The build step is skipped when ./minivec is newer than every source file.
set -euo pipefail

cd "$(dirname "$0")"

usage() {
  cat >&2 <<'EOF'
usage: ./run.sh [--record] <case.sql | case-directory> [harness options]

  --record           run the case and (re)write its .result file
  --no-build         never run make
  --build            force make before running
EOF
  exit 2
}

MODE=run
BUILD=auto
ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --record)   MODE=record; shift ;;
    --no-build) BUILD=never; shift ;;
    --build)    BUILD=force; shift ;;
    -h|--help)  usage ;;
    --)         shift; ARGS+=("$@"); break ;;
    -*)         ARGS+=("$1"); shift ;;
    *)          ARGS+=("$1"); shift ;;
  esac
done

if [ ${#ARGS[@]} -eq 0 ]; then
  usage
fi

need_build=0
case "$BUILD" in
  force) need_build=1 ;;
  never) need_build=0 ;;
  *)
    if [ ! -x ./minivec ]; then
      need_build=1
    else
      while IFS= read -r src; do
        if [ "$src" -nt ./minivec ]; then need_build=1; break; fi
      done < <(find src third_party -name '*.cpp' -o -name '*.h' -o -name '*.hpp' 2>/dev/null)
    fi
    ;;
esac

if [ "$need_build" = 1 ]; then
  echo "[run.sh] building minivec ..." >&2
  make -j"${JOBS:-$(nproc 2>/dev/null || echo 4)}"
fi

if [ ! -x ./minivec ]; then
  echo "[run.sh] ./minivec is missing -- build failed?" >&2
  exit 3
fi

exec env MVDB_PROTOCOL=sql python3 tools/harness.py "$MODE" "${ARGS[@]}"
