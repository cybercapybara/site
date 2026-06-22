#!/usr/bin/env bash
#
# One-time bootstrap: rename the template to your project identity.
#
# Usage:
#   ./scripts/init-project.sh <project-name> [registry]
#
# Example:
#   ./scripts/init-project.sh my-service docker.io/myorg
#
# Arguments:
#   project-name  Kebab-case project name (e.g. my-service)
#   registry      Container registry (default: docker.io/library)
#
# What it does:
#   1. Replaces all hardcoded template names across the codebase
#   2. Renames Helm chart directories
#   3. Updates project.env
#
set -euo pipefail

DRY_RUN=0
FORCE=0
ARGS=()
for arg in "$@"; do
    case "$arg" in
    --dry-run | -n) DRY_RUN=1 ;;
    --force | -f) FORCE=1 ;;
    --help | -h)
        cat <<USAGE
Usage: $0 [--dry-run] [--force] <project-name> [registry]

  --dry-run, -n   Print every file that would be touched and the patterns that
                  would run, without modifying anything on disk.
  --force, -f     Run even if project.env shows the template has already been
                  initialised under a different name. Without this flag the
                  script asks for confirmation interactively.

Example:
  $0 my-service docker.io/myorg
  $0 --dry-run my-service docker.io/myorg
USAGE
        exit 0
        ;;
    *) ARGS+=("$arg") ;;
    esac
done

