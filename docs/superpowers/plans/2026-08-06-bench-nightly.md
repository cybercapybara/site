# Nightly Benchmarks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A nightly GitHub Actions workflow that measures the `baseline` preset on master, publishes a public trend chart on gh-pages, and opens an alert issue on >30% regressions — never blocking a PR.

**Architecture:** Thin workflow, logic in two shell scripts: `wrk2bench.sh` (parse wrk output → github-action-benchmark custom JSON) and `bench-ci.sh` (orchestrate: wrk via the existing `scripts/bench.sh` harness, plus image size / cold start / idle RSS; emit `bench-bigger.json` + `bench-smaller.json`). Two `benchmark-action/github-action-benchmark` steps (biggerIsBetter for throughput, smallerIsBetter for latency+footprint) push series to `gh-pages:/dev/bench`.

**Tech Stack:** bash + awk + jq, wrk, docker compose (v1/v2 both supported by existing `compose()` detection pattern), `benchmark-action/github-action-benchmark`, GitHub Pages.

**Spec:** `docs/superpowers/specs/2026-08-06-bench-nightly-design.md`

## Global Constraints

- Branch: `ci/wave1-bench-nightly` (already exists, carries the spec commit). All work lands there; one PR to master.
- Plain conventional commit messages. NO AI-attribution trailers (`Co-Authored-By: Claude`, `Generated with`, …) — repo owner's standing rule.
- Fixed preset `baseline` only in CI; other presets stay manual.
- The DB-path benchmark MUST measure a 200 response (`curl -sf` guard before wrk), with `JOBS_ENABLED=true` exported (defaults to false; compose forwards it: `docker/docker-compose.yml` app env block).
- Actual jobs route is `/api/v1/jobs` — the `/api/jobs` default in `bench.sh`/docs is a stale 404 path and gets fixed in Task 5.
- `alert-threshold: 130%`, `comment-on-alert: true`, `fail-on-alert: false`.
- Action SHAs are pinned (repo convention — see any workflow); benchmark-action SHA resolved in Task 3.
- Migrations apply automatically on app start (`database.migrations_enabled` defaults true, `src/core/Core.hpp:341`) — no manual migrate step.

---

### Task 1: wrk output → benchmark JSON converter

**Files:**
- Create: `scripts/wrk2bench.sh` (mode `0755`)
- Test fixture: `/tmp/wrk-fixture.txt` (throwaway, not committed)

**Interfaces:**
- Produces: `wrk2bench.sh throughput <label> <wrk-output-file>` → prints a JSON array `[{"name":"<label> req/s","unit":"req/s","value":<float>}]`; `wrk2bench.sh latency <label> <file>` → `[{"name":"<label> p50","unit":"ms","value":<float>},{"name":"<label> p99","unit":"ms","value":<float>}]`. Latencies normalized to ms (wrk prints `us`/`ms`/`s`/`m`). Exits 1 with a message if any of req/s, p50, p99 is missing. Task 2 consumes exactly these two invocations.

- [ ] **Step 1: Write the fixture (the "failing test" input)**

```bash
cat > /tmp/wrk-fixture.txt <<'EOF'
Running 30s test @ http://localhost:8080/healthz
  4 threads and 200 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.91ms    2.34ms   50.11ms   90.60%
    Req/Sec    31.15k     2.14k    40.31k    71.00%
  Latency Distribution
     50%    1.20ms
     75%    2.10ms
     90%    3.55ms
     99%   11.83ms
  3723587 requests in 30.03s, 0.92GB read
Requests/sec: 123978.34
Transfer/sec:     31.34MB
EOF
./scripts/wrk2bench.sh throughput healthz /tmp/wrk-fixture.txt
```

