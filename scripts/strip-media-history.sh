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
# Requires git-filter-repo (pip install git-filter-repo, or brew install git-filter-repo).

set -euo pipefail

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

before=$(git count-objects -vH | awk '/size-pack/ {print $2 " " $3}')

# --invert-paths drops the listed paths from every commit. The media-only
# pages that referenced them were removed from the tree separately; their
# history can stay, it is small.
git filter-repo --force \
    --path docs/images \
    --path docs/contra-gallery-metrics.json \
    --path docs/gpu-motion-metrics.json \
    --path docs/preset-audit-4k.json \
    --path docs/showcase-captures.json \
    --invert-paths

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
