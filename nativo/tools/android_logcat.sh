#!/usr/bin/env sh
set -eu
command -v adb >/dev/null 2>&1 || { echo 'adb was not found on PATH.' >&2; exit 1; }
adb logcat -v time -s NFSMW NFSMW-REX NFSMW-VULKAN NFSMW-MEM NFSMW-AUDIO NFSMW-INPUT REX AndroidRuntime DEBUG
