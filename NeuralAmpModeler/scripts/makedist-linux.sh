#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ARCH="${1:-amd64}"
BUILD_DIR="$PROJECT_DIR/build-linux/$ARCH"
OUT_DIR="$BUILD_DIR/out"
PLUGIN_NAME="$(sed -n 's/^#define BUNDLE_NAME "\(.*\)".*$/\1/p' "$PROJECT_DIR/config.h" | tr -d '\r' | head -n 1)"
VERSION="$(sed -n 's/^#define PLUG_VERSION_STR "\(.*\)".*$/\1/p' "$PROJECT_DIR/config.h" | tr -d '\r' | head -n 1)"
test -n "$PLUGIN_NAME" && test -n "$VERSION"

cmake_flags=()
if [[ "${NAM_VERIFY_LINUX_UI:-0}" == 1 ]]; then
  cmake_flags+=("-DNAM_BUILD_LINUX_UI_TEST=ON")
fi
case "$ARCH" in
  amd64)
    ;;
  i386)
    export PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig
    cmake_flags+=("-DCMAKE_C_FLAGS=-m32 -msse2 -mfpmath=sse"
                  "-DCMAKE_CXX_FLAGS=-m32 -msse2 -mfpmath=sse")
    ;;
  *)
    echo "Expected amd64 or i386, got $ARCH" >&2
    exit 2
    ;;
esac

mkdir -p "$OUT_DIR"
cmake -S "$PROJECT_DIR/projects/linux-vst3" -B "$BUILD_DIR/cmake" -DCMAKE_BUILD_TYPE=Release "${cmake_flags[@]}"
cmake --build "$BUILD_DIR/cmake" --config Release --parallel "${NAM_BUILD_JOBS:-3}"
ctest --test-dir "$BUILD_DIR/cmake" --output-on-failure

VST3_DIR="$BUILD_DIR/cmake/VST3/Release/$PLUGIN_NAME.vst3"
APP_PATH="$BUILD_DIR/cmake/bin/Release/nam-on-steroids"
if [[ ! -d "$VST3_DIR" || ! -x "$APP_PATH" ]]; then
  echo "Expected the VST3 bundle at $VST3_DIR and the standalone at $APP_PATH" >&2
  exit 1
fi
elf_class=ELF64
if [[ "$ARCH" == i386 ]]; then
  elf_class=ELF32
fi
readelf -h "$APP_PATH" | awk -v expected="$elf_class" '$1 == "Class:" { found = $2 == expected } END { exit !found }'
vst_binary="$(find "$VST3_DIR" -type f -name '*.so' -print -quit)"
readelf -h "$vst_binary" | awk -v expected="$elf_class" '$1 == "Class:" { found = $2 == expected } END { exit !found }'

STAGING="$BUILD_DIR/package"
install -d "$STAGING/DEBIAN" "$STAGING/usr/bin" "$STAGING/usr/lib/vst3" \
  "$STAGING/usr/share/applications" "$STAGING/usr/share/doc/nam-on-steroids"
install -m755 "$APP_PATH" "$STAGING/usr/bin/nam-on-steroids"
cp -R "$VST3_DIR" "$STAGING/usr/lib/vst3/"
install -m644 "$PROJECT_DIR/installer/ThirdPartyNotices.txt" "$STAGING/usr/share/doc/nam-on-steroids/"
cat > "$STAGING/usr/share/applications/nam-on-steroids.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=NAM On Steroids
Exec=nam-on-steroids
Categories=AudioVideo;Audio;
Terminal=false
DESKTOP
cat > "$STAGING/DEBIAN/control" <<CONTROL
Package: nam-on-steroids
Version: $VERSION
Architecture: $ARCH
Maintainer: The Tone Scientist
Depends: libgtk-3-0 | libgtk-3-0t64, libasound2 | libasound2t64, libstdc++6 (>= 12), libatomic1
Section: sound
Priority: optional
Description: Neural Amp Modeler with oversampling and a modular signal chain
 Installs the standalone application and the VST3 plugin.
CONTROL
PACKAGE="$OUT_DIR/NAM-On-Steroids-v$VERSION-linux-$ARCH.deb"
dpkg-deb --root-owner-group --build "$STAGING" "$PACKAGE"
dpkg-deb --contents "$PACKAGE"
if [[ "${NAM_VERIFY_LINUX_UI:-0}" == 1 ]]; then
  NAM_LINUX_EDITOR_PREVIEW="$OUT_DIR/editor.png" bash "$SCRIPT_DIR/check-linux-ui.sh" "$PACKAGE" "$BUILD_DIR/cmake/bin/Release/NAMLinuxEditorSmoke"
fi
