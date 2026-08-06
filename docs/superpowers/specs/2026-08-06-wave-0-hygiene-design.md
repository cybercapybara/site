# Wave 0 — repo hygiene & agent-readiness

**Date:** 2026-08-06
**Source:** architecture-council session (5 perspectives + adversarial critique);
this wave is the set of proposals the critique rated *strong*, all small (S),
all low-risk, all executable by agents.

## Goal

Remove the cheap, verified blockers to discoverability, update safety, CI speed
and agent productivity before any feature work. No new subsystems, no new
scheduled workflows.

## Scope — six tasks

### A. GitHub metadata (no code changes; `gh` CLI)

Current state (verified via `gh repo view`): `isTemplate=false`, `topics=null`,
`homepageUrl=""`, Discussions off, description stale, 9 tags pushed
(v1.0.0–v1.4.0) with **zero** published releases, so the README release badge
renders empty.

- Set description to match the README positioning (production-ready C++20 REST
  template; drop the stale "powers the backend of tarassov.me").
- Set homepage to `https://app.demo.tarassov.me`.
- Add topics: `cpp`, `cpp20`, `drogon`, `rest-api`, `vcpkg`, `postgresql`,
  `redis`, `docker`, `kubernetes`, `helm`, `opentelemetry`, `template`.
- Mark the repo as a **template repository**.
- Enable Discussions.
- Publish a GitHub Release for every existing tag, notes extracted from the
  matching `CHANGELOG.md` section; mark v1.4.0 as latest.
- Out of scope (needs a human/browser): social-preview image upload — reported
  back to the owner as a manual follow-up.

Acceptance: `gh repo view --json ...` shows the new values; `gh release list`
shows 9 releases; release badge in README resolves.

### B. `CLAUDE.md` + one-off doc-drift fix

- Add a root `CLAUDE.md` (~100 lines) for coding agents: scaffolding-first rule
  (`new-resource.sh` / `new-endpoint.sh` / `new-job.sh`, not hand-rolling),
  repo invariants (header-only `.hpp`, `ADD_METHOD_TO` ↔ `Endpoints.hpp` ↔
  `docs/openapi.yaml` drift gate, test-bucket layout, migration conventions,
  unified error shape), and the local gate sequence in cheapest-first order.
  Includes a self-maintenance rule: update this file when gates change.
- Fix the already-drifted docs: remove the `.gitlab-ci.yml` row from
  `docs/INDEX.md` (file no longer exists) and rewrite the two stale
  `.gitlab-ci.yml` comment references in `.github/workflows/release.yml`.

Acceptance: `grep -ri gitlab docs/ .github/` returns nothing stale;
CLAUDE.md accurately names only files/gates that exist.

### C. Renovate: fix the risk inversion

Verified bug: the generic `automerge` rule for `patch, pin, digest` also
matches the vcpkg-baseline custom manager (git-refs **digest** of
`microsoft/vcpkg`) — the riskiest update in the repo (rebuilds the whole vcpkg
world) auto-merges, while harmless minors wait for a human.

- Add a package rule: `microsoft/vcpkg` → `automerge: false`, label
  `needs-human` (placed after the generic rule; last match wins).
- Add automerge for **minor** updates of GitHub Actions and npm
  devDependencies with `minimumReleaseAge: 7 days`.
- Enable `dependencyDashboard`.
- One-time check that branch-protection required checks cover the critical CI
  jobs (automerge merges on green *required* checks only).

Acceptance: `renovate-config-validator` (or schema check) passes; rule order
demonstrably quarantines vcpkg.

### D. CI: read the GHCR builder cache + add a TSAN job

- `builder-cache.yml` already publishes an inline cache to
  `ghcr.io/<repo>/builder:cache`; none of the three `docker/build-push-action`
  steps in `ci.yml` read it. Add
  `cache-from: type=registry,ref=ghcr.io/${{ github.repository }}/builder:cache`
  as a fallback after `type=gha` in all three jobs (build-and-test, clang-tidy,
  sanitizers).
- Add a `tsan` job mirroring the existing `sanitizers` job but with
  `-fsanitize=thread` over the unit-test bucket. If CMake lacks a TSAN toggle,
  add `ENABLE_TSAN` alongside the existing `ENABLE_SANITIZERS` option
  (mutually exclusive with ASan).
- Known risk (accepted): TSAN may surface real races in singleton init — those
  become fix-work, not a reason to drop the job. If the first run is red, the
  job lands as `continue-on-error: true` with a tracking issue, mirroring the
  clang-tidy precedent.

Acceptance: PR pipeline green; a run with cold GHA cache demonstrably hits the
registry cache instead of a ~30-min vcpkg rebuild.

### E. Coverage ratchet

- Measure actual line coverage (`make coverage` semantics: coverage preset +
  all three test buckets against live Postgres/Redis, gcovr over `src/`).
  Run happens in Docker (builder image + compose sidecars), not on the mac.
- Set `COVERAGE_MIN` to (actual − 3–5 pp) and document the ratchet rule
  ("floor only goes up") in CONTRIBUTING.md.

Acceptance: new floor committed; `make coverage` passes at the new floor.

## Non-goals (explicitly deferred, per critique)

Nightly bench gate, chaos suite, restore drills, Codespaces prebuild, upstream
sync mechanism, scheduled agent audits — either YAGNI at 0 forks or scheduled-
workflow tax. Revisit in later waves.

## Delivery

Task A is direct `gh` API work (no commits). B–E land as separate small PRs on
short-lived branches off `master`, each verified by the existing CI gates.
Plain conventional commit messages, no AI-attribution trailers.
