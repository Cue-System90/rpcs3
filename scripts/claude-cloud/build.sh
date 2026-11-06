#!/usr/bin/env bash
# RPCS3 build helper for Claude Code cloud sessions (works on any Linux box with Docker).
# Builds inside the official RPCS3 CI image, so the toolchain matches upstream Linux CI
# (clang-22 / gcc-13, Qt 6.12, LLVM 22, Vulkan, SDL3, ffmpeg).
#
# Usage: scripts/claude-cloud/build.sh [options]
#   --compiler clang|gcc   Compiler (default: clang)
#   --target NAME          Build only this target (rpcs3_emu, rpcs3_ui, rpcs3_lib, rpcs3, rpcs3_test, run_tests)
#   --tests                Configure with unit tests; default target becomes run_tests (build + run)
#   --lto                  Enable LTO like CI (slow and memory hungry; default: off)
#   --jobs N               Parallel jobs (default: nproc)
#   --reconfigure          Re-run CMake even if nothing changed
#   --clean                Delete the build directory first
#   --submodules-only      Only fetch the submodules, do not build
#   --exec "CMD"           Run CMD inside the CI container (repo root, CC/CXX set)
#   -h, --help             Show this help
#
# Build directory: build-cloud-<compiler>/ (ignored by .gitignore).
# Long builds: run in the background and poll the log, e.g.
#   nohup scripts/claude-cloud/build.sh > /tmp/rpcs3-build.log 2>&1 &
# Override the image with RPCS3_CI_IMAGE=<image:tag>.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEFAULT_IMAGE="rpcs3/rpcs3-ci-jammy:2.2"

COMPILER="clang"
TARGET=""
TESTS="OFF"
LTO="OFF"
JOBS="$(nproc)"
RECONFIGURE=0
CLEAN=0
SUBMODULES_ONLY=0
EXEC_CMD=""

usage() { sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
log() { echo "[rpcs3-build] $*" >&2; }
die() { log "ERROR: $*"; exit 1; }
need_arg() { [ $# -ge 2 ] && [ -n "$2" ] || die "$1 needs a value"; }

while [ $# -gt 0 ]; do
	case "$1" in
		--compiler) need_arg "$@"; COMPILER="$2"; shift 2 ;;
		--target) need_arg "$@"; TARGET="$2"; shift 2 ;;
		--tests) TESTS="ON"; shift ;;
		--lto) LTO="ON"; shift ;;
		--jobs) need_arg "$@"; JOBS="$2"; shift 2 ;;
		--reconfigure) RECONFIGURE=1; shift ;;
		--clean) CLEAN=1; shift ;;
		--submodules-only) SUBMODULES_ONLY=1; shift ;;
		--exec) need_arg "$@"; EXEC_CMD="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) die "unknown option: $1 (see --help)" ;;
	esac
done

case "$COMPILER" in
	clang|gcc) ;;
	*) die "--compiler must be clang or gcc" ;;
esac
if [ "$TESTS" = "ON" ] && [ -z "$TARGET" ]; then
	TARGET="run_tests"
fi

# Image tag used by upstream CI (x86_64 Linux job); falls back to the pinned default.
detect_image() {
	if [ -n "${RPCS3_CI_IMAGE:-}" ]; then
		echo "$RPCS3_CI_IMAGE"
		return
	fi
	local img
	img="$(grep -oE 'rpcs3/rpcs3-ci-jammy:[0-9][0-9.]*' "$REPO_ROOT/.github/workflows/rpcs3.yml" 2>/dev/null | head -n1 || true)"
	echo "${img:-$DEFAULT_IMAGE}"
}

