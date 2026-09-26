#!/bin/sh
# Copies editors/shared/ into each editor's own tree. VS Code cannot read a
# grammar from outside the extension directory it packages, so every editor
# gets a copy rather than a reference.
#
#   ./sync.sh            refresh every editor's copy
#   ./sync.sh --check    exit non-zero if any copy is stale
#
# Add a new editor by adding one `sync_one` line at the bottom.

set -e
cd "$(dirname "$0")"

check=no
if [ "$1" = "--check" ]; then
    check=yes
fi

# sync_one <name under shared/> <destination path>
sync_one() {
    src="shared/$1"
    dst="$2"
    if [ "$check" = yes ]; then
        if ! diff -q "$src" "$dst" >/dev/null 2>&1; then
            echo "stale: $dst does not match $src" >&2
            return 1
        fi
        return 0
    fi
    mkdir -p "$(dirname "$dst")"
    cp "$src" "$dst"
    echo "synced $dst"
}

rc=0
sync_one nio.tmLanguage.json         vscode/syntaxes/nio.tmLanguage.json   || rc=1
sync_one language-configuration.json vscode/language-configuration.json    || rc=1

if [ "$rc" != 0 ]; then
    echo "run editors/sync.sh to refresh" >&2
    exit 1
fi

if [ "$check" = yes ]; then
    echo "editors/shared is in sync"
fi
