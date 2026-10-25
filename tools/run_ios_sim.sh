#!/bin/bash
# Lance build/ios/sim/Portal2.app dans un simulateur iOS démarré sans fenêtre
# et écrit le journal dans build/ios/sim/launch.log.
#   tools/run_ios_sim.sh [secondes] [arguments du jeu...]
set -uo pipefail
cd "$(dirname "$0")/.."
DEVICE=${P2_SIM_DEVICE:-E9CD9753-CA45-4451-9E0F-E2ABC1CC5931}   # iPhone 17
SECONDS_MAX=${1:-120}; shift || true
GAME_ARGS=("$@"); [ ${#GAME_ARGS[@]} -eq 0 ] && GAME_ARGS=(-game portal2 -novid -fullscreen)
GAME_ROOT=$(python3 -c "import json;print(json.load(open('config/local.json'))['game_root'])")
LINUX_ROOT=$(python3 -c "import json;print(json.load(open('config/local.json'))['linux_root'])")
xcrun simctl bootstatus "$DEVICE" -b >/dev/null 2>&1 || xcrun simctl boot "$DEVICE"
xcrun simctl install "$DEVICE" build/ios/sim/Portal2.app
export SIMCTL_CHILD_P2_LOG_FILE="$PWD/build/ios/sim/game.log"
perl -e "alarm $SECONDS_MAX; exec @ARGV" xcrun simctl launch --console-pty --terminate-running-process "$DEVICE" \
  fr.kanapitsas.portal2 --images "$PWD/build/game/prep" --game-dir "$GAME_ROOT" --linux-root "$LINUX_ROOT" \
  -- "${GAME_ARGS[@]}" > build/ios/sim/launch.log 2>&1
status=$?
xcrun simctl terminate "$DEVICE" fr.kanapitsas.portal2 >/dev/null 2>&1
echo "statut $status ; journal build/ios/sim/game.log ($(wc -l < build/ios/sim/game.log) lignes)"
