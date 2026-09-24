#!/usr/bin/env bash
# strip-media-history.sh -- remove docs/images from the entire git history.
#
# The showcase images and videos were committed straight into this repository,
# several of them more than once, so a fresh clone downloads over 600 MB of
# media before a single line of C. The media now lives in the website
# repository (mynes-web). This script rewrites history so the blobs are gone
# for good and the clone shrinks to a few tens of megabytes.
#
# THIS REWRITES EVERY COMMIT HASH AND REQUIRES A FORCE PUSH. Anyone with an
# existing clone or fork must re-clone (or rebase onto the new history).
# Run it once, on a fresh clone of the repository, after the branch that
# removes docs/images from the working tree has been merged into master.
#
# Usage:
#   git clone https://github.com/yaglo/mynes.git mynes-rewrite
#   cd mynes-rewrite
#   ./scripts/strip-media-history.sh --yes
#   git push --force --all origin
#   git push --force --tags origin
#
# The script refuses to run while HEAD still tracks any of the paths it
# removes, and leaves git-filter-repo's own fresh-clone check on. Set
# MYNES_STRIP_FORCE=1 to pass --force to git-filter-repo only if that check
# rejects a clone you know is fresh.
#
# Requires git-filter-repo (pip install git-filter-repo, or brew install git-filter-repo).

set -euo pipefail

# The paths dropped from every commit. They must already be gone from the
# tip: the rewrite would otherwise delete files the tree still uses.
MEDIA_PATHS=(
    docs/images
    docs/contra-gallery-metrics.json
    docs/gpu-motion-metrics.json
    docs/preset-audit-4k.json
    docs/showcase-captures.json
)

if [[ "${1:-}" != "--yes" ]]; then
    echo "This rewrites all history. Read the header of this script, then rerun with --yes." >&2
    exit 1
fi

if ! git filter-repo --version >/dev/null 2>&1; then
    echo "git-filter-repo is not installed: pip install git-filter-repo" >&2
    exit 1
fi

if [[ -n "$(git status --porcelain)" ]]; then
    echo "Working tree is not clean; commit or stash first." >&2
    exit 1
fi

still_tracked=$(git ls-tree -r --name-only HEAD -- "${MEDIA_PATHS[@]}")
if [[ -n "$still_tracked" ]]; then
    count=$(printf '%s\n' "$still_tracked" | wc -l | tr -d ' ')
    echo "HEAD still tracks $count file(s) this script would delete from history:" >&2
    # sed rather than head: head closing the pipe early would kill printf
    # with SIGPIPE and, under pipefail, the script before its last message.
    printf '%s\n' "$still_tracked" | sed -n '1,20s/^/  /p' >&2
    if [[ $count -gt 20 ]]; then echo "  ..." >&2; fi
    echo "Merge the branch that removes them from the tree first (see the header)." >&2
    exit 1
fi

# git-filter-repo refuses to run outside a fresh clone unless forced. That
# check is the last guard against rewriting a working repository, so it
# stays on unless explicitly waived.
force=()
if [[ "${MYNES_STRIP_FORCE:-}" == "1" ]]; then
    force=(--force)
fi

before=$(git count-objects -vH | awk '/size-pack/ {print $2 " " $3}')

# --invert-paths drops the listed paths from every commit. The media-only
# pages that referenced them were removed from the tree separately; their
# history can stay, it is small.
path_args=()
for path in "${MEDIA_PATHS[@]}"; do
    path_args+=(--path "$path")
done
# ${force[@]+...} keeps an empty array legal under set -u in bash 3.2 (macOS).
git filter-repo ${force[@]+"${force[@]}"} "${path_args[@]}" --invert-paths

# filter-repo removes the origin remote on purpose; put it back so the
# force push in the usage notes works.
git remote add origin https://github.com/yaglo/mynes.git 2>/dev/null || true

git reflog expire --expire=now --all
git gc --prune=now --aggressive

after=$(git count-objects -vH | awk '/size-pack/ {print $2 " " $3}')
echo "Pack size before: $before"
echo "Pack size after:  $after"
echo
echo "Now inspect 'git log --stat | head' and, when satisfied:"
echo "  git push --force --all origin && git push --force --tags origin"
