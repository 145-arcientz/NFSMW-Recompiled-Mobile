#!/usr/bin/env sh
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
command -v java >/dev/null 2>&1 || { echo 'Java 17 or newer is required.' >&2; exit 1; }
if [ -z "${ANDROID_HOME:-}" ] && [ -d "$HOME/Android/Sdk" ]; then
    ANDROID_HOME="$HOME/Android/Sdk"
    export ANDROID_HOME ANDROID_SDK_ROOT="$ANDROID_HOME"
fi
wrapper="$repo_root/android/gradlew"
[ -x "$wrapper" ] || { echo 'Gradle wrapper is missing or not executable.' >&2; exit 1; }
cd "$repo_root/android"
"$wrapper" assembleRelease
printf 'APK: %s\n' "$repo_root/android/app/build/outputs/apk/release/app-release.apk"
