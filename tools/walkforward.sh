#!/bin/sh
# Walk the recent history forward, one commit at a time. The compiler of each
# commit's predecessor builds that commit's compiler.
#
# This script checks the two-commit rule (bootstrap/README.md). A new language
# feature used inside the compiler costs two commits: add it, then use it.
# Stage 0 is a checked-in artifact that knows only the language of the src/ it
# came from. One commit that does both still passes tests/bootstrap_test.nio.
# But then no commit in history can produce the artifact, and a commit is no
# longer buildable by its predecessor. Walk-forward recovery needs that.
#
#   sh tools/walkforward.sh          # the last 20 commits
#   sh tools/walkforward.sh 40       # the last 40 commits
#
# Environment:
#   WF_DEPTH   how many commits to walk   (default 20, overridden by $1)
#   WF_WORK    where to build             (default a temp directory, removed)
#
# It needs clang and nothing else. The oldest commit's compiler comes from its
# own bootstrap/build.sh, as in a fresh clone.
set -eu

DEPTH="${1:-${WF_DEPTH:-20}}"
REPO=$(cd "$(dirname "$0")/.." && pwd)

case $DEPTH in
    '' | *[!0-9]*) echo "usage: sh tools/walkforward.sh [commits]" >&2; exit 2 ;;
esac

if [ -n "${WF_WORK:-}" ]; then
    WORK=$WF_WORK
    mkdir -p "$WORK"
    KEEP=1
else
    WORK=$(mktemp -d)
    KEEP=0
fi
cleanup() { [ "$KEEP" = 1 ] || rm -rf "$WORK"; }
trap cleanup EXIT

# --first-parent makes a merge one step. The order is oldest first, so each
# step takes the previous step as its input.
COMMITS=$(git -C "$REPO" rev-list --first-parent -n "$DEPTH" HEAD | sed '1!G;h;$!d')
COUNT=$(printf '%s\n' "$COMMITS" | wc -l | tr -d ' ')
if [ "$COUNT" -lt 2 ]; then
    echo "need at least two commits to walk, found $COUNT" >&2
    exit 2
fi

# Use a separate worktree. The walk checks out old trees, and in the repository
# that would destroy the caller's work.
TREE=$WORK/tree
git -C "$REPO" worktree add --detach -f "$TREE" "$(printf '%s\n' "$COMMITS" | head -1)" >/dev/null
release() {
    git -C "$REPO" worktree remove --force "$TREE" 2>/dev/null || true
    cleanup
}
trap release EXIT

FIRST=$(printf '%s\n' "$COMMITS" | head -1)
echo "== walking $COUNT commits, oldest first"
echo "== stage 0: $(git -C "$REPO" log -1 --format='%h %s' "$FIRST")"

# The oldest commit's compiler comes from its own checked-in IR and clang. This
# step needs no compiler.
( cd "$TREE" && sh bootstrap/build.sh "$WORK/nio.0" >/dev/null )
echo "   built from bootstrap/nio.ll with clang alone"

# BUILT_BY is the commit whose compiler is in $WORK/nio.$i. Usually it is the
# previous commit. After a failure the last good compiler moves forward, so the
# report must name the commit that built it.
BUILT_BY=$FIRST
i=0
FAILED=""
for sha in $COMMITS; do
    [ "$sha" = "$FIRST" ] && continue
    i=$((i + 1))
    desc=$(git -C "$REPO" log -1 --format='%h %s' "$sha")
    git -C "$TREE" checkout --detach -f "$sha" >/dev/null 2>&1

    # Build this commit's source with the predecessor's compiler. A failure
    # means that $sha uses a feature its parent's compiler does not implement,
    # and that $sha breaks the two-commit rule.
    if ( cd "$TREE" && "$WORK/nio.$((i - 1))" build src/nio.nio -o "$WORK/nio.$i" ) \
            >"$WORK/log.$i" 2>&1; then
        echo "ok   $desc"
        BUILT_BY=$sha
    else
        echo "FAIL $desc"
        echo "     built by $(git -C "$REPO" log -1 --format='%h' "$BUILT_BY")'s compiler:"
        sed 's/^/     /' "$WORK/log.$i"
        FAILED="$FAILED $(git -C "$REPO" log -1 --format='%h' "$sha")"
        # Move the last good compiler forward, and keep BUILT_BY on it.
        # Otherwise one bad commit makes every later commit fail too.
        cp "$WORK/nio.$((i - 1))" "$WORK/nio.$i"
    fi
done

if [ -n "$FAILED" ]; then
    echo
    echo "not buildable by their predecessor:$FAILED"
    echo "See the 'Using a new language feature inside the compiler' section of"
    echo "bootstrap/README.md -- each of these needs splitting into two commits."
    exit 1
fi

echo
echo "all $i steps built by their predecessor"