if [[ ${#ARGS[@]} -lt 1 ]]; then
    echo "Usage: $0 [--dry-run] <project-name> [registry]"
    echo "Example: $0 my-service docker.io/myorg"
    exit 1
fi

PROJECT_NAME="${ARGS[0]}"

# Registry / namespace for the published images. Take it as the 2nd arg, OR —
# so a forker can't silently inherit the template author's namespace (the
# hardcoded resert/... in CI, compose, release.yml) — ASK for it interactively
# when omitted. Falls back to the default only non-interactively (CI / dry-run).
if [[ ${#ARGS[@]} -ge 2 ]]; then
    REGISTRY="${ARGS[1]}"
elif [[ $DRY_RUN -eq 0 && -t 0 ]]; then
    printf 'Container registry / namespace for images (e.g. docker.io/myorg, ghcr.io/me) [docker.io/library]: '
    read -r REGISTRY
    REGISTRY="${REGISTRY:-docker.io/library}"
else
    REGISTRY="docker.io/library"
fi

# Derive snake_case from kebab-case: my-service -> my_service
PROJECT_SNAKE="${PROJECT_NAME//-/_}"

if [[ $DRY_RUN -eq 1 ]]; then
    echo "==> DRY RUN — no files will be modified"
fi
echo "==> Bootstrapping project"
echo "    PROJECT_NAME  = ${PROJECT_NAME}"
echo "    PROJECT_SNAKE = ${PROJECT_SNAKE}"
echo "    REGISTRY      = ${REGISTRY}"
echo ""

# ── Collect target files ────────────────────────────────────────
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# ── Idempotency guard ───────────────────────────────────────────
# project.env is rewritten on success, so its current state tells us whether
# the template has already been re-named. If it has (and to something other
# than the requested name), running again would either be a no-op (same name)
# or would mangle the already-customised tree (different name). Stop and ask.
if [[ -f project.env ]]; then
    EXISTING_NAME="$(awk -F= '/^PROJECT_NAME=/{print $2}' project.env | tr -d '[:space:]')"
    # Default ships as PROJECT_NAME=cpp-api; treat that as "untouched template".
    if [[ -n "$EXISTING_NAME" && "$EXISTING_NAME" != "cpp-api" && "$EXISTING_NAME" != "cpp-rapid-rest-template" ]]; then
        if [[ "$EXISTING_NAME" == "$PROJECT_NAME" ]]; then
            echo "==> Project already initialised as '${EXISTING_NAME}' — nothing to do."
            exit 0
        fi
        if [[ $FORCE -eq 0 && $DRY_RUN -eq 0 ]]; then
            cat >&2 <<EOF
==> project.env shows this repo is already initialised as '${EXISTING_NAME}'.
    Re-running with a different name will not undo the previous rename and
    is almost never what you want.

    Pass --force to proceed anyway, or --dry-run to preview the next pass.
EOF
            exit 4
        fi
        if [[ $FORCE -eq 1 ]]; then
            echo "==> --force: continuing despite existing PROJECT_NAME='${EXISTING_NAME}'"
        fi
    fi
fi

# macOS ships bash 3.2 which lacks `mapfile`; use a portable while-read loop
# with a NUL-separated find to survive paths with spaces.
FILES=()
while IFS= read -r -d '' f; do
    FILES+=("$f")
done < <(
    find . -type f \
        \( -name '*.json' -o -name '*.yml' -o -name '*.yaml' \
        -o -name '*.tpl' -o -name 'CMakeLists.txt' \
        -o -name 'Dockerfile' -o -name 'Makefile' \
        -o -name '*.sh' -o -name '*.hpp' -o -name '*.cpp' \
        -o -name '*.md' -o -name '*.conf' -o -name '*.env' \
        -o -name '.gitignore' -o -name '.gitlab-ci.yml' \
        -o -name 'Chart.yaml' \) \
        -not -path './.git/*' \
        -not -path './build/*' \
        -not -path './vcpkg*' \
        -not -path './frontend/node_modules/*' \
        -not -path './frontend/dist/*' \
        -not -path './_reference/*' \
        -not -path './docs/html/*' \
        -print0
)

echo "==> Found ${#FILES[@]} files to process"

# ── Replacements (order matters: specific → general) ────────────
# Each sed runs in-place across all target files.
# Patterns are ordered from most specific to most general to
# prevent partial matches.

declare -a PATTERNS=(
    # 1. Inconsistent helm image ref (if present)
    "s|resert/cpp-rapid-rest-app|${REGISTRY}/${PROJECT_NAME}|g"
    # 2. CI image name
    "s|resert/cpp-rapid-rest-template|${REGISTRY}/${PROJECT_NAME}|g"
    # 3. Repo-level references
    "s|cpp-rapid-rest-template|${PROJECT_NAME}|g"
    # 4. CMake project name, binary names, Dockerfile
    "s|cpp_api_template|${PROJECT_SNAKE}|g"
    # 5. Bench service name
    "s|cpp_api_bench|${PROJECT_SNAKE}_bench|g"
    # 6. OTel service name
    "s|cpp_api_service|${PROJECT_SNAKE}_service|g"
    # 7. Worker OTel service name
    "s|cpp_worker_service|${PROJECT_SNAKE}_worker_service|g"
    # 8. Helm maintainer team
    "s|cpp-api-team|${PROJECT_NAME}-team|g"
    # 9. Helm worker chart name
    "s|cpp-worker|${PROJECT_NAME}-worker|g"
    # 10. Helm chart, K8s, ingress
    "s|cpp-api|${PROJECT_NAME}|g"
    # 11. Worker logger name
    "s|cpp_worker|${PROJECT_SNAKE}_worker|g"
    # 12. Logger, containers, general snake_case
    "s|cpp_api|${PROJECT_SNAKE}|g"
    # 13. Kafka client ID
    "s|cpp_producer|${PROJECT_SNAKE}_producer|g"
)

# GNU sed accepts `-i`, BSD/macOS sed requires `-i ''` (an explicit backup
# suffix argument). Detect once and pick the right invocation.
if sed --version >/dev/null 2>&1; then
    SED_INPLACE=(sed -i)
else
    SED_INPLACE=(sed -i '')
fi

if [[ $DRY_RUN -eq 1 ]]; then
    echo "==> Patterns that would run:"
    for pattern in "${PATTERNS[@]}"; do
        echo "    sed -i \"$pattern\" <files>"
    done
    echo ""
    echo "==> Files that would be touched (matches at least one pattern):"
    for f in "${FILES[@]}"; do
        if grep -qE 'cpp-rapid-rest-template|cpp_api_template|cpp-api|cpp_api|cpp-worker|cpp_worker|cpp_producer|cpp_api_bench|cpp_api_service|cpp_worker_service|cpp-api-team|resert/cpp-rapid-rest-template|resert/cpp-rapid-rest-app' "$f" 2>/dev/null; then
            echo "    $f"
        fi
    done
    echo ""
    if [[ -d "helm/cpp-api" && "cpp-api" != "${PROJECT_NAME}" ]]; then
        echo "==> Would rename helm/cpp-api -> helm/${PROJECT_NAME}"
    fi
    if [[ -d "helm/cpp-worker" && "cpp-worker" != "${PROJECT_NAME}-worker" ]]; then
        echo "==> Would rename helm/cpp-worker -> helm/${PROJECT_NAME}-worker"
    fi
    echo "==> Would write project.env (PROJECT_NAME=${PROJECT_NAME}, REGISTRY=${REGISTRY})"
    echo ""
    echo "DRY RUN complete. Re-run without --dry-run to apply."
    exit 0
fi

for pattern in "${PATTERNS[@]}"; do
    "${SED_INPLACE[@]}" "$pattern" "${FILES[@]}"
done

echo "==> Text replacements complete"

# ── Rename Helm chart directories ───────────────────────────────
if [[ -d "helm/cpp-api" && "cpp-api" != "${PROJECT_NAME}" ]]; then
    mv "helm/cpp-api" "helm/${PROJECT_NAME}"
    echo "==> Renamed helm/cpp-api -> helm/${PROJECT_NAME}"
fi

if [[ -d "helm/cpp-worker" && "cpp-worker" != "${PROJECT_NAME}-worker" ]]; then
    mv "helm/cpp-worker" "helm/${PROJECT_NAME}-worker"
    echo "==> Renamed helm/cpp-worker -> helm/${PROJECT_NAME}-worker"
fi

# ── Update project.env ──────────────────────────────────────────
cat >project.env <<EOF
PROJECT_NAME=${PROJECT_NAME}
REGISTRY=${REGISTRY}
EOF
echo "==> Updated project.env"

echo ""
echo "Done! Review changes with:"
echo "  git diff"
echo "  grep -r 'cpp_api\|cpp-api' . --include='*.hpp' --include='*.cpp' --include='*.yml' --include='*.yaml' --include='*.json' --include='Makefile' --include='Dockerfile' --include='CMakeLists.txt' | grep -v '.git/'"
