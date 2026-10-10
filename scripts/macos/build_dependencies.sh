#!/bin/bash
# Builds libc++ and every library nui-sftp links on macOS from pinned sources into one static prefix, with the
# LLVM that _cmake/macos/toolchain.cmake compiles nui-sftp with. Libraries the OS ships with a C interface
# (zlib, bzip2, curl, sqlite) come from the SDK. Finished parts are skipped on reruns.
#
# Usage: build_dependencies.sh [--prefix <dir>] [--jobs <n>]
#   --prefix  defaults to $NUI_SFTP_DEPENDENCY_PREFIX or ~/.cache/nui-sftp/macos-dependencies
#   NUI_SFTP_LLVM_ROOT overrides the LLVM, default is `brew --prefix llvm@23`.

set -Eeuo pipefail

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
SOURCE_DIRECTORY=$( cd -- "${SCRIPT_DIR}/../.." &> /dev/null && pwd )
TOOLCHAIN_FILE="${SOURCE_DIRECTORY}/_cmake/macos/toolchain.cmake"

LLVM_FORMULA="llvm@23"
LLVM_MAJOR="23"
MINIMUM_MACOS="15.2"
# libc++ keeps its symbols in this inline namespace, so nothing built against the system libc++ can link to it.
LIBCXX_ABI_NAMESPACE="__nui"

LIBCXX_VERSION="23.1.3"
LIBCXX_SHA256="c44186a7762ed28954be72e5ff6df9808e0779d4f1bf014ecc4e7e211d31ee34"
ZSTD_VERSION="1.5.7"
ZSTD_SHA256="eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3"
XZ_VERSION="5.8.4"
XZ_SHA256="4ce24038fd4221e0d13bc1a2de7a4db56e90b92b3bf75321f6c14be73f65de4b"
OPENSSL_VERSION="3.5.9"
OPENSSL_SHA256="603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a"
LIBSSH_VERSION="0.12.2"
LIBSSH_SHA256="49560f677d96e3706a904ac2de1116e25f3680937d51e5c92198fcba4a1c1e9f"
FMT_VERSION="12.1.0"
FMT_SHA256="695fd197fa5aff8fc67b5f2bbc110490a875cdf7a41686ac8512fb480fa8ada7"
JSON_VERSION="3.12.0"
JSON_SHA256="42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa"
CRYPTOPP_VERSION="8.9.0"
CRYPTOPP_SHA256="4cc0ccc324625b80b695fcd3dee63a66f1a460d3e51b71640cdbfc4cd1a3779c"
BOOST_VERSION="1.90.0"
BOOST_SHA256="5e93d582aff26868d581a52ae78c7d8edf3f3064742c6e77901a1f18a437eea9"
GOOGLETEST_VERSION="1.17.0"
GOOGLETEST_SHA256="65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c"

PREFIX="${NUI_SFTP_DEPENDENCY_PREFIX:-${HOME}/.cache/nui-sftp/macos-dependencies}"
JOBS="$(sysctl -n hw.ncpu)"

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

LLVM_ROOT="${NUI_SFTP_LLVM_ROOT:-$(brew --prefix "${LLVM_FORMULA}")}"
CLANG="${LLVM_ROOT}/bin/clang"
CLANGXX="${LLVM_ROOT}/bin/clang++"
[ -x "${CLANGXX}" ] || { echo "No clang++ in ${LLVM_ROOT}, brew install ${LLVM_FORMULA}" >&2; exit 1; }
[ "$("${CLANG}" -dumpversion | cut -d. -f1)" = "${LLVM_MAJOR}" ] || {
    echo "${CLANG} is not LLVM ${LLVM_MAJOR}" >&2
    exit 1
}

# Upstream clang honours both for the builds that do not go through CMake.
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
export MACOSX_DEPLOYMENT_TARGET="${MINIMUM_MACOS}"

WORK="${PREFIX}.work"
DOWNLOADS="${NUI_SFTP_DOWNLOAD_CACHE:-${HOME}/.cache/nui-sftp/downloads}"
STAMPS="${PREFIX}/.stamps"

