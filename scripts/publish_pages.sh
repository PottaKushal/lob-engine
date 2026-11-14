#!/usr/bin/env bash
# Manual fallback for publishing the demo: pushes _site/ to the gh-pages branch.
# The normal route is the Pages workflow (.github/workflows/pages.yml); use this
# only where Actions cannot deploy. Appends a commit; never force-pushes.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ -f _site/index.html ]] || { echo "run scripts/build_site.sh first"; exit 1; }
URL=$(git remote get-url origin)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if git ls-remote --exit-code --heads origin gh-pages >/dev/null 2>&1; then
  git clone --quiet --depth 1 --branch gh-pages "$URL" "$TMP/site"
  find "$TMP/site" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
else
  git init --quiet -b gh-pages "$TMP/site"
  git -C "$TMP/site" remote add origin "$URL"
fi
cp -R _site/. "$TMP/site/"
touch "$TMP/site/.nojekyll"
git -C "$TMP/site" add -A
git -C "$TMP/site" -c user.name="$(git config user.name)" -c user.email="$(git config user.email)" \
  commit --quiet -m "Publish demo site built from $(git rev-parse --short HEAD)"
git -C "$TMP/site" push --quiet origin gh-pages
echo "published to gh-pages"
