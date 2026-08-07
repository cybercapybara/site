#!/usr/bin/env bash
# Convert `wrk --latency` output into github-action-benchmark custom JSON.
#   wrk2bench.sh throughput <label> <file>  → [{"<label> req/s", req/s}]
#   wrk2bench.sh latency    <label> <file>  → [{"<label> p50", ms}, {"<label> p99", ms}]
# Latency values are normalized to milliseconds (wrk emits us/ms/s/m).
set -euo pipefail

mode="${1:?usage: wrk2bench.sh throughput|latency <label> <wrk-output-file>}"
label="${2:?missing label}"
file="${3:?missing wrk output file}"

rps=$(awk '$1=="Requests/sec:"{print $2}' "$file")
p50=$(awk '$1=="50%"{v=$2} END{print v}' "$file")
p99=$(awk '$1=="99%"{v=$2} END{print v}' "$file")
if [ -z "$rps" ] || [ -z "$p50" ] || [ -z "$p99" ]; then
    echo "ERROR: could not parse Requests/sec + latency distribution from $file (did wrk run with --latency?)" >&2
    exit 1
fi

# Order matters: check 'us' and 'ms' before the bare 's' / 'm' suffixes.
to_ms() {
    awk -v v="$1" 'BEGIN{
        if      (v ~ /us$/) { sub(/us$/, "", v); printf "%.3f", v/1000 }
        else if (v ~ /ms$/) { sub(/ms$/, "", v); printf "%.3f", v+0 }
        else if (v ~ /m$/)  { sub(/m$/,  "", v); printf "%.3f", v*60000 }
        else if (v ~ /s$/)  { sub(/s$/,  "", v); printf "%.3f", v*1000 }
        else                { printf "%.3f", v+0 }
    }'
}

case "$mode" in
    throughput)
        printf '[{"name":"%s req/s","unit":"req/s","value":%s}]\n' "$label" "$rps"
        ;;
    latency)
        printf '[{"name":"%s p50","unit":"ms","value":%s},{"name":"%s p99","unit":"ms","value":%s}]\n' \
            "$label" "$(to_ms "$p50")" "$label" "$(to_ms "$p99")"
        ;;
    *)
        echo "usage: wrk2bench.sh throughput|latency <label> <wrk-output-file>" >&2
        exit 2
        ;;
esac
