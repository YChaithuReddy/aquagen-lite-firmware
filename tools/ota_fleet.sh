#!/usr/bin/env bash
# OTA the ENTIRE AquaGen fleet to a firmware version.
# Prereq: that version is built, committed, and published as a GitHub release with aquagen_lite.bin.
# Usage:  ./tools/ota_fleet.sh v2.4.1
set -euo pipefail
VER="${1:?Usage: $0 <version-tag, e.g. v2.4.1>}"
AZ=/opt/homebrew/bin/az
CONN="$(cat ~/aquagen_hub.txt)"
URL="https://github.com/YChaithuReddy/aquagen-lite-firmware/releases/download/${VER}/aquagen_lite.bin"

echo "==> OTA target: $URL"
code=$(curl -sL -o /dev/null -w "%{http_code}" --max-time 30 "$URL")
[ "$code" = "200" ] || { echo "❌ release asset not found (http=$code). Publish the GitHub release first."; exit 1; }
echo "    asset OK (http 200)"

DEVS=$("$AZ" iot hub device-identity list --login "$CONN" --query "[].deviceId" -o tsv)
n=0; ok=0
for d in $DEVS; do
  n=$((n+1))
  if "$AZ" iot hub device-twin update --device-id "$d" --login "$CONN" \
       --desired "{\"ota_enabled\":true,\"ota_url\":\"$URL\"}" >/dev/null 2>&1; then
    ok=$((ok+1)); echo "  ✓ $d"
  else
    echo "  ✗ $d (twin update failed)"
  fi
done
echo "==> Queued OTA on $ok/$n devices. Online boxes flash + roll forward; offline ones update next time they connect."
