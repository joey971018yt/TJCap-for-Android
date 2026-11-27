#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SDK_ROOT="${ANDROID_HOME:-/opt/android-sdk}"
BUILD_TOOLS="$SDK_ROOT/build-tools/35.0.0"
PLATFORM_JAR="$SDK_ROOT/platforms/android-35/android.jar"

if [[ ! -x "$BUILD_TOOLS/aapt2" ]]; then
    BUILD_TOOLS="$(ls -d "$SDK_ROOT"/build-tools/* | sort -V | tail -n 1)"
fi
if [[ ! -f "$PLATFORM_JAR" ]]; then
    PLATFORM_JAR="$(ls "$SDK_ROOT"/platforms/android-*/android.jar | sort -V | tail -n 1)"
fi

echo "[APK] Using Android build-tools: $BUILD_TOOLS"
echo "[APK] Using Android platform   : $PLATFORM_JAR"

# Generate multi-density launcher icons from green assets/app_icon.png
python3 - "$REPO_ROOT/assets/app_icon.png" "$SCRIPT_DIR/app/src/main/res" << 'PY'
import sys, pathlib
from PIL import Image

src = Image.open(sys.argv[1]).convert("RGBA")
res_root = pathlib.Path(sys.argv[2])
densities = {
    "mipmap-mdpi": 48,
    "mipmap-hdpi": 72,
    "mipmap-xhdpi": 96,
    "mipmap-xxhdpi": 144,
    "mipmap-xxxhdpi": 192,
}
for folder, px in densities.items():
    out_dir = res_root / folder
    out_dir.mkdir(parents=True, exist_ok=True)
    im = src.resize((px, px), Image.Resampling.LANCZOS)
    im.save(out_dir / "ic_launcher.png", format="PNG", optimize=True)
PY

WORK_DIR="$REPO_ROOT/build-apk"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/compiled_res" "$WORK_DIR/gen" "$WORK_DIR/classes" "$WORK_DIR/dex" "$REPO_ROOT/companion"

# 1. Compile & link resources with aapt2
"$BUILD_TOOLS/aapt2" compile --dir "$SCRIPT_DIR/app/src/main/res" -o "$WORK_DIR/compiled_res/res.zip"
"$BUILD_TOOLS/aapt2" link \
    -o "$WORK_DIR/unaligned.apk" \
    -I "$PLATFORM_JAR" \
    --manifest "$SCRIPT_DIR/app/src/main/AndroidManifest.xml" \
    --java "$WORK_DIR/gen" \
    --auto-add-overlay \
    "$WORK_DIR/compiled_res/res.zip"

# 2. Compile Java sources against android.jar
find "$SCRIPT_DIR/app/src/main/java" "$WORK_DIR/gen" -name "*.java" > "$WORK_DIR/sources.txt"
javac --release 11 -encoding UTF-8 \
    -cp "$PLATFORM_JAR" \
    -d "$WORK_DIR/classes" \
    @"$WORK_DIR/sources.txt"

# 3. Convert .class files to classes.dex with d8
find "$WORK_DIR/classes" -name "*.class" > "$WORK_DIR/classes.txt"
"$BUILD_TOOLS/d8" \
    --lib "$PLATFORM_JAR" \
    --min-api 26 \
    --release \
    --output "$WORK_DIR/dex" \
    @"$WORK_DIR/classes.txt"

# 4. Add classes.dex to APK (uncompressed / deflated cleanly) and zipalign to 4/16KB page alignment
(cd "$WORK_DIR/dex" && zip -q "$WORK_DIR/unaligned.apk" classes.dex)
"$BUILD_TOOLS/zipalign" -f -p 4 "$WORK_DIR/unaligned.apk" "$WORK_DIR/aligned.apk"

# 5. Sign APK (v1 + v2 + v3 schemes for universal Android 8..16 installation)
KEYSTORE="$WORK_DIR/jcast_release.jks"
keytool -genkeypair -v \
    -keystore "$KEYSTORE" \
    -storepass "jcast2026" \
    -keypass "jcast2026" \
    -alias "jcast_companion" \
    -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=JCast for Android, OU=JCast Project, O=JCast, L=Taipei, ST=Taiwan, C=TW" >/dev/null 2>&1

OUT_APK="$REPO_ROOT/companion/JCast-Companion-v1.0.0.apk"
"$BUILD_TOOLS/apksigner" sign \
    --ks "$KEYSTORE" \
    --ks-key-alias "jcast_companion" \
    --ks-pass pass:jcast2026 \
    --key-pass pass:jcast2026 \
    --v1-signing-enabled true \
    --v2-signing-enabled true \
    --v3-signing-enabled true \
    --out "$OUT_APK" \
    "$WORK_DIR/aligned.apk"

"$BUILD_TOOLS/apksigner" verify --verbose "$OUT_APK" | head -n 6
ls -lh "$OUT_APK"
