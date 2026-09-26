#!/bin/sh
# Cut a release: choose the version, bump it, check the tree, then commit,
# tag and push. The pushed tag starts .github/workflows/release.yml, which
# builds the macOS, Linux and Windows binaries and publishes the release in
# the public repository nio-org/nio.
#
#	sh tools/release.sh
#
# It asks before every step that changes something outside the working tree.
# Nothing is pushed until the last question. When a check fails, the working
# tree keeps the bump, and `git checkout -- .` undoes it.
set -eu

# Where the workflow publishes; RELEASE_REPO in .github/workflows/release.yml.
release_repo=nio-org/nio

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
fail() {
    printf '\033[31mrelease: %s\033[0m\n' "$*" >&2
    exit 1
}

# ask "question" default  -> 0 for yes, 1 for no
ask() {
    case $2 in y) hint="[Y/n]" ;; *) hint="[y/N]" ;; esac
    printf '%s %s ' "$1" "$hint"
    read -r reply <&3 || reply=
    case ${reply:-$2} in
    y | Y | yes) return 0 ;;
    *) return 1 ;;
    esac
}

# subst file old new: replace a literal string, and fail when it is not there.
subst() {
    grep -qF -- "$2" "$1" || fail "$1 no longer contains '$2'; update tools/release.sh"
    OLD=$2 NEW=$3 perl -pi -e 's/\Q$ENV{OLD}\E/$ENV{NEW}/g' "$1"
}

[ -t 0 ] || fail "this script is interactive; run it from a terminal"

# Answers are read from fd 3, and every other command reads nothing, so no
# command run between two questions can take what was typed for the next.
exec 3<&0 0< /dev/null

# ---- preflight ----

say "Checking the repository"
branch=$(git rev-parse --abbrev-ref HEAD)
[ "$branch" = main ] || fail "on branch '$branch'; releases are cut from main"
[ -z "$(git status --porcelain)" ] || fail "the working tree has changes; commit or stash them first"
git ls-files --error-unmatch .github/workflows/release.yml > /dev/null 2>&1 ||
    fail ".github/workflows/release.yml is not committed, so a tag would publish nothing"

git fetch --quiet --tags origin
behind=$(git rev-list --count HEAD..origin/main)
[ "$behind" = 0 ] || fail "main is $behind commit(s) behind origin/main; pull first"
ahead=$(git rev-list --count origin/main..HEAD)
if [ "$ahead" != 0 ]; then
    echo "main is $ahead commit(s) ahead of origin/main; the push will include them:"
    git log --oneline origin/main..HEAD
fi
command -v clang > /dev/null 2>&1 || fail "clang is not on PATH"

current=$(sed -n 's/.*VERSION = "\([0-9]*\.[0-9]*\.[0-9]*\)".*/\1/p' src/version.nio)
[ -n "$current" ] || fail "cannot read the version from src/version.nio"
last_tag=$(git describe --tags --abbrev=0 --match 'v*' 2>/dev/null || true)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

# A compiler built from this tree, in a directory of its own, so that no step
# below overwrites a binary that is running.
say "Building a compiler from src/"
if [ -x ./nio ]; then
    seed=./nio
elif command -v nio > /dev/null 2>&1; then
    seed=$(command -v nio)
else
    sh bootstrap/build.sh "$work/nio0"
    seed=$work/nio0
fi
"$seed" build src/nio.nio -o "$work/nio"
NIO=$work/nio

# ---- the version ----

tag_exists() {
    git rev-parse -q --verify "refs/tags/v$1" > /dev/null ||
        git ls-remote --exit-code --tags origin "refs/tags/v$1" > /dev/null 2>&1
}

IFS=. read -r major minor patch <<EOF
$current
EOF
patch_v=$major.$minor.$((patch + 1))
minor_v=$major.$((minor + 1)).0
major_v=$((major + 1)).0.0

say "Choosing the version"
echo "src/version.nio says $current; the last tag is ${last_tag:-(none)}."
default=1
if ! tag_exists "$current"; then
    echo "  0) $current   (release the version as it is; it has no tag yet)"
    default=0