# Anything built for another LLVM major or deployment target has to go.
KEY="llvm${LLVM_MAJOR}-macos${MINIMUM_MACOS}-${LIBCXX_ABI_NAMESPACE}"
if [ -f "${STAMPS}/key" ] && [ "$(cat "${STAMPS}/key")" != "${KEY}" ]; then
    echo "Prefix was built for $(cat "${STAMPS}/key"), rebuilding for ${KEY}"
    rm -rf "${PREFIX}"
fi
mkdir -p "${PREFIX}" "${WORK}" "${DOWNLOADS}" "${STAMPS}"
echo "${KEY}" > "${STAMPS}/key"

LIBCXX_COMPILE_FLAGS="-stdlib++-isystem ${PREFIX}/include/c++/v1"
LIBCXX_LINK_FLAGS="-nostdlib++ ${PREFIX}/lib/libc++.a ${PREFIX}/lib/libc++abi.a"

# fetch <url> <sha256>: downloads into the cache once, prints the path.
fetch() {
    local url="$1" sha256="$2"
    local file="${DOWNLOADS}/$(basename "${url}")"
    if [ ! -f "${file}" ]; then
        curl -fsSL --retry 3 -o "${file}.part" "${url}" >&2
        mv "${file}.part" "${file}"
    fi
    echo "${sha256}  ${file}" | shasum -a 256 -c - >/dev/null || {
        echo "Checksum mismatch for ${file}" >&2
        exit 1
    }
    echo "${file}"
}

