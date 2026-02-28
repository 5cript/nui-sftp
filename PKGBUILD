pkgname=nui-sftp
pkgver=a596b43a3c0b501bbf780d351537db5952892698
pkgrel=1
pkgdesc="NUI-based SFTP application"
arch=('x86_64')
url="https://github.com/5cript/nui-sftp"
license=('MIT')
depends=(
    webkitgtk-6.0
    curl
    crypto++
    libssh
    fmt
    boost
    nlohmann-json
)
makedepends=(
    cmake
    ninja
    clang
    lld
    git
    python
    nodejs
)
source=(
    "$pkgname::git+$url.git#tag=$pkgver"
    "git+https://github.com/NuiCpp/Nui.git#commit=123245749ad73c330dccf9e143547a26622b6923"
    "git+https://github.com/5cript/roar.git#commit=a787bce9c8132f4c860bc9e55bff742fd1a3276f"
    "git+https://github.com/DNKpp/gimo.git#commit=16377a6d496b31a9272f9a079c060fba15258bcc"
    "git+https://github.com/NuiCpp/traits.git#commit=6c9caa21c48c9e1f7f039a7bdf8805a0940fce0a"
    "git+https://github.com/NuiCpp/ui5.git#commit=a514318f9110f7e77574abd283ef0c5ecf634f40"
    "git+https://github.com/5cript/5cript-nui-components.git#commit=fb33b5f751eed174b930329fbecf52138e63c0cf"
)
# TODO:
sha256sums=(
    'SKIP'
    'SKIP'
    'SKIP'
    'SKIP'
    'SKIP'
    'SKIP'
    'SKIP'
)

build() {
    cd "$pkgname"

    mkdir -p $srcdir/$pkgname/dependencies

    # Make copies from sources into src/$pkgname/dependencies/*
    cp -r "$srcdir/Nui" "$srcdir/$pkgname/dependencies/Nui"
    cp -r "$srcdir/roar" "$srcdir/$pkgname/dependencies/roar"
    cp -r "$srcdir/gimo" "$srcdir/$pkgname/dependencies/gimo"
    cp -r "$srcdir/traits" "$srcdir/$pkgname/dependencies/traits"
    cp -r "$srcdir/ui5" "$srcdir/$pkgname/dependencies/ui5"
    cp -r "$srcdir/5cript-nui-components" "$srcdir/$pkgname/dependencies/5cript-nui-components"

    cmake -B build \
        -S "$srcdir/$pkgname" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_LINKER=lld \
        -DCMAKE_CXX_STANDARD=23 \
        -DNUI_FETCH_TRAITS=OFF

    cmake --build build
}

package() {
    cd "$pkgname"
    BUILD_DIRECTORY="$srcdir/$pkgname/build" INSTALL_LOCATION="$pkgdir" ./scripts/deploy.sh
}