fi
echo "  1) $patch_v   (patch)"
echo "  2) $minor_v   (minor)"
echo "  3) $major_v   (major)"
echo "  or type a version, X.Y.Z"
printf 'Version [%s]: ' "$default"
read -r choice <&3 || choice=
case ${choice:-$default} in
0) version=$current ;;
1) version=$patch_v ;;
2) version=$minor_v ;;
3) version=$major_v ;;
*) version=${choice#v} ;;
esac
echo "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || fail "'$version' is not X.Y.Z"
if tag_exists "$version"; then
    fail "v$version is already tagged"
fi
tag=v$version

# ---- the bump ----

if [ "$version" != "$current" ]; then
    say "Moving $current to $version"
    subst src/version.nio "VERSION = \"$current\"" "VERSION = \"$version\""
    subst README.md "current version is **$current**" "current version is **$version**"
    subst README.md "badge/version-$current-" "badge/version-$version-"
fi

# The newest `## ` heading must be this version (tests/version_test.nio), and
# its body becomes the release notes.
if ! grep -qx "## $version" CHANGELOG.md; then
    say "Writing the CHANGELOG.md entry"
    {
        echo "## $version"
        echo
        if [ -n "$last_tag" ]; then
            echo "Commits since $last_tag, to be rewritten as what a user would notice:"
            echo
            git log --no-merges --format='- %s' "$last_tag..HEAD"
        else
            echo "What a user would notice in this release."
        fi
        echo
    } > "$work/entry.md"
    awk -v entry="$work/entry.md" '
        !done && /^## / { while ((getline l < entry) > 0) print l; done = 1 }
        { print }
    ' CHANGELOG.md > "$work/CHANGELOG.md"
    mv "$work/CHANGELOG.md" CHANGELOG.md
    echo "Opening CHANGELOG.md with a drafted entry. Save and close the editor to continue."
    ask "Open the editor?" y || fail "CHANGELOG.md needs an entry for $version"
    ${VISUAL:-${EDITOR:-vi}} CHANGELOG.md <&3 > /dev/tty
fi
newest=$(grep -m1 '^## ' CHANGELOG.md)
[ "$newest" = "## $version" ] || fail "the newest CHANGELOG.md entry is '$newest', not '## $version'"
body=$(awk -v h="## $version" '$0 == h { on = 1; next } on && /^## / { exit } on && NF { print }' CHANGELOG.md)
[ -n "$body" ] || fail "the CHANGELOG.md entry for $version is empty"

if [ "$version" != "$current" ]; then
    say "Re-bootstrapping (src/version.nio changed)"
    "$NIO" run tools/bootstrap.nio
fi

# ---- checks ----

say "Checking formatting"
"$NIO" format -l . || fail "files above are not formatted; run: nio format ."

# suite file: run it, print its last line, and on failure the failures.
suite() {
    if ! "$NIO" run "$1" > "$work/suite.log" 2>&1; then
        grep -v '^ok ' "$work/suite.log" | tail -40
        fail "$1 failed"
    fi
    tail -1 "$work/suite.log"
}

say "Running the fast suite"
suite tests/unit_main.nio

if ask "Run the full suite too (about 5 minutes)?" y; then
    suite tests/all.nio
fi

say "Building the release compiler for this host"
mkdir "$work/release"
"$NIO" build --release src/nio.nio -o "$work/release/nio"
reported=$("$work/release/nio" version)
echo "$reported"
case $reported in
"nio $version "*) ;;
*) fail "the release build reports '$reported', not $version" ;;
esac

# ---- publish ----

say "Ready to release $tag"
git status --short
echo
echo "Release notes (the CHANGELOG.md entry):"
echo "$body" | sed 's/^/    /' | head -20
echo

if ! ask "Commit, tag $tag and push both to origin?" n; then
    echo "Stopped before committing. The working tree keeps the changes; 'git checkout -- .' undoes them."
    exit 0
fi

git add -A
if ! git diff --cached --quiet; then
    git commit --quiet -m "Release $tag"
fi
git tag -a "$tag" -m "Nio $version"
git push --atomic origin main "$tag"

# The local compiler reports the new version too. mv renames, so a running
# nio keeps its old file.
mv -f "$work/release/nio" ./nio

say "Pushed $tag"
url=$(git remote get-url origin)
case $url in
https://github.com/* | git@github.com:*) ;;
*) exit 0 ;;
esac
slug=$(echo "$url" | sed -E 's#^(https://github\.com/|git@github\.com:)##; s#\.git$##')
echo "The release workflow is building the binaries:"
echo "  https://github.com/$slug/actions/workflows/release.yml"
echo "The release will be at:"
echo "  https://github.com/$release_repo/releases/tag/$tag"

if command -v gh > /dev/null 2>&1 && ask "Watch the workflow here?" y; then
    sleep 5
    run=$(gh run list --workflow release.yml --branch "$tag" --limit 1 --json databaseId -q '.[0].databaseId')
    if [ -n "$run" ]; then
        gh run watch "$run" --exit-status && gh release view "$tag" --repo "$release_repo" --web
    else
        echo "The run has not appeared yet; open the link above."
    fi
fi
