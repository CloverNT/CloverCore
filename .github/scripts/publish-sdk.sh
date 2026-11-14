#!/usr/bin/env bash
set -euo pipefail

: "${SDK_REMOTE:?SDK_REMOTE is not set}"
: "${SDK_BRANCH:?SDK_BRANCH is not set}"
: "${SDK_PKG_DIR:?SDK_PKG_DIR is not set}"
: "${SDK_COMMIT_MESSAGE:?SDK_COMMIT_MESSAGE is not set}"
SDK_TAG="${SDK_TAG:-}"

pkg="$(cd "$SDK_PKG_DIR" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

remote_has() {
    local rc=0
    git ls-remote --exit-code "$1" origin "$2" >/dev/null 2>&1 || rc=$?
    case $rc in
        0) return 0 ;;
        2) return 1 ;;
        *) echo "git ls-remote origin $2 failed ($rc)" >&2; exit "$rc" ;;
    esac
}

git clone --quiet "$SDK_REMOTE" "$work/repo"
cd "$work/repo"
git config user.name  "${SDK_GIT_NAME:-github-actions[bot]}"
git config user.email "${SDK_GIT_EMAIL:-41898282+github-actions[bot]@users.noreply.github.com}"

if remote_has --heads "refs/heads/$SDK_BRANCH"; then
    git checkout --quiet -B "$SDK_BRANCH" "origin/$SDK_BRANCH"
else
    echo "branch '$SDK_BRANCH' does not exist yet, creating it"
    git checkout --quiet --orphan "$SDK_BRANCH"
fi

find . -mindepth 1 -maxdepth 1 ! -name .git ! -name README.md -exec rm -rf {} +
cp -a "$pkg"/. .
git add -A

if [ -n "$(git status --porcelain)" ]; then
    git -c commit.gpgSign=false commit --quiet -m "$SDK_COMMIT_MESSAGE"
    git push --quiet origin "HEAD:refs/heads/$SDK_BRANCH"
    echo "pushed $(git rev-parse --short HEAD) to $SDK_BRANCH"
else
    echo "the SDK package did not change, nothing to commit"
fi

if [ -n "$SDK_TAG" ]; then
    if remote_has --tags "refs/tags/$SDK_TAG"; then
        echo "tag $SDK_TAG already exists in the SDK repository, leaving it alone"
    else
        git -c tag.gpgSign=false tag "$SDK_TAG"
        git push --quiet origin "refs/tags/$SDK_TAG"
        echo "tagged $SDK_TAG"
    fi
fi
