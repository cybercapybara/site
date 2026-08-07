#!/usr/bin/env bash
# CI benchmark orchestrator: fixed `baseline` preset against the middleware
# path (/healthz) and the DB path (/api/v1/jobs), plus three low-noise
# footprint metrics. Emits the two JSON files github-action-benchmark reads.
# Local dry run (needs wrk + jq + a built runtime image):
#   docker build --target runtime -f docker/Dockerfile -t cpp-rapid-rest-template:bench .
#   ./scripts/bench-ci.sh
set -euo pipefail

OUT_DIR="${OUT_DIR:-bench-out}"
BENCH_IMAGE="${BENCH_IMAGE:-cpp-rapid-rest-template:bench}"
APP_URL="${APP_URL:-http://localhost:8080}"
mkdir -p "$OUT_DIR"

# The jobs subsystem is off by default (jobs.enabled=false); the DB-path
# benchmark needs it. Compose forwards JOBS_ENABLED into the app container.
export JOBS_ENABLED=true
export APP_IMAGE="$BENCH_IMAGE"

compose() {
    if command -v docker-compose >/dev/null 2>&1; then
        docker-compose -f docker/docker-compose.yml --env-file docker/.env "$@"
    else
        docker compose -f docker/docker-compose.yml --env-file docker/.env "$@"
    fi
}

# A wrk run full of 500s (or any non-2xx/3xx) still exits 0 — wrk doesn't
# treat response status as a failure. Fail the run loudly instead of
# publishing a "successful" trend point full of errors.
check_wrk_errors() {
    local file="$1" endpoint="$2"
    if grep -q 'Non-2xx or 3xx responses' "$file"; then
        echo "ERROR: $endpoint returned non-2xx/3xx responses during the wrk run:" >&2
        grep 'Non-2xx or 3xx responses' "$file" >&2
        exit 1
    fi
}

# The app container runs as uid 1000 (appuser) and bind-mounts ../logs from
# the checkout; on Linux CI runners the checkout belongs to a different uid,
# so without this the app dies on "logs/app.log: Permission denied".
# Docker Desktop masks the mismatch locally, which is why dry runs pass.
mkdir -p logs && chmod a+w logs

# 1) wrk runs via the existing harness. bench.sh restarts the app with the
#    baseline config and waits for /ready itself.
./scripts/bench.sh baseline /healthz | tee "$OUT_DIR/wrk-healthz.txt"
check_wrk_errors "$OUT_DIR/wrk-healthz.txt" /healthz

# The trend must measure the 200 path, not a 401/404 fast-path.
if ! curl -sf "$APP_URL/api/v1/jobs" >/dev/null; then
    echo "ERROR: GET /api/v1/jobs is not returning 200 — check JOBS_ENABLED propagation" >&2
    exit 1
fi
./scripts/bench.sh baseline /api/v1/jobs | tee "$OUT_DIR/wrk-jobs.txt"
check_wrk_errors "$OUT_DIR/wrk-jobs.txt" /api/v1/jobs

# 2) Runtime image size (bytes → MB).
image_mb=$(docker image inspect "$BENCH_IMAGE" --format '{{.Size}}' \
    | awk '{printf "%.1f", $1/1048576}')

# 3) Cold start: restart the app container, time to first 200 on /ready.
#    python3 for millisecond timestamps — BSD date has no %N.
#    bench.sh only exports CONFIG_FILE inside its own process, so the
#    recreated container here needs its own export to pick up the baseline
#    preset (otherwise cold start / idle RSS are measured under whatever
#    config the image defaults to).
export CONFIG_FILE=config/bench/baseline.json
compose stop app >/dev/null
t0=$(python3 -c 'import time; print(int(time.time()*1000))')
compose up -d app >/dev/null
ready=0
for _ in $(seq 1 600); do
    if curl -sf "$APP_URL/ready" >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 0.05
done
if [ "$ready" -ne 1 ]; then
    echo "ERROR: app did not become ready within ~30s of cold start" >&2
    compose logs --tail 10 app
    exit 1
fi
t1=$(python3 -c 'import time; print(int(time.time()*1000))')
cold_ms=$((t1 - t0))

# 4) Idle RSS after a short settle.
sleep 5
rss_mb=$(docker stats --no-stream --format '{{.MemUsage}}' cpp_api_app \
    | awk -F'[ /]+' '{v=$1
        if (v ~ /GiB$/) { sub(/GiB$/, "", v); v *= 1024 }
        else if (v ~ /KiB$/) { sub(/KiB$/, "", v); v /= 1024 }
        else sub(/MiB$/, "", v)
        printf "%.1f", v}')

# 5) Assemble the two benchmark-action inputs.
{
    ./scripts/wrk2bench.sh throughput healthz "$OUT_DIR/wrk-healthz.txt"
    ./scripts/wrk2bench.sh throughput jobs    "$OUT_DIR/wrk-jobs.txt"
} | jq -s 'add' > "$OUT_DIR/bench-bigger.json"

{
    ./scripts/wrk2bench.sh latency healthz "$OUT_DIR/wrk-healthz.txt"
    ./scripts/wrk2bench.sh latency jobs    "$OUT_DIR/wrk-jobs.txt"
    printf '[{"name":"runtime image size","unit":"MB","value":%s},{"name":"cold start to /ready","unit":"ms","value":%s},{"name":"idle RSS","unit":"MB","value":%s}]\n' \
        "$image_mb" "$cold_ms" "$rss_mb"
} | jq -s 'add' > "$OUT_DIR/bench-smaller.json"

echo "==> bench-bigger.json / bench-smaller.json:"
jq . "$OUT_DIR/bench-bigger.json" "$OUT_DIR/bench-smaller.json"
