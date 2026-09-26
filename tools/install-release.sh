#!/bin/sh
# Install the nio compiler from a GitHub release (macOS and Linux).
#
#   curl -fsSL https://github.com/nio-org/nio/releases/latest/download/install.sh | sh
#
# The release publishes this file as install.sh. It downloads the archive for
# this host, checks it against the release's SHA256SUMS, and puts `nio` in
# $NIO_INSTALL. It changes no shell configuration file: when that directory is
# not on PATH, it prints the line to add. tools/install.sh builds the working
# tree from source into the same directory.
#
# Environment:
#   NIO_VERSION       the release to install, e.g. v0.1.0   (default: the latest)
#   NIO_INSTALL       the directory to put nio in            (default: ~/.local/bin)
#   NIO_REPO          the GitHub repository of the releases  (default: nio-org/nio)
#   NIO_DOWNLOAD_URL  a directory with the release files, for a mirror;
#                     replaces the two above
#
# Windows uses tools/install-release.ps1, published as install.ps1.
set -eu

repo=${NIO_REPO:-nio-org/nio}
version=${NIO_VERSION:-latest}
dest=${NIO_INSTALL:-$HOME/.local/bin}

fail() {
    echo "nio install: $*" >&2
    exit 1
}

case $(uname -s) in
Darwin) os=darwin ;;
Linux) os=linux ;;
MINGW* | MSYS* | CYGWIN*) fail "on Windows, use install.ps1 from the same release" ;;
*) fail "no prebuilt nio for $(uname -s); build it from source: https://nio-lang.org/docs/installation" ;;
esac

case $(uname -m) in
arm64 | aarch64) arch=arm64 ;;
x86_64 | amd64) arch=x86_64 ;;
*) arch=$(uname -m) ;;
esac

case $os-$arch in
darwin-arm64 | linux-x86_64) ;;
*) fail "no prebuilt nio for $os-$arch; build it from source: https://nio-lang.org/docs/installation" ;;
esac

asset=nio-$os-$arch.tar.gz

if [ -n "${NIO_DOWNLOAD_URL:-}" ]; then
    base=$NIO_DOWNLOAD_URL
elif [ "$version" = latest ]; then
    base=https://github.com/$repo/releases/latest/download
else
    base=https://github.com/$repo/releases/download/$version
fi

fetch() {
    if command -v curl > /dev/null 2>&1; then
        curl -fsSL "$1" -o "$2"
    elif command -v wget > /dev/null 2>&1; then
        wget -q "$1" -O "$2"
    else
        fail "needs curl or wget"
    fi
}

sha256() {
    if command -v sha256sum > /dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum > /dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        fail "needs sha256sum or shasum to check the download"
    fi
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

echo "downloading $base/$asset"
fetch "$base/$asset" "$tmp/$asset" || fail "cannot download $base/$asset"
fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS" || fail "cannot download $base/SHA256SUMS"

expected=$(awk -v a="$asset" '$2 == a {print $1}' "$tmp/SHA256SUMS")
[ -n "$expected" ] || fail "SHA256SUMS has no entry for $asset"
actual=$(sha256 "$tmp/$asset")
[ "$expected" = "$actual" ] || fail "checksum mismatch for $asset: expected $expected, got $actual"

tar -xzf "$tmp/$asset" -C "$tmp"
[ -f "$tmp/nio" ] || fail "$asset holds no nio binary"

# A rename in the same directory replaces the file in one step, so a nio that
# is running now keeps its old binary.
mkdir -p "$dest"
cp "$tmp/nio" "$dest/nio.new"
chmod 755 "$dest/nio.new"
mv -f "$dest/nio.new" "$dest/nio"

echo "installed $("$dest/nio" version) at $dest/nio"

# nio compiles through clang. A machine without clang can install nio but
# cannot build anything.
if ! command -v clang > /dev/null 2>&1; then
    echo
    echo "nio needs clang on PATH to build programs, and there is none."
    case $os in
    darwin) echo "Install the Xcode command line tools:  xcode-select --install" ;;
    linux) echo "Install clang from your distribution, e.g.  sudo apt install clang  or  sudo dnf install clang" ;;
    esac
fi

case :$PATH: in
*":$dest:"*) ;;
*)
    echo
    echo "$dest is not on PATH. Add it to your shell's configuration:"
    case ${SHELL:-} in
    */fish) echo "  fish_add_path $dest" ;;
    */zsh) echo "  echo 'export PATH=\"$dest:\$PATH\"' >> ~/.zshrc" ;;
    */bash) echo "  echo 'export PATH=\"$dest:\$PATH\"' >> ~/.bashrc" ;;
    *) echo "  export PATH=\"$dest:\$PATH\"" ;;
    esac
    ;;
esac
