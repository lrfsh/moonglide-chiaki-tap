#!/usr/bin/env bash
# Build moonglide-chiaki-tap from scratch (idempotent) and install it to
# ~/.local/share/moonglide/bin/moonglide-chiaki-tap. No root needed: the two -dev packages that are
# not installed (libevent-dev, libsdl2-dev) are unpacked into a private root, protoc comes from
# the GitHub release zip.
# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE="${MOONGLIDE_TAP_CACHE:-$HOME/.cache/moonglide/chiaki-tap}"
DEST="${MOONGLIDE_TAP_DEST:-$HOME/.local/share/moonglide/bin}"
CHIAKI_TAG=v1.10.1
PROTOC_VER=21.12

SRC="$CACHE/chiaki-ng"
LIBBUILD="$CACHE/chiaki-build"
DEPS="$CACHE/deps"
ROOT="$DEPS/root"
PROTOC="$DEPS/protoc/bin/protoc"
BUILD="$CACHE/build"
mkdir -p "$CACHE" "$DEPS" "$DEST"

# 1. chiaki-ng source (pinned tag, submodules)
if [ ! -d "$SRC/.git" ]; then
	git clone --branch "$CHIAKI_TAG" --depth 1 --recurse-submodules --shallow-submodules \
		https://github.com/streetpea/chiaki-ng "$SRC"
fi
# CMake >= 3.28 needs find_package(PkgConfig) before pkg_search_module (one line, idempotent)
if grep -q 'find_program(PKG_CONFIG_EXECUTABLE pkg-config)' "$SRC/lib/CMakeLists.txt"; then
	sed -i 's/^\(\s*\)find_program(PKG_CONFIG_EXECUTABLE pkg-config)/\1find_package(PkgConfig)/' "$SRC/lib/CMakeLists.txt"
fi

# 2. protoc
if [ ! -x "$PROTOC" ]; then
	curl -fsSL -o "$DEPS/protoc.zip" \
		"https://github.com/protocolbuffers/protobuf/releases/download/v$PROTOC_VER/protoc-$PROTOC_VER-linux-x86_64.zip"
	mkdir -p "$DEPS/protoc" && unzip -qo "$DEPS/protoc.zip" -d "$DEPS/protoc"
fi

# 3. dev packages into a private root (only the ones the system lacks)
export PKG_CONFIG_PATH="$ROOT/usr/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
need_pkgs=()
pkg-config --exists libevent || need_pkgs+=(libevent-dev)
pkg-config --exists sdl2 || need_pkgs+=(libsdl2-dev)
if [ ${#need_pkgs[@]} -gt 0 ]; then
	mkdir -p "$DEPS/debs" "$ROOT"
	(cd "$DEPS/debs" && apt-get download "${need_pkgs[@]}")
	for deb in "$DEPS"/debs/*.deb; do dpkg -x "$deb" "$ROOT"; done
	# dangling .so symlinks (they point at the runtime libs in /usr/lib) and pc prefixes
	libdir="$ROOT/usr/lib/x86_64-linux-gnu"
	find "$libdir" -maxdepth 1 -type l ! -exec test -e {} \; -print | while read -r l; do
		t="$(basename "$(readlink "$l")")"
		ln -sf "/usr/lib/x86_64-linux-gnu/$t" "$l"
	done
	find "$libdir/pkgconfig" -name '*.pc' -exec sed -i "s|^prefix=.*|prefix=$ROOT/usr|" {} +
fi
EXTRA_INC=""
[ -d "$ROOT/usr/include/x86_64-linux-gnu" ] && EXTRA_INC="$ROOT"

# 4. libchiaki (library only, no GUI/CLI/decoders)
if [ ! -f "$LIBBUILD/lib/libchiaki.a" ]; then
	cmake -S "$SRC" -B "$LIBBUILD" -G Ninja -DPROTOC="$PROTOC" -DCMAKE_BUILD_TYPE=Release \
		-DCHIAKI_ENABLE_GUI=OFF -DCHIAKI_ENABLE_CLI=OFF -DCHIAKI_ENABLE_TESTS=OFF \
		-DCHIAKI_ENABLE_FFMPEG_DECODER=OFF -DCHIAKI_ENABLE_PI_DECODER=OFF -DCHIAKI_ENABLE_RUDP=OFF \
		-DCHIAKI_LIB_JSONC_EXTERNAL_PROJECT=ON -DCHIAKI_LIB_MINIUPNPC_EXTERNAL_PROJECT=ON \
		-DCHIAKI_ENABLE_SETSU=OFF -DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF -DCHIAKI_ENABLE_SPEEX=OFF \
		-DCHIAKI_ENABLE_STEAM_SHORTCUT=OFF -Wno-dev
	cmake --build "$LIBBUILD"
fi

# 5. the client
cmake -S "$HERE" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCHIAKI_SRC="$SRC" -DCHIAKI_BUILD="$LIBBUILD" -DDEPS_ROOT="$EXTRA_INC" >/dev/null
cmake --build "$BUILD"
install -m 0755 "$BUILD/moonglide-chiaki-tap" "$DEST/moonglide-chiaki-tap"
echo "installed $DEST/moonglide-chiaki-tap"