Expected now: `no such file or directory` (script doesn't exist yet).

- [ ] **Step 2: Write the converter**

```bash
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
```

Save as `scripts/wrk2bench.sh`, then `chmod 755 scripts/wrk2bench.sh`.

- [ ] **Step 3: Verify against the fixture**

```bash
./scripts/wrk2bench.sh throughput healthz /tmp/wrk-fixture.txt
./scripts/wrk2bench.sh latency healthz /tmp/wrk-fixture.txt
./scripts/wrk2bench.sh latency healthz /tmp/wrk-fixture.txt | jq .
printf 'Requests/sec: 10\n     50%%    850.00us\n     99%%    1.05s\n' > /tmp/wrk-units.txt
./scripts/wrk2bench.sh latency units /tmp/wrk-units.txt
echo "no distribution" > /tmp/wrk-broken.txt
./scripts/wrk2bench.sh latency broken /tmp/wrk-broken.txt; echo "exit=$?"
```

Expected, line by line:
1. `[{"name":"healthz req/s","unit":"req/s","value":123978.34}]`
2. `[{"name":"healthz p50","unit":"ms","value":1.200},{"name":"healthz p99","unit":"ms","value":11.830}]`
3. jq parses without error (valid JSON).
4. `[{"name":"units p50","unit":"ms","value":0.850},{"name":"units p99","unit":"ms","value":1050.000}]`
5. ERROR line and `exit=1`.

- [ ] **Step 4: Commit**

```bash
git add scripts/wrk2bench.sh
git commit -m "feat(bench): add wrk-to-benchmark-json converter"
```

---

### Task 2: CI benchmark orchestrator

**Files:**
- Create: `scripts/bench-ci.sh` (mode `0755`)

**Interfaces:**
- Consumes: `scripts/bench.sh baseline <endpoint>` (existing harness; brings up postgres+redis, restarts app with `config/bench/baseline.json`, runs wrk 30 s with `--latency`); `scripts/wrk2bench.sh` from Task 1 (exact invocations listed there).
- Produces: `bench-out/bench-bigger.json` (throughput series) and `bench-out/bench-smaller.json` (p50/p99 for both endpoints + `runtime image size` MB + `cold start to /ready` ms + `idle RSS` MB) — the two files Task 3's workflow feeds to github-action-benchmark. Env contract: `BENCH_IMAGE` (default `cpp-rapid-rest-template:bench`) must name an existing local runtime image; `JOBS_ENABLED=true` and `APP_IMAGE=$BENCH_IMAGE` are exported inside.

- [ ] **Step 1: Write the orchestrator**

```bash
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

# 1) wrk runs via the existing harness. bench.sh restarts the app with the
#    baseline config and waits for /ready itself.
./scripts/bench.sh baseline /healthz | tee "$OUT_DIR/wrk-healthz.txt"

# The trend must measure the 200 path, not a 401/404 fast-path.
if ! curl -sf "$APP_URL/api/v1/jobs" >/dev/null; then
    echo "ERROR: GET /api/v1/jobs is not returning 200 — check JOBS_ENABLED propagation" >&2
    exit 1
fi
./scripts/bench.sh baseline /api/v1/jobs | tee "$OUT_DIR/wrk-jobs.txt"

# 2) Runtime image size (bytes → MB).
image_mb=$(docker image inspect "$BENCH_IMAGE" --format '{{.Size}}' \
    | awk '{printf "%.1f", $1/1048576}')

# 3) Cold start: restart the app container, time to first 200 on /ready.
#    python3 for millisecond timestamps — BSD date has no %N.
compose stop app >/dev/null 2>&1
t0=$(python3 -c 'import time; print(int(time.time()*1000))')
compose up -d app >/dev/null 2>&1
until curl -sf "$APP_URL/ready" >/dev/null 2>&1; do sleep 0.05; done
t1=$(python3 -c 'import time; print(int(time.time()*1000))')
cold_ms=$((t1 - t0))

# 4) Idle RSS after a short settle.
sleep 5
rss_mb=$(docker stats --no-stream --format '{{.MemUsage}}' cpp_api_app \
    | awk -F'[ /]+' '{v=$1
        if (v ~ /GiB$/) { sub(/GiB$/, "", v); v *= 1024 } else sub(/MiB$/, "", v)
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
```

Save as `scripts/bench-ci.sh`, `chmod 755 scripts/bench-ci.sh`.

- [ ] **Step 2: Local dry run**

```bash
command -v wrk >/dev/null || brew install wrk
command -v jq  >/dev/null || brew install jq
docker build --target runtime -f docker/Dockerfile -t cpp-rapid-rest-template:bench .
WRK_DURATION=5s ./scripts/bench-ci.sh
jq -e 'length == 2' bench-out/bench-bigger.json
jq -e 'length == 7' bench-out/bench-smaller.json
```

Expected: script completes; final `jq -e` checks pass (2 throughput entries; 4 latency + 3 footprint = 7 smaller-is-better entries); all `value` fields are positive numbers. `WRK_DURATION=5s` keeps the dry run short — CI uses the 30 s default.

- [ ] **Step 3: Clean up local stack and commit**

```bash
make down
git add scripts/bench-ci.sh
git commit -m "feat(bench): add CI benchmark orchestrator"
```

---

### Task 3: nightly workflow

**Files:**
- Create: `.github/workflows/bench-nightly.yml`

**Interfaces:**
- Consumes: `scripts/bench-ci.sh` env contract from Task 2 (`BENCH_IMAGE`, output files under `bench-out/`).
- Produces: benchmark series named `Throughput` and `Latency & footprint` in `gh-pages:/dev/bench` — the names Task 5's docs link to.

- [ ] **Step 1: Collect the pinned action SHAs**

```bash
grep -h -A1 'uses: actions/checkout@\|uses: docker/setup-buildx-action@\|uses: docker/build-push-action@' .github/workflows/ci.yml | head -6
gh api repos/benchmark-action/github-action-benchmark/commits/v1 --jq .sha
```

Use the checkout/buildx/build-push SHAs exactly as ci.yml pins them; use the returned commit SHA for benchmark-action (comment it `# v1`).

- [ ] **Step 2: Write the workflow** (replace `<sha-*>` with Step 1 values)

```yaml
# Nightly benchmark trend on master. Shared-runner numbers are NOISY — this
# workflow tracks trends and alerts on >30% regressions; it never gates PRs.
# Absolute numbers: run `make bench` on your own hardware (docs/BENCHMARKS.md).
name: bench-nightly

on:
  schedule:
    - cron: '20 3 * * *'
  workflow_dispatch:

permissions:
  contents: write   # push data points to gh-pages
  issues: write     # alert issue on regression

concurrency:
  group: bench-nightly
  cancel-in-progress: false

jobs:
  bench:
    runs-on: ubuntu-latest
    timeout-minutes: 45
    steps:
      - uses: actions/checkout@<sha-checkout>  # v4

      - uses: docker/setup-buildx-action@<sha-buildx>  # v3.12.0

      - name: Build runtime image (dep layer from cache)
        uses: docker/build-push-action@<sha-build-push>  # v5.4.0
        with:
          context: .
          file: docker/Dockerfile
          target: runtime
          load: true
          tags: cpp-rapid-rest-template:bench
          cache-from: |
            type=gha
            type=registry,ref=ghcr.io/${{ github.repository }}/builder:cache

      - name: Install wrk and jq
        run: sudo apt-get update && sudo apt-get install -y wrk jq

      - name: Run benchmarks
        env:
          BENCH_IMAGE: cpp-rapid-rest-template:bench
        run: ./scripts/bench-ci.sh

      - name: Publish throughput trend
        uses: benchmark-action/github-action-benchmark@<sha-benchmark-action>  # v1
        with:
          name: Throughput
          tool: customBiggerIsBetter
          output-file-path: bench-out/bench-bigger.json
          github-token: ${{ secrets.GITHUB_TOKEN }}
          auto-push: true
          alert-threshold: '130%'
          comment-on-alert: true
          fail-on-alert: false

      - name: Publish latency and footprint trend
        uses: benchmark-action/github-action-benchmark@<sha-benchmark-action>  # v1
        with:
          name: Latency & footprint
          tool: customSmallerIsBetter
          output-file-path: bench-out/bench-smaller.json
          github-token: ${{ secrets.GITHUB_TOKEN }}
          auto-push: true
          alert-threshold: '130%'
          comment-on-alert: true
          fail-on-alert: false

      - name: Tear down compose stack
        if: always()
        run: make down || true
```

- [ ] **Step 3: Validate and commit**

```bash
python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/bench-nightly.yml')); print('yaml ok')"
git add .github/workflows/bench-nightly.yml
git commit -m "ci: add nightly benchmark workflow with public trend and regression alerts"
```

Expected: `yaml ok`.

---

### Task 4: gh-pages branch + GitHub Pages

**Files:** none in the working tree (orphan branch + repo settings).

**Interfaces:**
- Produces: existing `gh-pages` branch (benchmark-action pushes into it) and Pages serving it — chart lands at `https://moveeeax.github.io/cpp-rapid-rest-template/dev/bench/`, the URL Task 5 links.

- [ ] **Step 1: Create the orphan branch**

```bash
git stash --include-untracked || true
git switch --orphan gh-pages
git commit --allow-empty -m "chore: init gh-pages for benchmark data"
git push -u origin gh-pages
git switch ci/wave1-bench-nightly
git stash pop || true
```

- [ ] **Step 2: Enable Pages on gh-pages**

```bash
gh api -X POST repos/moveeeax/cpp-rapid-rest-template/pages \
  -f 'source[branch]=gh-pages' -f 'source[path]=/' \
  || gh api -X PUT repos/moveeeax/cpp-rapid-rest-template/pages \
  -f 'source[branch]=gh-pages' -f 'source[path]=/'
gh api repos/moveeeax/cpp-rapid-rest-template/pages --jq '{status, source}'
```

Expected: final query shows `"branch": "gh-pages"`. (POST for first-time enable; PUT if Pages was somehow already configured.)

---

### Task 5: documentation + stale endpoint fix

**Files:**
- Modify: `scripts/bench.sh` (usage comment line ~6, default-endpoint comment lines ~43-50, `case` line ~53)
- Modify: `Makefile` (bench target help strings, lines ~466 and ~469: `E=/api/jobs`)
- Modify: `docs/BENCHMARKS.md` (every `/api/jobs` mention; new section)
- Modify: `README.md` (badge row)

**Interfaces:**
- Consumes: chart URL and series names from Tasks 3–4.

- [ ] **Step 1: Fix the stale `/api/jobs` default** — in `scripts/bench.sh` replace both comment mentions and the case arm:

```bash
case "$preset" in
    pool20 | pool50 | max) default_endpoint="/api/v1/jobs" ;;
esac
```

In `Makefile`, both help strings become `E=/api/v1/jobs`. In `docs/BENCHMARKS.md`, replace every `/api/jobs` with `/api/v1/jobs`. Then verify no stale mention survives anywhere tracked:

```bash
git grep -n '/api/jobs' -- ':!docs/superpowers' ; echo "exit=$?"
```

Expected: no output, `exit=1`.

- [ ] **Step 2: Add the "Continuous benchmarks" section to `docs/BENCHMARKS.md`** (after the "Harness" section):

```markdown
## Continuous benchmarks

A nightly workflow (`.github/workflows/bench-nightly.yml`) runs the `baseline`
preset on master — wrk against `/healthz` and `/api/v1/jobs` — plus three
low-noise footprint metrics (runtime image size, cold start to `/ready`, idle
RSS), and appends every point to a public trend:

**<https://moveeeax.github.io/cpp-rapid-rest-template/dev/bench/>**

Read it as a **trend**, not as absolute numbers: GitHub shared runners are
noisy (±10–20% between nights is normal). A regression above 30% against the
previous point opens an alert issue automatically; no PR is ever blocked by
benchmarks. For absolute numbers on your hardware, run `make bench` as
described above.
```

- [ ] **Step 3: Add the README badge** — in the badge row at the top of `README.md`, after the existing CI badge, add:

```markdown
[![benchmarks](https://img.shields.io/badge/benchmarks-trend-blue)](https://moveeeax.github.io/cpp-rapid-rest-template/dev/bench/)
```

(Match the exact style/separator of the neighboring badges when editing.)

- [ ] **Step 4: Commit**

```bash
git add scripts/bench.sh Makefile docs/BENCHMARKS.md README.md
git commit -m "docs(bench): document the nightly trend, fix stale /api/jobs endpoint"
```

---

### Task 6: PR, merge, live verification

**Files:** none (process task).

- [ ] **Step 1: Push and open the PR**

```bash
git push -u origin ci/wave1-bench-nightly
gh pr create --base master \
  --title "ci: nightly benchmark trend with regression alerts (Wave 1.1)" \
  --body "Nightly cron runs the baseline preset (wrk on /healthz and /api/v1/jobs + image size / cold start / idle RSS) and publishes the series to gh-pages via github-action-benchmark. >30% regression opens an alert issue; nothing gates PRs. Spec: docs/superpowers/specs/2026-08-06-bench-nightly-design.md. Also fixes the stale /api/jobs default (actual route is /api/v1/jobs — the old default benchmarked a 404)."
```

- [ ] **Step 2: Wait for CI green, merge**

```bash
gh pr checks --watch   # or the poll loop; gh run watch is flaky on this setup
gh pr merge --squash --delete-branch
```

- [ ] **Step 3: Two dispatch runs**

```bash
gh workflow run bench-nightly
# wait for completion (poll gh run list --workflow bench-nightly.yml)
gh workflow run bench-nightly
# wait again
```

Expected: both runs green, each under ~20 min with warm caches.

- [ ] **Step 4: Verify the trend and alert math**

```bash
git fetch origin gh-pages
git show origin/gh-pages:dev/bench/data.js | head -30
curl -sf https://moveeeax.github.io/cpp-rapid-rest-template/dev/bench/ >/dev/null && echo "chart page 200"
gh issue list --state open
```

Expected: `data.js` contains two entries per series (9 series total: 2 throughput + 4 latency + 3 footprint); chart page returns 200 (Pages deploy can lag a few minutes); no alert issue open. Then check the alert math offline: for every series compute run2/run1 ratio from `data.js` and confirm each is below 1.30 (matching "no alert fired"); if any ratio exceeds 1.30, an alert issue SHOULD exist — verify consistency rather than absence.

- [ ] **Step 5: Close the loop**

Update the memory file (Wave 1.1 shipped, chart URL, any surprises) and report: chart URL, first-night numbers, whether alert threshold looks sane after two points.