# Same selection as .ci/build-linux.sh: llvm, SDL, curl and zlib come from the CI image.
# Relative submodule URLs are pinned to github.com, because the clone's origin may be a proxy URL.
init_submodules() {
	local key name path url
	local paths=()
	while read -r key path; do
		name="${key#submodule.}"
		name="${name%.path}"
		case "$path" in
			*llvm*|*libsdl-org*|*curl*|*zlib*) continue ;;
		esac
		url="$(git -C "$REPO_ROOT" config -f .gitmodules --get "submodule.$name.url")"
		case "$url" in
			../../*) url="https://github.com/${url#../../}" ;;
		esac
		git -C "$REPO_ROOT" config "submodule.$name.url" "$url"
		paths+=("$path")
	done < <(git -C "$REPO_ROOT" config -f .gitmodules --get-regexp '^submodule\..*\.path$')

	log "updating ${#paths[@]} submodules ..."
	if ! git -C "$REPO_ROOT" submodule update --init --depth 1 --jobs 8 -- "${paths[@]}"; then
		log "shallow submodule fetch failed, retrying with full history ..."
		git -C "$REPO_ROOT" submodule update --init --jobs 8 -- "${paths[@]}"
	fi
}

ensure_docker() {
	command -v docker >/dev/null 2>&1 || die "docker not found"
	if docker info >/dev/null 2>&1; then
		return 0
	fi
	if [ "$(id -u)" -eq 0 ] && command -v dockerd >/dev/null 2>&1; then
		log "starting dockerd ..."
		nohup dockerd >/var/log/dockerd.log 2>&1 &
		for _ in $(seq 1 60); do
			docker info >/dev/null 2>&1 && return 0
			sleep 1
		done
	fi
	die "Docker daemon not reachable (see /var/log/dockerd.log)"
}

ensure_image() {
	if docker image inspect "$IMAGE" >/dev/null 2>&1; then
		return 0
	fi
	log "pulling $IMAGE (about 1 GB) ..."
	docker pull --quiet "$IMAGE" >/dev/null
}

# Runs inside the container (environment comes from the CI image and from docker run -e).
read -r -d '' CONTAINER_SCRIPT <<'EOS' || true
set -euo pipefail
git config --global --add safe.directory '*'

if [ "$COMPILER" = "gcc" ]; then
	export CC="$GCC_BINARY" CXX="$GXX_BINARY"
	LINKER=gold
	AR="/usr/bin/gcc-ar-$GCCVER"
	RANLIB="/usr/bin/gcc-ranlib-$GCCVER"
else
	export CC="$CLANG_BINARY" CXX="$CLANGXX_BINARY"
	LINKER=lld
	AR="/usr/bin/llvm-ar-$LLVMVER"
	RANLIB="/usr/bin/llvm-ranlib-$LLVMVER"
fi

if [ -n "$EXEC_CMD" ]; then
	exec bash -c "$EXEC_CMD"
fi

EXTRA_FLAGS=""
if [ "$COMPILER" = "gcc" ] && [ "$LTO" = "ON" ]; then
	EXTRA_FLAGS="-fuse-linker-plugin"
fi

if [ "$DO_CONFIGURE" = "1" ]; then
	cmake -S . -B "$BUILD_DIR" -G Ninja \
		-DUSE_NATIVE_INSTRUCTIONS=OFF \
		-DUSE_PRECOMPILED_HEADERS=OFF \
		-DUSE_LTO="$LTO" \
		-DCMAKE_C_FLAGS="$EXTRA_FLAGS" \
		-DCMAKE_CXX_FLAGS="$EXTRA_FLAGS" \
		-DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=$LINKER" \
		-DCMAKE_MODULE_LINKER_FLAGS="-fuse-ld=$LINKER" \
		-DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=$LINKER" \
		-DCMAKE_AR="$AR" \
		-DCMAKE_RANLIB="$RANLIB" \
		-DUSE_SYSTEM_CURL=ON \
		-DUSE_SDL=ON \
		-DUSE_SYSTEM_SDL=ON \
		-DUSE_SYSTEM_FFMPEG=ON \
		-DUSE_DISCORD_RPC=ON \
		-DOpenGL_GL_PREFERENCE=LEGACY \
		-DLLVM_DIR=/opt/llvm/lib/cmake/llvm \
		-DSTATIC_LINK_LLVM=ON \
		-DBUILD_RPCS3_TESTS="$TESTS" \
		-DRUN_RPCS3_TESTS="$TESTS"
	printf '%s\n' "$OPTS" > "$BUILD_DIR/.claude-cloud-options"
fi

start=$(date +%s)
if [ -n "$TARGET" ]; then
	cmake --build "$BUILD_DIR" -j "$JOBS" --target "$TARGET"
else
	cmake --build "$BUILD_DIR" -j "$JOBS"
fi
echo "[rpcs3-build] build finished in $(( $(date +%s) - start ))s"
EOS

IMAGE="$(detect_image)"
BUILD_DIR="build-cloud-$COMPILER"
CCACHE_HOST_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/rpcs3-ccache"
OPTS="compiler=$COMPILER tests=$TESTS lto=$LTO image=$IMAGE"

init_submodules
if [ "$SUBMODULES_ONLY" = "1" ]; then
	log "submodules ready"
	exit 0
fi

ensure_docker
ensure_image

if [ "$CLEAN" = "1" ]; then
	log "removing $BUILD_DIR"
	rm -rf "${REPO_ROOT:?}/$BUILD_DIR"
fi

DO_CONFIGURE=0
if [ "$RECONFIGURE" = "1" ] \
	|| [ ! -f "$REPO_ROOT/$BUILD_DIR/build.ninja" ] \
	|| [ "$(cat "$REPO_ROOT/$BUILD_DIR/.claude-cloud-options" 2>/dev/null || true)" != "$OPTS" ]; then
	DO_CONFIGURE=1
fi

user_args=()
if [ "$(id -u)" -ne 0 ]; then
	user_args=(--user "$(id -u):$(id -g)" -e HOME=/tmp)
fi
mkdir -p "$CCACHE_HOST_DIR"

if [ -n "$EXEC_CMD" ]; then
	log "exec in $IMAGE: $EXEC_CMD"
else
	log "image=$IMAGE compiler=$COMPILER target=${TARGET:-all} tests=$TESTS lto=$LTO jobs=$JOBS dir=$BUILD_DIR configure=$DO_CONFIGURE"
fi

docker run --rm --init \
	"${user_args[@]}" \
	-v "$REPO_ROOT:/rpcs3" -w /rpcs3 \
	-v "$CCACHE_HOST_DIR:/ccache" -e CCACHE_DIR=/ccache \
	-e COMPILER="$COMPILER" \
	-e BUILD_DIR="$BUILD_DIR" \
	-e TARGET="$TARGET" \
	-e TESTS="$TESTS" \
	-e LTO="$LTO" \
	-e JOBS="$JOBS" \
	-e DO_CONFIGURE="$DO_CONFIGURE" \
	-e OPTS="$OPTS" \
	-e EXEC_CMD="$EXEC_CMD" \
	"$IMAGE" bash -c "$CONTAINER_SCRIPT"
