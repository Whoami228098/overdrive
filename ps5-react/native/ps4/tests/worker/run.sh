#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
shared="$(cd "$root/../shared" && pwd)"
testdir="$root/tests/worker"
out="${RUNNER_TEMP:-/tmp}/trapstar-ps4-workers"
mkdir -p "$out"
cc -O1 -g -fsanitize=address,undefined -c "$root/app_identity.c" -o "$out/identity.o"
c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$testdir" -I"$root" -I"$shared" "$testdir/check.cpp" "$root/app_control.cpp" "$root/package_installer.cpp" \
  "$out/identity.o" -Wl,--wrap=open -o "$out/check"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/check"
