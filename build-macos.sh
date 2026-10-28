#!/usr/bin/env bash
set -euo pipefail

for argument in "$@"; do
    case "$argument" in
        --release|--sanitize|--no-gui) ;;
        -h|--help)
            printf 'Usage: %s [--release] [--sanitize] [--no-gui]\n' "$0"
            printf '%s\n' 'Requires Xcode Command Line Tools. Builds the engine, the game modules, the tests, the player and the editor (Sokol Metal).'
            exit 0 ;;
        *) printf 'Unknown option: %s\n' "$argument" >&2; exit 2 ;;
    esac
done

if [ "$(uname -s)" != Darwin ]; then
    echo 'This script requires macOS. Use build.sh on Linux or build.ps1 on Windows.' >&2
    exit 1
fi
if ! command -v xcrun >/dev/null 2>&1; then
    echo 'Xcode Command Line Tools are required. Install them with: xcode-select --install' >&2
    exit 1
fi
if ! mac_clang=$(xcrun --sdk macosx --find clang) ||
   ! mac_ar=$(xcrun --sdk macosx --find ar) ||
   ! mac_sdk=$(xcrun --sdk macosx --show-sdk-path); then
    echo 'Could not locate the macOS toolchain and SDK. Check xcode-select -p.' >&2
    exit 1
fi
if [ ! -x "$mac_clang" ] || [ ! -x "$mac_ar" ] || [ ! -d "$mac_sdk" ]; then
    echo 'The selected macOS toolchain or SDK is missing.' >&2
    exit 1
fi

export PATH="$(dirname "$mac_clang"):$(dirname "$mac_ar"):$PATH"
export SDKROOT="$mac_sdk"
script_root=$(cd "$(dirname "$0")" && pwd)
exec /bin/bash "$script_root/build.sh" --compiler clang "$@"
