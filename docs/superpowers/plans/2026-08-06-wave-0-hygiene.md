# Wave 0 — Repo Hygiene & Agent-Readiness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Execute the six verified, low-risk hygiene fixes from the Wave 0 spec (`docs/superpowers/specs/2026-08-06-wave-0-hygiene-design.md`): GitHub metadata + releases, CLAUDE.md + doc-drift fix, Renovate risk-inversion fix, CI GHCR cache + TSAN job, coverage ratchet.

**Architecture:** Task 1 is pure `gh` API work (no commits). Tasks 2–4 are independent small PRs off `master`, each verified by existing CI. Task 5 measures coverage in Docker first, then lands a PR. No new scheduled workflows, no new subsystems.

**Tech Stack:** gh CLI, GitHub Actions, Renovate, CMake, Docker/compose, gcovr.

## Global Constraints

- Plain conventional commit messages. **No AI-attribution trailers** (user's global rule).
- Every claim about repo state below was verified on 2026-08-06; re-verify with the given commands before editing.
- No new scheduled workflows in this wave.
- Branch protection on `master` does **not** exist (verified: `gh api .../branches/master/protection` → 404). Renovate automerge therefore gates on all reported checks, not required ones — do not enable stricter protection in this wave without the owner.
- PR bodies: no generated-with footer.

---

### Task 1: GitHub metadata + publish releases (no commits)

**Files:** none (remote repo settings only).

**Interfaces:**
- Consumes: existing tags `v1.0.0`…`v1.4.0`, `CHANGELOG.md` sections `## [X.Y.Z] — DATE`.
- Produces: repo metadata + 9 published releases; later tasks don't depend on it.

- [ ] **Step 1: Set description, homepage, topics, template flag, discussions**

```bash
gh repo edit moveeeax/cpp-rapid-rest-template \
  --description "Production-ready C++20 REST service template: Drogon + PostgreSQL + Redis, JWT/RBAC, jobs+DLQ, OpenTelemetry, Helm, React SPA — fork it and ship endpoints, not middleware" \
  --homepage "https://app.demo.tarassov.me" \
  --template \
  --enable-discussions \
  --add-topic cpp --add-topic cpp20 --add-topic drogon --add-topic rest-api \
  --add-topic vcpkg --add-topic postgresql --add-topic redis --add-topic docker \
  --add-topic kubernetes --add-topic helm --add-topic opentelemetry --add-topic template
```

- [ ] **Step 2: Verify metadata**

Run: `gh repo view --json description,homepageUrl,isTemplate,repositoryTopics,hasDiscussionsEnabled`
Expected: all fields populated, `isTemplate: true`.

- [ ] **Step 3: Publish a release per tag from CHANGELOG (ascending, so v1.4.0 ends up "latest")**

```bash
cd "$(git rev-parse --show-toplevel)"
SCRATCH=<scratchpad-dir>   # session scratchpad
for tag in $(git tag -l 'v*' | sort -V); do
  ver=${tag#v}
  awk -v ver="$ver" '
    $0 ~ "^## \\[" ver "\\]" {flag=1; next}
    flag && /^## \[/ {exit}
    flag {print}
  ' CHANGELOG.md | sed -e '1{/^$/d}' > "$SCRATCH/notes-$tag.md"
  [ -s "$SCRATCH/notes-$tag.md" ] || { echo "EMPTY notes for $tag — STOP"; break; }
  gh release create "$tag" --verify-tag --title "$tag" --notes-file "$SCRATCH/notes-$tag.md"
done
```

- [ ] **Step 4: Verify releases**

Run: `gh release list --limit 20`
Expected: 9 releases, `v1.4.0` marked `Latest`.

- [ ] **Step 5: Note manual follow-up for the owner**

Social-preview image can't be set via API — include "upload a social preview in Settings → General" in the final report.

---

### Task 2: PR-1 — CLAUDE.md + doc-drift fix (+ commit spec & plan)

**Files:**
- Create: `CLAUDE.md`
- Modify: `docs/INDEX.md` (row referencing deleted `.gitlab-ci.yml`), `.github/workflows/release.yml` (two stale `.gitlab-ci.yml` comment mentions, lines ~16 and ~32)
- Add: `docs/superpowers/specs/2026-08-06-wave-0-hygiene-design.md`, `docs/superpowers/plans/2026-08-06-wave-0-hygiene.md` (already written locally)

**Interfaces:**
- Produces: root `CLAUDE.md` that Tasks 3–5's PRs (and all future agent work) follow.

- [ ] **Step 1: Branch**

```bash
git checkout -b chore/wave0-claude-md master
```

- [ ] **Step 2: Write `CLAUDE.md`** with exactly this content:

```markdown
# CLAUDE.md — agent guide for this repo

C++20 REST service template: Drogon + PostgreSQL + Redis, vcpkg/CMake,
React SPA in `frontend/`, Helm charts in `helm/`. `docs/INDEX.md` is the
map of all documentation; `docs/CONVENTIONS.md` is the pattern reference.

## Prime directive: scaffold, don't hand-roll

- Full CRUD resource: `./scripts/new-resource.sh Name` — migration + DTO +
  repository + controller + registry row + OpenAPI block + integration test.
- Single endpoint: `./scripts/new-endpoint.sh FooController Get /api/v1/foo
  [--with-test] [--patch-openapi]`
- Background job: `./scripts/new-job.sh <type>`
- Migration: `make new-migration SLUG=<slug>`
- React page: `./scripts/new-react-page.sh`

The generators encode the invariants below — their output passes the CI
gates by construction. Hand-rolled versions usually don't.

## Invariants the CI gates enforce

1. **Route triple-sync:** every `ADD_METHOD_TO` in a controller must also
   appear in `Api::get_endpoints()` (`src/api/Endpoints.hpp`) **and** in
   `docs/openapi.yaml`. `scripts/check-openapi-drift.sh` and
   `scripts/check-routes-registered.sh` fail CI on any mismatch.
2. **API versioning (ADR 0006):** business routes live under `/api/v1`;
   `new-endpoint.sh` rejects unversioned paths. Probe routes (`/healthz`,
   `/ready`, `/health`, `/metrics`) stay unversioned.
3. **Header-only src/ (ADR 0003):** implementation lives in `.hpp`; don't
   add `.cpp` files except the existing binary entry points.
4. **One error shape:** `{error, status, message, ...}` everywhere — use
   `ErrorResponse::*` / `Api::Validation::*`, never hand-rolled error JSON.
5. **Test buckets by directory** (`scripts/check-test-buckets.sh`):
   `tests/unit` (no services), `tests/integration` (real Postgres/Redis),
   `tests/api` (controller via `TestHelpers::make_request`), `tests/e2e`
   (real HTTP server, separate binary).
6. **Migrations:** `migrations/NNN_slug.sql`, sequential numbering, no
   `BEGIN`/`COMMIT` (the runner wraps them; use the
   `-- migrate:no-transaction` marker for `CREATE INDEX CONCURRENTLY`).
7. **No secrets in tracked files:** `config/config.json` holds `${VAR}`
   placeholders, env overrides everything (`docs/CONFIG.md` is the full
   table). gitleaks gates CI; `make prod-check` gates the prod profile.
8. **Commits:** conventional commits, no AI-attribution trailers.

## Gate sequence — run cheapest-first before pushing

1. `make fmt` — clang-format in place
2. `./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh
   && ./scripts/check-test-buckets.sh` — seconds, no build
3. `make lint-openapi` — spectral over `docs/openapi.yaml`
4. `make test-quick` — cached test image, ~5 s
5. `make test` — full rebuild + suite, ~2 min; what CI runs
6. `make helm-lint` — only if `helm/` was touched
7. `make ci-local` — full local reproduction of CI

CI additionally runs clang-tidy, ASan+UBSan (+TSAN), gitleaks, Trivy,
helm-render and the OpenAPI-drift gate.

## Don'ts

- Don't edit the `builtin-baseline` in `vcpkg.json` or `ARG VCPKG_REF` in
  `docker/Dockerfile` by hand — Renovate owns them, and a baseline bump
  rebuilds the entire dependency world.
- Don't weaken `config/config.production.json` — `make prod-check` gates it.
- Don't change the error-response shape without updating `docs/openapi.yaml`.

## Self-maintenance

When a PR adds or changes a CI gate, scaffolding script, or invariant,
update this file in the same PR.
```

- [ ] **Step 3: Fix `docs/INDEX.md`** — delete the whole table row
  `| [\`../.gitlab-ci.yml\`](../.gitlab-ci.yml) | Self-hosted GitLab pipeline (kept for parity) |`
  and drop the "(kept in parity with the canonical GitLab pipeline)" clause from the `ci.yml` row in the same table.

- [ ] **Step 4: Fix `.github/workflows/release.yml` comments** — rewrite the two stale mentions:
  - Line ~16: `helm/*/values-prod.yaml both deploy \`resert/…\` from Docker Hub, and GitLab CI (.gitlab-ci.yml, IMAGE_NAME) publishes there too.` → `helm/*/values-prod.yaml both deploy \`resert/…\` from Docker Hub.`
  - Line ~30-32: `# Single source of truth for the published image namespace; mirrors # .gitlab-ci.yml's IMAGE_NAME.` → `# Single source of truth for the published image namespace.`
  - Also line ~"Build order (mirrors GitLab CI pattern):" → `# Build order:` if present.

- [ ] **Step 5: Verify no stale references and no other gitlab drift**

Run: `grep -rin 'gitlab' docs/ .github/ README.md CONTRIBUTING.md | grep -v 'gitlab16.skiftrade'`
Expected: no hits referring to this repo's own CI (mentions of GitLab-as-product in ADRs/history are fine — inspect each hit).

- [ ] **Step 6: Commit, push, open PR**

```bash
git add CLAUDE.md docs/INDEX.md .github/workflows/release.yml docs/superpowers/
git commit -m "docs: add CLAUDE.md agent guide, drop stale GitLab CI references"
git push -u origin chore/wave0-claude-md
gh pr create --title "docs: add CLAUDE.md agent guide, drop stale GitLab CI references" \
  --body "Adds a root CLAUDE.md (scaffolding-first rule, CI-enforced invariants, cheapest-first gate sequence) and removes doc references to the deleted .gitlab-ci.yml. Also commits the Wave-0 spec and plan. Part of Wave 0 (docs/superpowers/specs/2026-08-06-wave-0-hygiene-design.md)."
```

- [ ] **Step 7: Wait for CI green, merge**

Run: `gh pr checks --watch` then `gh pr merge --squash --delete-branch`
Expected: all checks pass. If a check fails, fix on the branch before merging.

---

### Task 3: PR-2 — Renovate risk-inversion fix

**Files:**
- Modify: `renovate.json`

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: quarantined vcpkg baseline; automerged green minors.

- [ ] **Step 1: Branch**

```bash
git checkout -b chore/wave0-renovate master
```

- [ ] **Step 2: Edit `renovate.json`** — add `"dependencyDashboard": true` at top level (after `"description"`), and replace the `packageRules` array with:

```json
  "packageRules": [
    {
      "description": "Auto-merge patch updates when CI is green",
      "matchUpdateTypes": ["patch", "pin", "digest"],
      "automerge": true
    },
    {
      "description": "Infra images (Postgres/Redis/Kafka) — majors need a human",
      "matchDatasources": ["docker"],
      "matchUpdateTypes": ["major"],
      "automerge": false,
      "labels": ["infra-major"]
    },
    {
      "description": "GitHub Actions minors auto-merge after a week of soak",
      "matchManagers": ["github-actions"],
      "matchUpdateTypes": ["minor"],
      "minimumReleaseAge": "7 days",
      "automerge": true
    },
    {
      "description": "npm devDependencies minors auto-merge after a week of soak",
      "matchManagers": ["npm"],
      "matchDepTypes": ["devDependencies"],
      "matchUpdateTypes": ["minor"],
      "minimumReleaseAge": "7 days",
      "automerge": true
    },
    {
      "description": "vcpkg baseline bump rebuilds the whole dependency world — never automerge (must stay last: later rules win)",
      "matchDepNames": ["microsoft/vcpkg"],
      "automerge": false,
      "labels": ["needs-human"]
    }
  ],
```

The vcpkg rule MUST stay the last element — Renovate applies rules in order and the last match wins, which is what overrides the generic digest-automerge rule above it.

- [ ] **Step 3: Validate config**

Run: `npx --yes --package renovate -- renovate-config-validator renovate.json`
Expected: `Config validated successfully`. Fallback if npx/network fails: `jq . renovate.json` (syntax) + manual schema review — say so in the PR body.

- [ ] **Step 4: Create the `needs-human` label**

Run: `gh label create needs-human --description "Requires a human decision, do not automerge" --color D93F0B`
Expected: label created (or "already exists" — fine).

- [ ] **Step 5: Commit, push, PR, merge**

```bash
git add renovate.json
git commit -m "chore(renovate): quarantine vcpkg baseline from automerge, automerge soaked minors"
git push -u origin chore/wave0-renovate
gh pr create --title "chore(renovate): quarantine vcpkg baseline from automerge, automerge soaked minors" \
  --body "The generic digest automerge rule matched the vcpkg-baseline custom manager (git-refs digest of microsoft/vcpkg) — the riskiest update in the repo auto-merged while harmless minors waited. Quarantines vcpkg behind a needs-human label, automerges GitHub-Actions/npm-dev minors after 7 days soak, enables the dependency dashboard. Part of Wave 0."
gh pr checks --watch && gh pr merge --squash --delete-branch
```

---

### Task 4: PR-3 — CI reads GHCR builder cache + TSAN job

**Files:**
- Modify: `.github/workflows/ci.yml` (three `docker/build-push-action` steps at ~lines 32, 186, 238; new `tsan` job after `sanitizers`)
- Modify: `CMakeLists.txt` (new `ENABLE_TSAN` option after the `ENABLE_SANITIZERS` block at ~line 41-50)
- Modify: `CLAUDE.md` (gate list mentions TSAN — already worded to include it in Task 2)

**Interfaces:**
- Consumes: `ghcr.io/moveeeax/cpp-rapid-rest-template/builder:cache` published by the existing `builder-cache.yml`.
- Produces: `ENABLE_TSAN` CMake option (used by the new CI job only).

- [ ] **Step 1: Branch**

```bash
git checkout -b ci/wave0-ghcr-cache-tsan master
```

- [ ] **Step 2: Add registry cache fallback** — in ALL THREE `docker/build-push-action` steps of `ci.yml` (jobs `build-and-test`, `clang-tidy`, `sanitizers`), replace

```yaml
          cache-from: type=gha
```

with

```yaml
          cache-from: |
            type=gha
            type=registry,ref=ghcr.io/${{ github.repository }}/builder:cache
```

(`cache-to` stays `type=gha,mode=max`. Registry ref is lowercase already — `github.repository` is `moveeeax/cpp-rapid-rest-template`.)

- [ ] **Step 3: Add `ENABLE_TSAN` to `CMakeLists.txt`** immediately after the existing `ENABLE_SANITIZERS` block:

```cmake
# ThreadSanitizer — mutually exclusive with ASan/UBSan (they can't share a
# process). Used by the CI `tsan` job over the unit-test bucket.
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    if(ENABLE_SANITIZERS)
        message(FATAL_ERROR "ENABLE_TSAN and ENABLE_SANITIZERS are mutually exclusive")
    endif()
    message(STATUS "Sanitizer enabled: thread")
    add_compile_options(
        -fsanitize=thread
        -fno-omit-frame-pointer
        -O1
        -g)
    add_link_options(-fsanitize=thread)
endif()
```

- [ ] **Step 4: Add the `tsan` job to `ci.yml`** after the `sanitizers` job, mirroring its structure exactly (same checkout/buildx/build-push SHAs — copy them from the `sanitizers` job):

```yaml
  tsan:
    runs-on: ubuntu-latest
    # ThreadSanitizer over the unit-test bucket. The codebase leans on inline
    # singletons + atomics (ADR 0004) — exactly where races hide. Kept apart
    # from the ASan job: TSan can't share a process with ASan.
    timeout-minutes: 25
    steps:
      - uses: actions/checkout@34e114876b0b11c390a56381ad16ebd13914f8d5 # v4

      - name: Set up Docker Buildx
        uses: docker/setup-buildx-action@8d2750c68a42422c14e847fe6c8ac0403b4cbd6f  # v3.12.0

      - name: Build builder image
        uses: docker/build-push-action@ca052bb54ab0790a636c9b5f226502c73d547a25  # v5.4.0
        with:
          context: .
          file: docker/Dockerfile
          target: builder
          push: false
          load: true
          tags: cpp-api-template:builder
          build-args: ENABLE_WERROR=ON
          cache-from: |
            type=gha
            type=registry,ref=ghcr.io/${{ github.repository }}/builder:cache
          cache-to: type=gha,mode=max

      - name: Build & run unit tests with TSan
        run: |
          docker run --rm \
              --entrypoint /bin/bash \
              cpp-api-template:builder -c "
                  set -e
                  cd /app
                  rm -rf build-tsan
                  cmake -B build-tsan \
                      -G Ninja \
                      -DCMAKE_BUILD_TYPE=Debug \
                      -DENABLE_TSAN=ON \
                      -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
                      -DVCPKG_INSTALLED_DIR=/app/build/vcpkg_installed
                  cmake --build build-tsan -j\$(nproc) --target cpp_api_template_tests_unit
                  TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
                  ./build-tsan/cpp_api_template_tests_unit \
                      --gtest_color=yes
              "
```

- [ ] **Step 5: Lint the workflow locally**

Run: `actionlint .github/workflows/ci.yml` if available, else `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci.yml'))"`
Expected: no errors.

- [ ] **Step 6: Commit, push, PR**

```bash
git add .github/workflows/ci.yml CMakeLists.txt
git commit -m "ci: read GHCR builder cache as fallback, add ThreadSanitizer job"
git push -u origin ci/wave0-ghcr-cache-tsan
gh pr create --title "ci: read GHCR builder cache as fallback, add ThreadSanitizer job" \
  --body "All three builder builds now fall back to the GHCR cache that builder-cache.yml already publishes but nothing read — after a GHA-cache eviction this was 3×~30 min of cold vcpkg. Adds an ENABLE_TSAN CMake option and a tsan CI job over the unit bucket (inline singletons + atomics are exactly where races hide). Part of Wave 0."
```

- [ ] **Step 7: Watch CI; decide on TSAN outcome**

Run: `gh pr checks --watch`
- If `tsan` is green → merge: `gh pr merge --squash --delete-branch`.
- If `tsan` fails with **real data races** (read the log: `gh run view --log-failed`): set `continue-on-error: true` on the `tsan` job with a comment `# ratchet — flip to false once races are fixed (see issue #N)`, open an issue titled "TSAN: data races in unit bucket" containing the race reports, push, wait green, merge. Do NOT silently drop the job.
- If `tsan` fails for infra reasons (image, cmake flags) → fix the infra, don't add continue-on-error.

---

### Task 5: Coverage measurement + PR-4 — ratchet

**Files:**
- Modify: `Makefile` (line 11: `COVERAGE_MIN ?= 40`)
- Modify: `CONTRIBUTING.md` (add ratchet rule sentence)

**Interfaces:**
- Consumes: builder image (local or `make warm-cache`), compose `test-postgres`/`test-redis` (profile `test`, network `app-network`, env `TEST_PG_HOST`/`TEST_REDIS_HOST`).

- [ ] **Step 1: Start test sidecars**

```bash
cd "$(git rev-parse --show-toplevel)"
docker compose -f docker/docker-compose.yml --profile test up -d test-postgres test-redis
docker network ls | grep app-network   # note the exact network name (usually docker_app-network)
```

- [ ] **Step 2: Ensure a builder image exists locally**

Run: `docker image inspect cpp-api-template:builder >/dev/null 2>&1 || make warm-cache`
If warm-cache pulls nothing usable, build it (long, background): `docker build --target builder -t cpp-api-template:builder -f docker/Dockerfile .`

- [ ] **Step 3: Build with coverage + run all three buckets inside the builder container** (run in background; ~10–40 min depending on cache):

```bash
docker run --rm --network docker_app-network \
  -e TEST_PG_HOST=test-postgres -e TEST_REDIS_HOST=test-redis \
  --entrypoint /bin/bash cpp-api-template:builder -c "
    set -e
    cd /app
    pip3 install --quiet gcovr 2>/dev/null || (apt-get update -qq && apt-get install -y -qq python3-pip && pip3 install --quiet gcovr)
    rm -rf build-cov
    cmake -B build-cov -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_INSTALLED_DIR=/app/build/vcpkg_installed
    cmake --build build-cov -j\$(nproc)
    ./build-cov/cpp_api_template_tests_unit || true
    ./build-cov/cpp_api_template_tests_integration || true
    ./build-cov/cpp_api_template_e2e || true
    gcovr -r . --filter 'src/.*' --print-summary
  " 2>&1 | tail -30
```

Record the `lines:` percentage. IMPORTANT: if the integration or e2e binary failed to reach Postgres/Redis (look for connection errors in output), the number understates — fix connectivity before trusting it.

- [ ] **Step 4: Branch + set the ratchet**

New floor = floor(actual − 4). Example: actual 63.2 % → `COVERAGE_MIN ?= 59`.

```bash
git checkout -b chore/wave0-coverage-ratchet master
# Makefile line 11: COVERAGE_MIN ?= <new floor>
```

In `CONTRIBUTING.md`, in the testing section, add:

```markdown
Coverage floor (`COVERAGE_MIN` in the Makefile) is a ratchet: it only goes
up. When real coverage climbs, bump the floor to (actual − 3–5 pp) in the
same PR. Never lower it to make a PR pass.
```

- [ ] **Step 5: Verify the gate passes at the new floor** — re-run the gcovr line from Step 3 inside the container with `--fail-under-line <new floor>` appended; expected exit 0.

- [ ] **Step 6: Commit, push, PR, merge**

```bash
git add Makefile CONTRIBUTING.md
git commit -m "test: raise coverage floor to <N>% and document the ratchet rule"
git push -u origin chore/wave0-coverage-ratchet
gh pr create --title "test: raise coverage floor to <N>%, document ratchet" \
  --body "Measured line coverage over src/ with all three buckets against live sidecars: <actual>%. Floor set to actual−4 per the ratchet rule (floor only goes up), rule documented in CONTRIBUTING. Part of Wave 0."
gh pr checks --watch && gh pr merge --squash --delete-branch
```

- [ ] **Step 7: Cleanup**

```bash
docker compose -f docker/docker-compose.yml --profile test down
```

---

## Execution notes

- Task order: 1 first (instant win), then 2 (CLAUDE.md governs the rest), 3–5 in any order; 5's measurement can run in background from the start.
- Tasks 2–4 each merge before the next branches from `master` **or** branch from `master` immediately — they touch disjoint files, so either way merges cleanly (Task 4 edits `ci.yml` lines that Task 2's `release.yml` edit doesn't touch).
- If any PR's CI reveals a pre-existing failure unrelated to the change, stop and report — don't "fix" unrelated things inside these PRs.
