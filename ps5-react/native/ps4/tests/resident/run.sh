#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
shared="$(cd "$root/../shared" && pwd)"
out="${RUNNER_TEMP:-/tmp}/trapstar-ps4-resident"
mkdir -p "$out"
cc -O1 -g -fsanitize=address,undefined -c "$root/app_identity.c" -o "$out/identity.o"
c++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DPS4=1 -DOVERDRIVE_RESIDENT=1 -I"$root/tests/worker" -I"$root" -I"$root/resident" -I"$root/torrent" \
  -I"$root/archives/crypto" -I"$shared" "$root/tests/resident/archive_flow.cpp" "$root/resident/archive_platform.cpp" \
  "$shared/archives.cpp" "$shared/archive_writer.cpp" "$shared/archive_preflight.cpp" "$shared/rar5_password.cpp" \
  "$out/identity.o" -larchive -llzma -lz -lmbedcrypto -o "$out/check"
python3 "$root/tests/resident/fixtures.py" "$out"
for kind in zip rar protected wrong-password zip-slip zip-symlink read-failure; do
  # /data is created solely for this disposable CI test, never on a user host.
  test ! -e /data || { echo 'Refusing an existing /data'; exit 1; }
  sudo mkdir /data
  sudo chmod 777 /data
  mode=success
  archive="$out/game.zip"
  password=''
  if [ "$kind" = rar ]; then archive="$out/game.rar"; fi
  if [ "$kind" = protected ]; then archive="$out/protected.rar"; password='Published fixture'; fi
  if [ "$kind" = wrong-password ]; then archive="$out/protected.rar"; password='Incorrect fixture'; mode=failure; fi
  if [ "$kind" = zip-slip ] || [ "$kind" = zip-symlink ]; then mode=failure; archive="$out/$kind.zip"; fi
  if [ "$kind" = read-failure ]; then mode=read-failure; fi
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/check" "$archive" "$out/owned.pkg" "$password" "$mode"
  # The fixed path was newly created and is CI-owned, confirmed above.
  sudo mv /data "$out/data-$kind"
done
