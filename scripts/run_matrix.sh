#!/usr/bin/env bash
# run_matrix.sh - run repro_opaque_fd x combo many times, log everything, write <outdir>/summary.md
# Usage: scripts/run_matrix.sh [-n invocations] [-r runs] [-c combo,combo,...] [-o outdir] [-x "extra args"] [-b builddir]
# Env equivalents: INVOCATIONS, RUNS, COMBOS, OUTDIR, EXTRA_ARGS, BUILD_DIR
# Combos: batch-none inter-none inter-hip inter-vk inter-all
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$here/../build}"; INVOCATIONS="${INVOCATIONS:-20}"; RUNS="${RUNS:-50}"
COMBOS="${COMBOS:-batch-none,inter-none,inter-hip,inter-vk,inter-all}"
EXTRA_ARGS="${EXTRA_ARGS:-}"; OUTDIR="${OUTDIR:-$here/../results/run}"
while getopts "n:r:c:o:x:b:" opt; do
  case $opt in
    n) INVOCATIONS=$OPTARG;; r) RUNS=$OPTARG;; c) COMBOS=$OPTARG;; o) OUTDIR=$OPTARG;;
    x) EXTRA_ARGS=$OPTARG;; b) BUILD_DIR=$OPTARG;;
    *) sed -n '2,4p' "$0"; exit 1;;
  esac
done
mkdir -p "$OUTDIR"
programs=(repro_opaque_fd)
rows=(); envheader=""

for prog in "${programs[@]}"; do
  exe="$(find "$BUILD_DIR" -name "$prog" -type f -perm -u+x 2>/dev/null | head -n1)"
  if [ -z "$exe" ]; then echo "NO EXE for $prog in $BUILD_DIR"; continue; fi
  for combo in ${COMBOS//,/ }; do
    clean=0; mismatched=0; hung=0; importfail=0; bad=0; stale=0; runs=0
    for ((i = 1; i <= INVOCATIONS; i++)); do
      log="$OUTDIR/${prog}_${combo}_${i}.log"
      if [ "$combo" = batch-none ]; then cargs=(--pattern batch --sync none)
      else cargs=(--pattern interleave --sync "${combo#inter-}"); fi
      # shellcheck disable=SC2206
      runargs=(--runs "$RUNS" "${cargs[@]}" $EXTRA_ARGS)
      timeout 120 "$exe" "${runargs[@]}" > "$log" 2>&1
      code=$?
      if [ -z "$envheader" ]; then envheader="$(sed '/^Golden:/,$d' "$log")"; fi
      badnow=$(grep -c 'match=no' "$log")
      bad=$((bad + badnow))
      stale=$((stale + $(grep -c 'stale_by_one=yes' "$log")))
      runs=$((runs + $(grep -c '^run [0-9]*:' "$log")))
      case $code in
        0) clean=$((clean + 1));;
        1) mismatched=$((mismatched + 1));;
        2) importfail=$((importfail + 1));;
        3|124) hung=$((hung + 1));;   # 124 = timeout(1) killed it
      esac
      echo "[$prog $combo $i/$INVOCATIONS] exit=$code bad_runs=$badnow (combo total bad=$bad)"
      grep "match=no" "$log" | sed 's/^/    /'
    done
    echo "$prog combo=$combo invocations=$INVOCATIONS clean=$clean mismatched=$mismatched hung=$hung importfail=$importfail bad_runs=$bad stale_by_one=$stale total_runs=$runs"
    rows+=("| $prog | $combo | $INVOCATIONS | $runs | $bad | $stale | $mismatched | $hung | $importfail |")
  done
done

{
  echo '# Results matrix'; echo
  if [ -n "$envheader" ]; then echo 'Environment:'; echo; echo '```'; echo "$envheader"; echo '```'; echo; fi
  echo '| program | combo | invocations | runs | bad runs | stale_by_one | invocations with mismatch | hangs | import fail |'
  echo '|---|---|---|---|---|---|---|---|---|'
  printf '%s\n' "${rows[@]}"
} > "$OUTDIR/summary.md"
echo 'MATRIX DONE'
