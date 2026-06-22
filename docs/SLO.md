# SLOs & alert thresholds

Defines what "healthy" means so the alerts in `docs/prometheus-rules.yml` /
the Helm `PrometheusRule` have a rationale, and on-call has a target rather
than a guess. These are **starting defaults** — set yours from real traffic.

## Service-level objectives (suggested)

| SLO | Target | Measured by |
|---|---|---|
| Availability (non-5xx) | 99.9% / 30d | `1 - rate(http_requests_total{status=~"5.."}) / rate(http_requests_total)` |
| Latency | p99 < 1s | `histogram_quantile(0.99, http_request_duration_seconds)` |
| Job delivery | DLQ drains < 15m | `jobs_dlq_depth` returns to 0 |

Error budget at 99.9% ≈ 43 min/month of full unavailability. The `High5xxRate`
alert fires at 5% (well above budget burn) so it catches incidents, not slow
burn — add a multi-window burn-rate alert if you adopt strict budgeting.

## Alert thresholds → SLO mapping

| Alert | Threshold | Why | Tune when |
|---|---|---|---|
| `High5xxRate` | 5xx > 5% for 5m | Fast incident signal, not budget burn | Lower to 1% once traffic is steady |
| `HighP99Latency` | p99 > 1s for 10m | Matches latency SLO | Set to your real p99 + headroom |
| `ReplicationLagHigh` | lag > 60s for 5m | Stale reads become user-visible | Lower if you serve read-heavy traffic from replicas |
| `DeadLetterQueueGrowing` | DLQ > 0 (or `dlqDepth`) for 15m | Jobs silently failing | Raise `dlqDepth`/`dlqPerTypeDepth` if some failures are expected |
| `RetriesExhaustedSpike` | exhausted retries sustained | Downstream past retry budget | — |
| `*TargetDown` | no scrape 2m | Process down/wedged | — |

Helm thresholds live in `values.yaml → monitoring.thresholds`; the compose
copy is inlined in `docker/prometheus-rules.yml`. Every alert links to
`docs/RUNBOOK.md` via `runbook_url`.

## What's NOT measured yet (gaps to wire when you need them)

- No multi-window burn-rate alerting (single-threshold only).
- No saturation SLI for the DB pool (add `db_pool_in_use / db_pool_size` if you
  expose it).
- Dashboards are provisioned (`docker/grafana/`) but the panel set is minimal —
  build per-SLO panels from the queries above.
