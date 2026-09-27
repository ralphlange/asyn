#!/bin/sh
#
# Runs the testParallelCallback IOC once per callback worker thread count and
# prints the resulting table. Each run is a fresh IOC because
# callbackParallelThreads() only has an effect before iocInit().
#
# Usage: ./runBenchmark.sh [nchan [rounds [threads ...]]]

set -e

NCHAN=${1:-500}
ROUNDS=${2:-200}
shift 2 2>/dev/null || true
THREADS=${*:-"1 2 4 8"}

IOC=../../bin/${EPICS_HOST_ARCH:?set EPICS_HOST_ARCH}/testParallelCallback
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

printf 'channels=%s rounds=%s\n\n' "$NCHAN" "$ROUNDS"
printf '%9s | %28s | %28s\n' "" "asyn I/O Intr records" "plain callbacks (control)"
printf '%9s | %8s %8s %10s | %8s %8s %10s\n' \
       "cb thr" push settle round push settle round
printf '%9s-|-%8s-%8s-%10s-|-%8s-%8s-%10s\n' \
       --------- -------- -------- ---------- -------- -------- ----------

for n in $THREADS; do
    NCHAN=$NCHAN ROUNDS=$ROUNDS NTHREADS=$n \
        $IOC st.cmd < /dev/null > "$OUT/run.$n" 2>&1 || {
            echo "run with $n threads failed:"; cat "$OUT/run.$n"; exit 1; }
    # Only the output after the "=== ... ===" banner is the reported run,
    # everything before it is the warm up.
    sed -n '/^=== /,$p' "$OUT/run.$n" | awk -v n="$n" '
        function num(  i) { for (i = 1; i <= NF; i++)
                                if ($i == "mean") return $(i+1)
                            return 0 }
        /:bench:/           { sec = "a" }
        /:queueBench:/      { sec = "c" }
        /mean/ && /^ *push|^ *post/   { p[sec] = num() }
        /mean/ && /^ *settle/         { s[sec] = num() }
        /mean/ && /^ *round/          { r[sec] = num() }
        END { printf "%9s | %8.1f %8.1f %10.1f | %8.1f %8.1f %10.1f\n",
                     n, p["a"], s["a"], r["a"], p["c"], s["c"], r["c"] }'
done

echo
echo "All times are microseconds per round (one entry per channel per round)."

echo
echo "Full IOC output of the last run:"
sed -n '/^=== /,$p' "$OUT/run.$n"
