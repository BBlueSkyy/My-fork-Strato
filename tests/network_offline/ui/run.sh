#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
build=${1:-/tmp/strato-airplane-mode-ui-test}
mkdir -p "$build"
kotlinc \
  "$root/tests/network_offline/ui/host/android/content/SharedPreferences.kt" \
  "$root/tests/network_offline/ui/host/androidx/preference/SwitchPreferenceCompat.kt" \
  "$root/app/src/main/java/org/stratoemu/strato/preference/AirplaneModePreferenceBinder.kt" \
  "$root/tests/network_offline/ui/airplane_mode_preference_tests.kt" \
  -include-runtime -d "$build/airplane-ui-tests.jar"
java -jar "$build/airplane-ui-tests.jar"
