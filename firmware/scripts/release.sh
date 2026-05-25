#!/usr/bin/env bash
# release.sh — RFNETHM Release-Artefakte für den Webflasher bauen.
#
# Bauweise (BOSE-Style):
#   1. Akzeptiert einen RELEASE_TAG (z.B. v0.14.138) als Arg oder env;
#      bei Release-Builds bekommt die Firmware den TAG als VERSION_STRING,
#      damit factory.bin / firmware.bin / manifest.json deckungsgleich
#      sind und der /api/update/check (sobald implementiert) sauber
#      vergleichen kann.
#   2. `pio run -e rfnethm` mit RELEASE_TAG-env (version_bump.py
#      überschreibt FW_VERSION_STRING auf den TAG).
#   3. Generiert unter `webflasher/`:
#        - factory_rfnethm_esp32s3.bin   (merge_bin --flash_mode dio)
#        - firmware_rfnethm_esp32s3.bin  (= app-only, für OTA)
#        - manifest.json                  (esp-web-tools-Schema)
#        - MD5SUMS
#   4. Pre-Release-Test-Assertions (Test 0 aus docs/_internal/release_tests.md):
#        - sdkconfig DIO (sdkconfig.rfnethm UND sdkconfig.defaults)
#        - bootloader.bin Header-Byte 2 == 0x02
#        - factory.bin Header-Byte 2 == 0x02
#
# Was es bewusst NICHT macht:
#   - Kein automatischer git-commit, kein push, kein rsync.
#     Release-Push und Webflasher-Deploy macht der User explizit.
#   - Kein Hineinpatchen auf origin/main — der release-commit wird
#     manuell konsolidiert (squash + tag + push tag).
#
# Aufruf:
#   bash firmware/scripts/release.sh v0.14.138
#   RELEASE_TAG=v0.14.138 bash firmware/scripts/release.sh
#   bash firmware/scripts/release.sh          # ohne tag = dev-build
#                                             # (counter-getrieben)

set -euo pipefail

# ───── Pfade auflösen ────────────────────────────────────────────────────
REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

BUILD_DIR=${BUILD_DIR:-/root/pio-build/rfnethm-build/rfnethm}
ESPTOOL=${ESPTOOL:-$(command -v esptool.py 2>/dev/null || echo "$HOME/.platformio/penv/bin/esptool.py")}
PIO=${PIO:-$HOME/.platformio/penv/bin/pio}
OUT=$REPO_ROOT/webflasher
SDKCONFIG_R=firmware/sdkconfig.rfnethm
SDKCONFIG_D=firmware/sdkconfig.defaults

# ───── Release-Tag-Handling ──────────────────────────────────────────────
RELEASE_TAG="${1:-${RELEASE_TAG:-}}"
if [ -n "$RELEASE_TAG" ]; then
  # strip leading 'v' for the manifest version field (BOSE-convention)
  TAG_STRIPPED="${RELEASE_TAG#v}"
  export RELEASE_TAG
  echo ">>> Release build with tag $RELEASE_TAG (FW_VERSION_STRING = $TAG_STRIPPED)"
else
  TAG_STRIPPED=""
  echo ">>> Dev build (no RELEASE_TAG) — counter-getrieben"
fi

# ───── Test 0 Assertions (Webflasher-Sicherheit) ────────────────────────
# sdkconfig.rfnethm + sdkconfig.defaults müssen DIO setzen.
assert_dio_config() {
  local file=$1
  if grep -qE '^CONFIG_ESPTOOLPY_FLASHMODE_QIO=y' "$file"; then
    echo "ABORT: $file enthält CONFIG_ESPTOOLPY_FLASHMODE_QIO=y" >&2
    echo "       → siehe memory/qio_dio_webflasher_incident.md" >&2
    exit 2
  fi
  if ! grep -qE '^CONFIG_ESPTOOLPY_FLASHMODE_DIO=y' "$file"; then
    echo "ABORT: $file enthält kein CONFIG_ESPTOOLPY_FLASHMODE_DIO=y" >&2
    exit 2
  fi
}
echo "[release.sh] Test 0 — sdkconfig DIO check"
assert_dio_config "$SDKCONFIG_R"
assert_dio_config "$SDKCONFIG_D"
# Zusätzlich für bootloader-spezifisches CONFIG_FLASHMODE_*
if grep -qE '^CONFIG_FLASHMODE_QIO=y' "$SDKCONFIG_R"; then
  echo "ABORT: $SDKCONFIG_R enthält CONFIG_FLASHMODE_QIO=y (bootloader-code)" >&2
  exit 2
fi
echo "   OK — beide sdkconfig auf DIO"

