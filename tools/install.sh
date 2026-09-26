#!/bin/sh
# Build the nio compiler and install it onto the zsh PATH (macOS).
#
#	sh tools/install.sh [install-dir]      (default: ~/.local/bin)
#
# Needs only clang. If a working `nio` is already available (repo root or
# PATH) it rebuilds with that; otherwise it bootstraps stage 0 from the
# checked-in bootstrap/nio.ll first. The compiler is always rebuilt from
# src/, so the installed binary reflects the working tree.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
bindir=${1:-"$HOME/.local/bin"}
zshrc="$HOME/.zshrc"

command -v clang >/dev/null 2>&1 || {
    echo "error: clang not found in PATH (install Xcode command line tools: xcode-select --install)" >&2
    exit 1
}

# Pick a compiler to build with: the repo's own binary, one on PATH, or
# stage 0 bootstrapped into a temp dir.
tmpdir=
cleanup() { if [ -n "$tmpdir" ]; then rm -rf "$tmpdir"; fi; }
trap cleanup EXIT

if [ -x "$root/nio" ]; then
    nio="$root/nio"
elif command -v nio >/dev/null 2>&1; then
    nio=$(command -v nio)
else
    echo "==> No nio found; bootstrapping stage 0 from bootstrap/nio.ll (clang only)..."
    tmpdir=$(mktemp -d)
    sh "$root/bootstrap/build.sh" "$tmpdir/nio"
    nio="$tmpdir/nio"
fi

# Build under the leaf name `nio`, then move it into place. On macOS the leaf
# name at link time goes into the Mach-O UUID, and every other stage links
# under the same name.
echo "==> Building nio from src/ with $nio ..."
builddir=$(mktemp -d)
trap 'cleanup; rm -rf "$builddir"' EXIT
"$nio" build --release "$root/src/nio.nio" -o "$builddir/nio"

mkdir -p "$bindir"
mv -f "$builddir/nio" "$bindir/nio"
echo "==> Installed $bindir/nio"

# Put the install dir on the zsh PATH if it is not already reachable.
case ":$PATH:" in
*":$bindir:"*)
    echo "==> $bindir is already on your PATH"
    ;;
*)
    line="export PATH=\"$bindir:\$PATH\""
    if [ -f "$zshrc" ] && grep -Fqs "$line" "$zshrc"; then
        echo "==> $zshrc already adds $bindir (open a new shell to pick it up)"
    else
        printf '\n# Added by nio tools/install.sh\n%s\n' "$line" >>"$zshrc"
        echo "==> Added $bindir to PATH in $zshrc"
    fi
    echo "    Run 'source ~/.zshrc' or open a new terminal, then: nio --help"
    ;;
esac

echo "Done."
