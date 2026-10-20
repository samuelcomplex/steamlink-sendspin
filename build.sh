#!/usr/bin/env bash
# Builds sendspin-steamlink for the Steam Link and packs dist/steamlink-sendspin.sh.
#
# Needs: cmake, git, python3 with `pip install ziglang` (zig cross-compiles C++20 against the
# Steam Link's glibc 2.19; Valve's SDK GCC 9.2 is too old for sendspin-cpp), and Go 1.24+ for the
# installer apps (skipped when Go isn't installed).
#
#   ./build.sh            legacy protocol, for Music Assistant 2.10.x  (sendspin-cpp v0.8.0)
#   ./build.sh encrypted  Sendspin 1.0 encrypted protocol, for newer servers (sendspin-cpp v0.9.5+)
set -euo pipefail
cd "$(dirname "$0")"

VARIANT="${1:-legacy}"
if [ "$VARIANT" = legacy ]; then
    REF=v0.8.0; LEGACY=ON
else
    REF="${SENDSPIN_CPP_REF:-v0.9.5}"; LEGACY=OFF
fi

if [ ! -d deps/sendspin-cpp-$REF ]; then
    git clone --quiet --branch "$REF" --depth 1 https://github.com/Sendspin/sendspin-cpp.git \
        deps/sendspin-cpp-$REF
fi

BUILD=build/arm-$VARIANT
cmake -S . -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE=toolchain/steamlink-zig.cmake \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DSENDSPIN_LEGACY_API=$LEGACY \
    -DSENDSPIN_CPP_DIR="$PWD/deps/sendspin-cpp-$REF"
cmake --build "$BUILD" -j"$(nproc)"

# The Steam Link has glibc 2.19: refuse a binary that needs anything newer.
NEED=$(readelf -V "$BUILD/sendspin-steamlink" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
echo "highest glibc symbol: $NEED"
[ "$(printf '%s\nGLIBC_2.19\n' "$NEED" | sort -V | tail -1)" = GLIBC_2.19 ] || {
    echo "binary needs newer glibc than the Steam Link has"; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp "$BUILD/sendspin-steamlink" device/run.sh device/ctl.sh device/S99steam "$STAGE/"
chmod +x "$STAGE"/*
mkdir -p dist
OUT=dist/steamlink-sendspin.sh
sed "s|@BUILD_INFO@|$VARIANT, sendspin-cpp $REF, $(date -u +%Y-%m-%d)|" \
    host/steamlink-sendspin.template.sh > "$OUT"
tar czf - -C "$STAGE" . | base64 -w 76 >> "$OUT"
chmod +x "$OUT"
echo "packed $OUT ($(du -h "$OUT" | cut -f1))"

# The GUI app (Windows .exe and Linux binaries), with the same payload embedded.
if command -v go >/dev/null && [ "${SKIP_APPS:-0}" != 1 ]; then
    # Resolve the Go libraries (a local app/go.work, if present, overrides where they come from).
    (cd app && { [ -f go.work ] || [ -f go.sum ] || go mod tidy; })
    (cd "$STAGE" && tar --format=ustar -czf "$OLDPWD/app/payload.tar.gz" sendspin-steamlink run.sh ctl.sh S99steam)
    if [ "$VARIANT" = legacy ]; then FOR="Music Assistant 2.10"; else FOR="Sendspin 1.0 servers"; fi
    INFO="built $(date -u +%Y-%m-%d) for $FOR"
    (cd app && CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -trimpath \
        -ldflags "-s -w -H windowsgui -X 'main.buildInfo=$INFO'" -o ../dist/SteamLinkSpeaker.exe .)
    (cd app && CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath \
        -ldflags "-s -w -X 'main.buildInfo=$INFO'" -o ../dist/steamlink-speaker-linux-x64 .)
    (cd app && CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath \
        -ldflags "-s -w -X 'main.buildInfo=$INFO'" -o ../dist/steamlink-speaker-linux-arm64 .)
    ls -la dist/
fi
