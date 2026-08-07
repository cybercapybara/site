# Wave 1.1 — nightly benchmarks with a public trend

**Date:** 2026-08-06
**Context:** Wave 1 kickoff. The council deferred "nightly bench gate" from
Wave 0; the owner picked benchmarks as the first Wave-1 sub-project (order:
benchmarks → launch campaign → failover/chaos → forker DX). Measurement venue
decision: GitHub Actions shared runners — publish **trends, not absolute
latency claims** (±10–20% runner noise makes absolutes dishonest and PR gates
flaky). The existing harness (`scripts/bench.sh`, `config/bench/*.json`,
`docs/BENCHMARKS.md`) stays the tool for absolute numbers on user hardware.

## Goal

A nightly workflow that measures the fixed `baseline` preset on master,
appends results to a public trend chart, and opens an alert on real
regressions — without ever blocking a PR.

## Components

### 1. Workflow `.github/workflows/bench-nightly.yml`

- Triggers: cron `20 3 * * *` + `workflow_dispatch`. Master only.
- `timeout-minutes: 45`, concurrency group `bench-nightly` (no parallel runs).
- Steps: checkout → buildx with gha/GHCR `cache-from` (dep layer comes from
  the Wave-0.5 cache — minutes, not a cold vcpkg build) → build `runtime`
  image → `docker compose up` app + Postgres + Redis → `apt install wrk` →
  run measurements → convert → publish via `benchmark-action`.
- Permissions: `contents: write` (gh-pages push; the benchmark-action posts
  regression alerts as a commit comment, which needs no extra permission).

### 2. Measurements (fixed `baseline` preset only)

| Series | How | Direction |
|---|---|---|
| `/healthz` req/s, p50, p99 | wrk 30 s, `--latency` | bigger / smaller / smaller |
| DB path req/s, p50, p99 | wrk 30 s against `/api/jobs`; if the route is auth-gated, mint a JWT via `scripts/make-jwt.sh` and pass `wrk -H "Authorization: Bearer …"` (resolve at implementation step 1; the benchmark must measure the 200 path, not a 401 fast-path) | bigger / smaller / smaller |
| Runtime image size (MB) | `docker images --format` on the runtime stage | smaller |
| Cold start to `/ready` (ms) | container start → first 200 | smaller |
| Idle RSS (MB) | `docker stats --no-stream` after ready + settle | smaller |

The last three are low-noise absolutes usable in launch material; the wrk
series are trend-only. Other presets (`pool*`, `threads8`, `max`) remain
manual-only via `make bench`.

### 3. Conversion & storage

- `scripts/wrk2bench.sh`: parse wrk output (req/s, p50, p99 from `--latency`
  block) → `github-action-benchmark` custom JSON (`customBiggerIsBetter` /
  `customSmallerIsBetter` entries, one per series above).
- `benchmark-action/github-action-benchmark@<pinned-sha>`: data on `gh-pages`
  branch under `/dev/bench`, auto-push, interactive chart.
- Alerts: `alert-threshold: 130%`, `comment-on-alert: true`,
  `fail-on-alert: false` (nightly stays green; the alert commit comment is
  the signal — the pinned action posts a commit comment on regression, it
  cannot open issues).
- One-time setup: enable GitHub Pages for branch `gh-pages` via `gh api`.

### 4. Publication

- README: a `benchmarks` badge/link to the chart page
  (`https://moveeeax.github.io/cpp-rapid-rest-template/dev/bench/`).
- `docs/BENCHMARKS.md`: new "Continuous benchmarks" section — methodology,
  chart link, explicit caveat: shared-runner **trends**, not absolute claims;
  measure absolutes on your own hardware with `make bench`.

## Error handling

- Compose or readiness failure → run fails loudly (it's nightly; a red run is
  the alert). wrk parse failure → converter exits non-zero, same effect.
- Runner noise → absorbed by the 130% threshold and trend averaging; a single
  noisy point that trips the threshold produces one alert commit comment,
  with nothing to close (no auto-close in scope).

## Verification

- Two `workflow_dispatch` runs: chart page shows two data points for every
  series; no alert commit comment posted.
- Alert threshold logic checked offline against the first run's JSON (feed a
  +40% mutated copy through the converter and the action's dry logic).
- CI job total stays under ~20 min with warm caches.

## Non-goals

PR benchmark gate, absolute marketing numbers from CI, additional presets in
CI, opening or closing GitHub issues on regression, publishing charts
anywhere but gh-pages.

## Delivery

One branch `ci/wave1-bench-nightly` carrying this spec + implementation;
normal PR to master, verified by two dispatch runs before the cron ever
fires. Plain conventional commits, no AI-attribution trailers.
