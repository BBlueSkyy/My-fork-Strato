#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
output=${1:-/tmp/strato-airplane-mode-tests}
mkdir -p "$output"
kotlinc \
  "$root/tests/network_offline/host/android/content/Context.kt" \
  "$root/tests/network_offline/host/androidx/preference/PreferenceManager.kt" \
  "$root/app/src/main/java/org/stratoemu/strato/settings/AirplaneModeSettings.kt" \
  "$root/app/src/main/java/org/stratoemu/strato/settings/AirplaneModePolicy.kt" \
  "$root/tests/network_offline/airplane_mode_mapping_tests.kt" \
  "$root/tests/network_offline/airplane_mode_migration_tests.kt" \
  -include-runtime -d "$output/airplane_mode_tests.jar"
java -jar "$output/airplane_mode_tests.jar"