# unpack <archive> <name>: fresh extraction into $WORK/<name>, prints the source directory.
unpack() {
    local archive="$1" name="$2"
    local directory="${WORK}/${name}"
    rm -rf "${directory}"
    mkdir -p "${directory}"
    case "${archive}" in
        *.zip) unzip -q "${archive}" -d "${directory}" ;;
        *) tar -xf "${archive}" -C "${directory}" ;;
    esac
    # Archives with a single top level directory unpack into it.
    local entries=("${directory}"/*)
    if [ ${#entries[@]} -eq 1 ] && [ -d "${entries[0]}" ]; then
        echo "${entries[0]}"
    else
        echo "${directory}"
    fi
}

built() {
    [ -f "${STAMPS}/$1-$2" ]
}

mark_built() {
    touch "${STAMPS}/$1-$2"
    printf '\033[32mBuilt %s %s\033[0m\n' "$1" "$2"
}

# cmake_dependency <source directory> <build directory> [cmake arguments]
cmake_dependency() {
    local source="$1" build="$2"
    shift 2
    cmake -G Ninja -S "${source}" -B "${build}" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
        -DNUI_SFTP_LLVM_ROOT="${LLVM_ROOT}" \
        -DNUI_SFTP_DEPENDENCY_PREFIX="${PREFIX}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${MINIMUM_MACOS}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_TESTING=OFF \
        "$@"
    cmake --build "${build}" --parallel "${JOBS}"
    cmake --install "${build}"
}

build_libcxx() {
    built libcxx "${LIBCXX_VERSION}" && return
    local archive
    archive="$(fetch "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LIBCXX_VERSION}/llvm-project-${LIBCXX_VERSION}.src.tar.xz" "${LIBCXX_SHA256}")"
    local top="llvm-project-${LIBCXX_VERSION}.src"
    rm -rf "${WORK}/libcxx"
    mkdir -p "${WORK}/libcxx"
    tar -xf "${archive}" -C "${WORK}/libcxx" \
        "${top}/runtimes" "${top}/libcxx" "${top}/libcxxabi" "${top}/libunwind" "${top}/libc" \
        "${top}/cmake" "${top}/llvm/cmake" "${top}/llvm/utils" "${top}/third-party"
    # Hermetic: the archives export nothing, so the system libc++ the frameworks load never sees them.
    cmake -G Ninja -S "${WORK}/libcxx/${top}/runtimes" -B "${WORK}/libcxx/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
        -DCMAKE_C_COMPILER="${CLANG}" \
        -DCMAKE_CXX_COMPILER="${CLANGXX}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${MINIMUM_MACOS}" \
        -DCMAKE_OSX_SYSROOT="${SDKROOT}" \
        -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
        -DLIBCXX_ENABLE_SHARED=OFF \
        -DLIBCXXABI_ENABLE_SHARED=OFF \
        -DLIBCXX_HERMETIC_STATIC_LIBRARY=ON \
        -DLIBCXXABI_HERMETIC_STATIC_LIBRARY=ON \
        -DLIBCXX_ABI_NAMESPACE="${LIBCXX_ABI_NAMESPACE}" \
        -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
        -DLIBCXX_INCLUDE_TESTS=OFF \
        -DLIBCXXABI_INCLUDE_TESTS=OFF \
        -DLIBCXX_INCLUDE_BENCHMARKS=OFF
    cmake --build "${WORK}/libcxx/build" --parallel "${JOBS}"
    cmake --install "${WORK}/libcxx/build"
    mark_built libcxx "${LIBCXX_VERSION}"
}

build_zstd() {
    built zstd "${ZSTD_VERSION}" && return
    local source
    source="$(unpack "$(fetch "https://github.com/facebook/zstd/releases/download/v${ZSTD_VERSION}/zstd-${ZSTD_VERSION}.tar.gz" "${ZSTD_SHA256}")" zstd)"
    cmake_dependency "${source}/build/cmake" "${WORK}/zstd-build" \
        -DZSTD_BUILD_SHARED=OFF \
        -DZSTD_BUILD_STATIC=ON \
        -DZSTD_BUILD_PROGRAMS=OFF \
        -DZSTD_BUILD_TESTS=OFF
    mark_built zstd "${ZSTD_VERSION}"
}

build_xz() {
    built xz "${XZ_VERSION}" && return
    local source
    source="$(unpack "$(fetch "https://github.com/tukaani-project/xz/releases/download/v${XZ_VERSION}/xz-${XZ_VERSION}.tar.xz" "${XZ_SHA256}")" xz)"
    cmake_dependency "${source}" "${WORK}/xz-build" \
        -DXZ_NLS=OFF \
        -DXZ_DOC=OFF \
        -DXZ_TOOL_XZ=OFF \
        -DXZ_TOOL_XZDEC=OFF \
        -DXZ_TOOL_LZMADEC=OFF \
        -DXZ_TOOL_LZMAINFO=OFF \
        -DXZ_TOOL_SCRIPTS=OFF
    mark_built xz "${XZ_VERSION}"
}

build_openssl() {
    built openssl "${OPENSSL_VERSION}" && return
    local source
    source="$(unpack "$(fetch "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz" "${OPENSSL_SHA256}")" openssl)"
    (
        cd "${source}"
        ./Configure darwin64-arm64-cc no-shared no-tests no-docs no-apps \
            --prefix="${PREFIX}" --libdir=lib --openssldir=/private/etc/ssl CC="${CLANG}"
        make -j "${JOBS}"
        make install_sw
    )
    mark_built openssl "${OPENSSL_VERSION}"
}

build_libssh() {
    built libssh "${LIBSSH_VERSION}" && return
    local source
    source="$(unpack "$(fetch "https://www.libssh.org/files/${LIBSSH_VERSION%.*}/libssh-${LIBSSH_VERSION}.tar.xz" "${LIBSSH_SHA256}")" libssh)"
    cmake_dependency "${source}" "${WORK}/libssh-build" \
        -DWITH_EXAMPLES=OFF \
        -DUNIT_TESTING=OFF \
        -DCLIENT_TESTING=OFF \
        -DWITH_GSSAPI=OFF \
        -DWITH_ZLIB=ON
    mark_built libssh "${LIBSSH_VERSION}"
}

build_fmt() {
    built fmt "${FMT_VERSION}" && return
    local source
    source="$(unpack "$(fetch "https://github.com/fmtlib/fmt/releases/download/${FMT_VERSION}/fmt-${FMT_VERSION}.zip" "${FMT_SHA256}")" fmt)"
    cmake_dependency "${source}" "${WORK}/fmt-build" -DFMT_TEST=OFF -DFMT_DOC=OFF
    mark_built fmt "${FMT_VERSION}"
}

build_json() {
    built json "${JSON_VERSION}" && return
    local source
    source="$(unpack "$(fetch "https://github.com/nlohmann/json/releases/download/v${JSON_VERSION}/json.tar.xz" "${JSON_SHA256}")" json)"
    cmake_dependency "${source}" "${WORK}/json-build" -DJSON_BuildTests=OFF -DJSON_Install=ON
    mark_built json "${JSON_VERSION}"
}

build_cryptopp() {
    built cryptopp "${CRYPTOPP_VERSION}" && return
    local source
    source="$(unpack "$(fetch "https://github.com/weidai11/cryptopp/releases/download/CRYPTOPP_${CRYPTOPP_VERSION//./_}/cryptopp${CRYPTOPP_VERSION//./}.zip" "${CRYPTOPP_SHA256}")" cryptopp)"
    # The makefile probes the ARM crypto extensions with small test programs, those have to link too.
    make -C "${source}" -j "${JOBS}" static \
        CXX="${CLANGXX}" \
        CXXFLAGS="-DNDEBUG -O3 -fPIC ${LIBCXX_COMPILE_FLAGS}" \
        LDFLAGS="${LIBCXX_LINK_FLAGS}"
    mkdir -p "${PREFIX}/include/cryptopp" "${PREFIX}/lib"
    cp "${source}"/*.h "${PREFIX}/include/cryptopp/"
    cp "${source}/libcryptopp.a" "${PREFIX}/lib/"
    mark_built cryptopp "${CRYPTOPP_VERSION}"
}

build_boost() {
    built boost "${BOOST_VERSION}" && return
    local source
    source="$(unpack "$(fetch "https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${BOOST_VERSION//./_}.tar.gz" "${BOOST_SHA256}")" boost)"
    (
        cd "${source}"
        ./bootstrap.sh
        cat > user-config.jam <<JAM
using clang : nui : "${CLANGXX}" : <cxxflags>"${LIBCXX_COMPILE_FLAGS}" <linkflags>"${LIBCXX_LINK_FLAGS}" ;
JAM
        # Only the compiled libraries are listed, the headers of all of boost are installed regardless.
        # Nui gzips its assets through iostreams, with the zlib of the SDK. The standard matches nui-sftp,
        # boost's configuration macros change signatures between standards.
        ./b2 --user-config=user-config.jam toolset=clang-nui \
            variant=release link=static threading=multi cxxstd=23 cxxstd-dialect=gnu \
            --prefix="${PREFIX}" --layout=system -j "${JOBS}" -d1 \
            -sNO_BZIP2=1 -sNO_ZSTD=1 -sNO_LZMA=1 \
            --with-atomic --with-chrono --with-container --with-context --with-date_time --with-exception \
            --with-filesystem --with-iostreams --with-process --with-program_options --with-random \
            --with-serialization --with-system --with-thread \
            install
    )
    mark_built boost "${BOOST_VERSION}"
}

build_googletest() {
    built googletest "${GOOGLETEST_VERSION}" && return
    local source
    source="$(unpack "$(fetch "https://github.com/google/googletest/releases/download/v${GOOGLETEST_VERSION}/googletest-${GOOGLETEST_VERSION}.tar.gz" "${GOOGLETEST_SHA256}")" googletest)"
    cmake_dependency "${source}" "${WORK}/googletest-build" -DINSTALL_GTEST=ON
    mark_built googletest "${GOOGLETEST_VERSION}"
}

build_libcxx
build_zstd
build_xz
build_openssl
build_libssh
build_fmt
build_json
build_cryptopp
build_boost
build_googletest

rm -rf "${WORK}"
echo "Dependencies are in ${PREFIX}"
