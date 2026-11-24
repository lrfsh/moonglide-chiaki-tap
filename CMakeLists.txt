# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
cmake_minimum_required(VERSION 3.16)
project(moonglide-chiaki-tap C)
set(CMAKE_C_STANDARD 11)

# Where chiaki-ng's source and (library-only) build live; build.sh passes both.
set(CHIAKI_SRC "" CACHE PATH "chiaki-ng source tree")
set(CHIAKI_BUILD "" CACHE PATH "chiaki-ng build tree (libchiaki.a and its static deps)")
# rp_tap.h (copy of moonglide src/rp_tap.h)
set(RP_TAP_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "directory with rp_tap.h")

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
pkg_check_modules(LIBEVENT REQUIRED IMPORTED_TARGET libevent)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple libpulse)

# build.sh unpacks libsdl2-dev/libevent-dev into a private root (no root access needed); its
# multiarch config header lives in a second include dir.
set(DEPS_ROOT "" CACHE PATH "private dev-package root (usr/include/x86_64-linux-gnu), optional")
if(DEPS_ROOT)
	include_directories(SYSTEM "${DEPS_ROOT}/usr/include/x86_64-linux-gnu")
endif()

function(need_lib var)
	find_library(${var} NAMES ${ARGN} PATHS ${CHIAKI_BUILD} PATH_SUFFIXES
		lib _deps/json-c-build _deps/miniupnpc-build third-party third-party/curl/lib third-party/nanopb
		NO_DEFAULT_PATH REQUIRED)
endfunction()
need_lib(L_CHIAKI chiaki)
need_lib(L_JSONC json-c)
need_lib(L_MINIUPNPC miniupnpc)
need_lib(L_CURL curl)
need_lib(L_NANOPB protobuf-nanopb)
need_lib(L_JERASURE jerasure)
need_lib(L_GF gf_complete)

# libcurl.a (chiaki's own build) was configured with these; some ship without -dev symlinks
# on a stock Ubuntu, so the runtime sonames are accepted too.
set(CURL_DEPS "")
foreach(dep z idn2 psl:libpsl.so.5 ssh2:libssh2.so.1 nghttp2:libnghttp2.so.14 brotlidec zstd)
	string(REPLACE ":" ";" parts "${dep}")
	list(GET parts 0 name)
	find_library(L_${name} NAMES ${parts})
	if(L_${name})
		list(APPEND CURL_DEPS ${L_${name}})
	else()
		message(STATUS "optional curl dependency not found: ${name}")
	endif()
endforeach()

add_executable(moonglide-chiaki-tap moonglide-chiaki-tap.c)
target_include_directories(moonglide-chiaki-tap PRIVATE
	"${CHIAKI_SRC}/lib/include"
	"${CHIAKI_BUILD}/lib/include"      # generated chiaki/config.h
	"${CHIAKI_BUILD}/lib/protobuf"     # generated takion.pb.h
	"${CHIAKI_SRC}/third-party/nanopb"
	"${CHIAKI_SRC}/third-party/curl/include"
	"${CHIAKI_BUILD}/_deps/json-c-build" "${CHIAKI_BUILD}/_deps/json-c-src"
	"${RP_TAP_DIR}")
target_compile_definitions(moonglide-chiaki-tap PRIVATE CURL_STATICLIB)
target_compile_options(moonglide-chiaki-tap PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(moonglide-chiaki-tap PRIVATE
	${L_CHIAKI} ${L_JSONC} ${L_MINIUPNPC} ${L_CURL} ${L_NANOPB} ${L_JERASURE} ${L_GF}
	${CURL_DEPS} OpenSSL::SSL OpenSSL::Crypto PkgConfig::OPUS PkgConfig::LIBEVENT PkgConfig::SDL2 PkgConfig::PULSE
	Threads::Threads m)
install(TARGETS moonglide-chiaki-tap RUNTIME DESTINATION bin)
