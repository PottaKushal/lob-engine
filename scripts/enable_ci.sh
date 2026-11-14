#!/usr/bin/env bash
# One-time step: push the GitHub Actions workflows and switch Pages to the
# workflow build. Needs a token with the `workflow` scope:
#
#   gh auth refresh -h github.com -s workflow
#   scripts/enable_ci.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ls .github/workflows/*.yml >/dev/null
# Workflows were kept out of git locally until the token could push them.
if [[ -f .git/info/exclude ]]; then
  grep -v '^\.github/workflows/$' .git/info/exclude > .git/info/exclude.tmp || true
  mv .git/info/exclude.tmp .git/info/exclude
fi
git add .github/workflows
git commit -m "Add CI: build matrix, sanitizers, fuzzing, benchmark artifact, CodeQL, Pages deploy"
git push origin main
REPO=$(gh repo view --json nameWithOwner -q .nameWithOwner)
gh api -X PUT "repos/$REPO/pages" -f build_type=workflow >/dev/null
echo "Workflows pushed. Pages now deploys from the Pages workflow: https://github.com/$REPO/actions"