# ───── Build ────────────────────────────────────────────────────────────
[ -x "$PIO" ] || { echo "ABORT: pio fehlt unter $PIO" >&2; exit 1; }
echo "[release.sh] pio run -e rfnethm  (in $REPO_ROOT/firmware)"
(cd "$REPO_ROOT/firmware" && "$PIO" run -e rfnethm)

# Build-Artefakte da?
for f in bootloader.bin partitions.bin ota_data_initial.bin firmware.bin; do
  [ -f "$BUILD_DIR/$f" ] || { echo "ABORT: $BUILD_DIR/$f fehlt nach Build" >&2; exit 1; }
done

# Bootloader-Header byte 2 == 0x02 (DIO)?
bl_byte2=$(xxd -p -s 2 -l 1 "$BUILD_DIR/bootloader.bin")
[ "$bl_byte2" = "02" ] || {
  echo "ABORT: bootloader.bin Header-Byte 2 = 0x$bl_byte2, erwartet 0x02 (DIO)" >&2
  exit 2
}
echo "   bootloader.bin Header-Byte 2 = 0x02  ✔"

# ───── Version resolven (TAG > version.h) ───────────────────────────────
if [ -n "$TAG_STRIPPED" ]; then
  VERSION="$TAG_STRIPPED"
else
  VERSION=$(awk '/^#define FW_VERSION_STRING / {gsub(/"/,"",$3); print $3}' firmware/src/version.h)
fi
echo "[release.sh] Release-Version: v$VERSION"

# ───── Out-Dir vorbereiten ──────────────────────────────────────────────
mkdir -p "$OUT"
rm -f "$OUT"/factory_rfnethm_esp32s3.bin \
      "$OUT"/firmware_rfnethm_esp32s3.bin \
      "$OUT"/manifest.json \
      "$OUT"/MD5SUMS

FACTORY="$OUT/factory_rfnethm_esp32s3.bin"
FIRMWARE="$OUT/firmware_rfnethm_esp32s3.bin"

# ───── factory.bin via merge_bin --flash_mode dio ───────────────────────
echo "[release.sh] esptool merge_bin → factory_rfnethm_esp32s3.bin"
"$ESPTOOL" --chip esp32s3 merge_bin -o "$FACTORY" \
    --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0     "$BUILD_DIR/bootloader.bin" \
    0x8000  "$BUILD_DIR/partitions.bin" \
    0xd000  "$BUILD_DIR/ota_data_initial.bin" \
    0x10000 "$BUILD_DIR/firmware.bin" \
    > /dev/null

# factory.bin Header byte 2 muss auch 0x02 sein
fa_byte2=$(xxd -p -s 2 -l 1 "$FACTORY")
[ "$fa_byte2" = "02" ] || {
  echo "ABORT: factory.bin Header-Byte 2 = 0x$fa_byte2, erwartet 0x02 (DIO)" >&2
  exit 2
}
echo "   factory.bin Header-Byte 2 = 0x02  ✔"

# firmware.bin nur kopieren (= app-only für OTA)
cp "$BUILD_DIR/firmware.bin" "$FIRMWARE"

# ───── manifest.json (esp-web-tools v10) ────────────────────────────────
FW_MD5=$(md5sum "$FIRMWARE" | awk '{print $1}')
cat > "$OUT/manifest.json" <<EOF
{
  "name": "RFNetHM — HomeMatic-RF Network Bridge",
  "version": "$VERSION",
  "funding_url": "https://busware.de",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "improv": true,
      "parts": [
        {
          "path": "factory_rfnethm_esp32s3.bin",
          "offset": 0
        }
      ]
    }
  ],
  "ota": {
    "ESP32-S3": {
      "path": "firmware_rfnethm_esp32s3.bin",
      "md5": "$FW_MD5"
    }
  }
}
EOF

# ───── MD5SUMS ──────────────────────────────────────────────────────────
( cd "$OUT" && md5sum factory_rfnethm_esp32s3.bin firmware_rfnethm_esp32s3.bin manifest.json > MD5SUMS )

# ───── Summary ──────────────────────────────────────────────────────────
echo
echo "=== Release artefacts (version $VERSION) ==="
ls -lh "$OUT"/factory_rfnethm_esp32s3.bin \
      "$OUT"/firmware_rfnethm_esp32s3.bin \
      "$OUT"/manifest.json \
      "$OUT"/MD5SUMS
echo
echo "Public landing page:  https://install.busware.de/rfnethm/"
echo "Deploy (manuell, vom User getriggert):"
echo "  rsync -av webflasher/ 10.10.22.1:/var/www/install/rfnethm/"
echo
if [ -n "$RELEASE_TAG" ]; then
echo "Nach erfolgreichem Webflasher-Deploy:"
echo "  git tag -a $RELEASE_TAG -m \"release $RELEASE_TAG\""
echo "  git push origin $RELEASE_TAG"
fi
