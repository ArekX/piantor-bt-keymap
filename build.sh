#!/usr/bin/env bash
#
# Build the piantor firmware locally using the same container ZMK's CI uses
# (zmkfirmware/zmk-build-arm:stable, which ships a native linux/arm64 image).
#
# The ZMK workspace (zmk + zephyr + modules, ~1.5GB) is cached in
# <parent>/.zmk-workspace so only the first run pays the download cost.
# Finished .uf2 files are copied to <parent>/firmware.
#
# Usage:
#   ./build.sh                 # left + right, nice_view (default)
#   ./build.sh --reset         # settings_reset for both halves
#   ./build.sh --all           # everything in build.yaml
#   ./build.sh --pristine      # force a clean rebuild
#   ./build.sh --update        # re-run `west update` (after changing west.yml)
#   ./build.sh --shell         # drop into the container instead of building

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_NAME="$(basename "$REPO_DIR")"
WORKSPACE="$(dirname "$REPO_DIR")"

IMAGE="zmkfirmware/zmk-build-arm:stable"
# Keep in sync with the `revision:` in config/west.yml.
ZMK_TAG="$(sed -n 's/^[[:space:]]*revision:[[:space:]]*\(v[0-9.]*\).*/\1/p' "$REPO_DIR/config/west.yml" | head -1)"
ZMK_TAG="${ZMK_TAG:-v0.3}"

PRISTINE=0
DO_UPDATE=0
SHELL_ONLY=0
SET="default"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --reset)          SET="reset" ;;
        --all)            SET="all" ;;
        -p|--pristine)    PRISTINE=1 ;;
        -u|--update)      DO_UPDATE=1 ;;
        --shell)          SHELL_ONLY=1 ;;
        -h|--help)        sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)                echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# board ; shield ; snippet ; extra cmake args ; artifact name
# Mirrors build.yaml -- keep the two in sync.
TARGETS_DEFAULT="piantor_pro_bt_left;nice_view;studio-rpc-usb-uart;-DCONFIG_ZMK_STUDIO=y;piantor_pro_bt_left
piantor_pro_bt_right;nice_view;studio-rpc-usb-uart;;piantor_pro_bt_right"
TARGETS_RESET="piantor_pro_bt_left;settings_reset;;;piantor_pro_bt_left-settings_reset
piantor_pro_bt_right;settings_reset;;;piantor_pro_bt_right-settings_reset"

case "$SET" in
    default) TARGETS="$TARGETS_DEFAULT" ;;
    reset)   TARGETS="$TARGETS_RESET" ;;
    all)     TARGETS="$TARGETS_DEFAULT
$TARGETS_RESET" ;;
esac

command -v docker >/dev/null || { echo "error: docker is not installed" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "error: docker daemon is not running -- start Docker Desktop" >&2; exit 1; }

if [[ ! -d "$WORKSPACE/.zmk-workspace/zmk" ]]; then
    echo "First run: downloading the ZMK toolchain image and Zephyr tree (~3.5GB, several minutes)."
fi

DOCKER_FLAGS=(--rm -i -v "$WORKSPACE:/workspaces" -w /workspaces)
if [[ -t 0 ]]; then DOCKER_FLAGS+=(-t); fi

if [[ "$SHELL_ONLY" == 1 ]]; then
    exec docker run "${DOCKER_FLAGS[@]}" "$IMAGE" bash
fi

docker run "${DOCKER_FLAGS[@]}" \
    -e REPO_NAME="$REPO_NAME" \
    -e ZMK_TAG="$ZMK_TAG" \
    -e PRISTINE="$PRISTINE" \
    -e DO_UPDATE="$DO_UPDATE" \
    -e TARGETS="$TARGETS" \
    "$IMAGE" bash -s <<'INNER'
set -euo pipefail

WS=/workspaces/.zmk-workspace
# `west init -l zmk/app` makes the *parent of the manifest repo* the workspace
# top level, i.e. $WS/zmk -- not $WS. All west commands must run from there.
ZMK_DIR="$WS/zmk"
CONFIG_DIR="/workspaces/$REPO_NAME/config"
MODULE_DIR="/workspaces/$REPO_NAME"
OUT_DIR=/workspaces/firmware

if [[ ! -d "$ZMK_DIR" ]]; then
    echo "==> Cloning ZMK $ZMK_TAG"
    mkdir -p "$WS"
    git clone --depth 1 --branch "$ZMK_TAG" https://github.com/zmkfirmware/zmk.git "$ZMK_DIR"
    DO_UPDATE=1
fi

cd "$ZMK_DIR"
if [[ ! -d .west ]]; then
    echo "==> west init"
    west init -l app
    DO_UPDATE=1
fi
if [[ "$DO_UPDATE" == 1 || ! -d zephyr ]]; then
    echo "==> west update (this is the slow one)"
    west update
fi
west zephyr-export >/dev/null

mkdir -p "$OUT_DIR"
failed=0

while IFS=';' read -r board shield snippet cmake_args artifact; do
    [[ -z "${board:-}" ]] && continue
    echo
    echo "==> Building $artifact  (board=$board shield=$shield)"

    args=(-s app -d "$WS/build/$artifact" -b "$board")
    [[ "$PRISTINE" == 1 ]] && args+=(-p)
    [[ -n "$snippet" ]] && args+=(-S "$snippet")

    cmake_extra=()
    [[ -n "$shield" ]] && cmake_extra+=("-DSHIELD=$shield")
    cmake_extra+=("-DZMK_CONFIG=$CONFIG_DIR" "-DZMK_EXTRA_MODULES=$MODULE_DIR")
    [[ -n "$cmake_args" ]] && cmake_extra+=($cmake_args)

    if west build "${args[@]}" -- "${cmake_extra[@]}"; then
        cp "$WS/build/$artifact/zephyr/zmk.uf2" "$OUT_DIR/$artifact.uf2"
        echo "==> firmware/$artifact.uf2"
    else
        echo "!!! FAILED: $artifact" >&2
        failed=1
    fi
done <<< "$TARGETS"

echo
if [[ "$failed" == 1 ]]; then
    echo "One or more builds failed." >&2
    exit 1
fi
echo "Done. Firmware in $OUT_DIR:"
ls -1 "$OUT_DIR"
INNER
