#!/usr/bin/env bash
# 3D-terrain baseline (PR #4190, pinned on branch terrain-3d).
#
# One entry point for CI (.github/workflows/terrain-ci.yml) and local runs:
#   1. configure the build directory if it has no build.ninja yet,
#   2. build mbgl-test-runner and mbgl-render-test-runner,
#   3. run the terrain gtest filter,
#   4. run the render + query suites filtered to terrain/,
#   5. exit non-zero on any failure that is not named in
#      metrics/ignores/terrain-known.json.
#
# Usage: scripts/terrain-ci.sh [metal|opengl|vulkan]
#   default backend: metal on macOS, opengl on Linux.
#
# Environment:
#   BUILD_DIR          build directory (default build-macos-metal / build-linux-opengl / build-linux-vulkan)
#   CMAKE_CONFIGURE_ARGS  extra args for the configure step (only used when configuring)
#   JOBS               parallel build jobs (default: CPU count)
#   SKIP_BUILD=1       use the existing binaries as-is
#   STRICT_KNOWN=1     also fail when a known failure now passes (stale entry)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# GeoJSONTile.* holds the tile-level test of the feature-state revision (Task 33).
# StillImageOffline.* and the Offline* storage tests cover still images offline (Task 35); they need
# no test server. RenderTargetDrapeKey.* holds the drape zoom key per load mode.
GTEST_FILTER='*Terrain*:*DEM*:*TileLod*:*TileCover*:GeoJSONTile.*:StillImageOffline.*:OnlineFileSource.Offline*:MainResourceLoader.Offline*:RenderTargetDrapeKey.*'
RENDER_FILTER='terrain/'
KNOWN="metrics/ignores/terrain-known.json"

case "$(uname -s)" in
  Darwin) default_backend=metal ;;
  *) default_backend=opengl ;;
esac
BACKEND="${1:-$default_backend}"

case "$BACKEND" in
  metal)
    PRESET=macos-metal
    MANIFEST=metrics/macos-metal-terrain.json
    : "${BUILD_DIR:=build-macos-metal}"
    WRAP=()
    ;;
  opengl)
    PRESET=linux-opengl
    MANIFEST=metrics/linux-opengl-terrain.json
    : "${BUILD_DIR:=build-linux-opengl}"
    if [[ -z "${DISPLAY:-}" ]] && command -v xvfb-run >/dev/null; then WRAP=(xvfb-run -a); else WRAP=(); fi
    ;;
  vulkan)
    PRESET=linux-vulkan
    MANIFEST=metrics/linux-vulkan-terrain.json
    : "${BUILD_DIR:=build-linux-vulkan}"
    # Same driver pinning as linux-ci.yml: lavapipe only (Mesa's gfxstream ICD leaks a pthread key
    # per vkCreateInstance and exhausts them in the render tests).
    : "${VK_DRIVER_FILES:=/usr/share/vulkan/icd.d/lvp_icd.json}"
    export VK_DRIVER_FILES
    if [[ -z "${DISPLAY:-}" ]] && command -v xvfb-run >/dev/null; then WRAP=(xvfb-run -a); else WRAP=(); fi
    ;;
  *) echo "unknown backend: $BACKEND (expected metal|opengl|vulkan)" >&2; exit 64 ;;
esac

JOBS="${JOBS:-$( (sysctl -n hw.ncpu 2>/dev/null || nproc) )}"
METRIC_DIR="metrics/$(basename "$MANIFEST" .json)"
REPORT_HTML="metrics/$(basename "$MANIFEST" .json).html"

echo "== terrain-ci: backend=$BACKEND preset=$PRESET build=$BUILD_DIR commit=$(git rev-parse --short HEAD)"

# --- configure + build -------------------------------------------------------
if [[ "${SKIP_BUILD:-0}" != 1 ]]; then
  if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    echo "== configure: cmake --preset $PRESET ${CMAKE_CONFIGURE_ARGS:-}"
    # shellcheck disable=SC2086
    cmake --preset "$PRESET" -B "$BUILD_DIR" ${CMAKE_CONFIGURE_ARGS:-}
  fi
  echo "== build"
  cmake --build "$BUILD_DIR" --target mbgl-test-runner mbgl-render-test-runner -j "$JOBS"
fi

status=0

# --- gtest ------------------------------------------------------------------
echo "== gtest --gtest_filter=$GTEST_FILTER"
if ! "${WRAP[@]+"${WRAP[@]}"}" "$BUILD_DIR/mbgl-test-runner" --gtest_filter="$GTEST_FILTER"; then
  echo "!! gtest: failures (none are allowed; the known list covers render/query tests only)"
  status=1
fi

# --- known list sanity: every entry must name an existing test ---------------
python3 - "$KNOWN" <<'EOF' || status=1
import json, os, sys
known = json.load(open(sys.argv[1]))
missing = [k for k in known if not os.path.isdir(os.path.join("metrics/integration", k))]
for k in missing:
    print(f"!! known list names a test that does not exist: {k}")
sys.exit(1 if missing else 0)
EOF

# --- render + query tests ---------------------------------------------------
mkdir -p "$METRIC_DIR"
echo "== render/query tests: --manifestPath $MANIFEST --filter $RENDER_FILTER"
render_rc=0
"${WRAP[@]+"${WRAP[@]}"}" "$BUILD_DIR/mbgl-render-test-runner" \
  --manifestPath "$MANIFEST" --filter "$RENDER_FILTER" || render_rc=$?

# The runner exits 0 when only ignored tests fail. Summarise by status from its
# HTML report so known failures are listed by name in every run.
python3 - "$REPORT_HTML" "$KNOWN" "${STRICT_KNOWN:-0}" <<'EOF' || status=1
import json, re, sys
html, known_path, strict = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
known = json.load(open(known_path))
results = re.findall(r'<h2><span class="label"[^>]*>([^<]+)</span> (\S+)</h2>', open(html).read())
by = {}
for st, tid in results:
    by.setdefault(st, []).append(tid)
print(f"== render/query summary: {len(results)} run")
for st in sorted(by):
    print(f"   {st}: {len(by[st])}")
for tid in sorted(by.get("ignored failed", [])):
    print(f"   known failure: {tid} -- {known.get(tid, '(ignored by a platform list)')}")
rc = 0
for st in ("failed", "errored"):
    for tid in sorted(by.get(st, [])):
        print(f"!! NEW {st}: {tid}")
        rc = 1
for tid in sorted(by.get("ignored passed", [])):
    if tid in known:
        print(f"!! known failure now passes, remove it from the known list: {tid}")
        rc = rc or (1 if strict else 0)
if not results:
    print("!! no terrain tests ran")
    rc = 1
sys.exit(rc)
EOF
if [[ $render_rc -ne 0 ]]; then
  echo "!! render-test-runner exit code $render_rc (report: $REPORT_HTML)"
  status=1
fi

if [[ $status -eq 0 ]]; then echo "== terrain-ci: GREEN"; else echo "== terrain-ci: RED"; fi
exit $status
