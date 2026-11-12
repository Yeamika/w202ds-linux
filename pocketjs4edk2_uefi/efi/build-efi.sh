#!/bin/bash
# Build the ESP loaders and diagnostics independently of the platform FV.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")" && pwd -P)
project=$(cd -- "$repo/.." && pwd -P)
output= edk2= platform= abi= tools= python= jobs=4
while (($#)); do
  case "$1" in
    --output) output=${2:?}; shift 2;;
    --edk2) edk2=${2:?}; shift 2;;
    --platform) platform=${2:?}; shift 2;;
    --abi-include) abi=${2:?}; shift 2;;
    --tools) tools=${2:?}; shift 2;;
    --python) python=${2:?}; shift 2;;
    --jobs) jobs=${2:?}; shift 2;;
    --help|-h) echo 'Usage: build-efi.sh --output DIR --edk2 DIR --platform DIR --abi-include DIR [--tools DIR] [--python CMD] [--jobs N]'; exit 0;;
    *) echo "Unknown option: $1" >&2; exit 2;;
  esac
done
: "${output:?pass --output}" "${edk2:?pass --edk2}" "${platform:?pass --platform}" "${abi:?pass --abi-include}"
output=$(realpath -m "$output")
edk2=$(realpath "$edk2")
platform=$(realpath "$platform")
abi=$(realpath "$abi")
for source in "$project" "$platform" "$edk2" "$abi"; do
  case "$output/" in "$source/"*) echo 'Output must be outside source and ABI directories' >&2; exit 2;; esac
done
test ! -e "$output" || { echo 'Choose a fresh EFI output directory' >&2; exit 2; }
test -f "$abi/pocketjs_boot_context.h" || { echo 'Missing pocketjs_boot_context.h' >&2; exit 2; }
: "${EDK_TOOLS_PATH:=${tools:-}}"
: "${EDK_TOOLS_PATH:?set EDK_TOOLS_PATH or pass --tools}"
EDK_TOOLS_PATH=$(realpath "$EDK_TOOLS_PATH")
export EDK_TOOLS_PATH
export WORKSPACE="$output/workspace" CONF_PATH="$output/workspace/Conf"
export PACKAGES_PATH="$project:$platform:$edk2"
export GCC5_AARCH64_PREFIX=${GCC5_AARCH64_PREFIX:-aarch64-linux-gnu-}
: "${PYTHON_COMMAND:=${python:-python3}}"
export PYTHON_COMMAND
mkdir -p "$CONF_PATH" "$output/bin"
cd "$WORKSPACE"
set +u
source "$edk2/edksetup.sh"
set -u
build -a AARCH64 -t GCC5 -b RELEASE -p efi/PocketJsUefiPkg.dsc \
  -D "PJB_BOOT_ABI_INCLUDE=$abi" -D "W202_PLATFORM_INCLUDE=$platform/Ums9620Pkg/W202ds/Include" \
  -D "W202_ANDROID_BOOT_ENTER=${W202_ANDROID_BOOT_ENTER:-FALSE}" \
  -D "W202_LK_RETURN_PROBE_ONLY=${W202_LK_RETURN_PROBE_ONLY:-FALSE}" -n "$jobs"
for name in AndroidBoot AndroidLk DebianServer DebianKde HandoffProbe TouchScope; do
  cp "$WORKSPACE/Build/PocketJsUefi/RELEASE_GCC5/AARCH64/$name.efi" "$output/bin/"
done
"$PYTHON_COMMAND" "$repo/../tools/efi_manifest.py" --output "$output" \
  --source "$repo" --platform "$platform" --abi-include "$abi" --edk2 "$edk2"
