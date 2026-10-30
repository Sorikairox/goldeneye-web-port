#!/bin/sh
# Back up the work to the user's PRIVATE repo (origin = Sorikairox/goldeneye-web-port, created 2026-10-30).
# Never pushes to upstream (jkdansereau/goldeneye-pc-port; its push URL is disabled too). Uses gh's credentials
# without changing global git config. Pushes web and every web-* session branch, and warns about any web-* branch
# with commits web lacks (merge it into web).
set -e
cd "$(dirname "$0")/../.."
url=$(git remote get-url origin)
case "$url" in *Sorikairox/goldeneye-web-port*) ;; *) echo "origin is not the private backup repo: $url" >&2; exit 1;; esac
branches=$(git for-each-ref --format='%(refname:short)' 'refs/heads/web' 'refs/heads/web-*')
for b in $branches; do
  n=$(git rev-list --count "web..$b")
  [ "$n" = 0 ] || echo "note: $b has $n commits web lacks" >&2
done
git -c credential.helper= -c credential.helper='!gh auth git-credential' push --quiet origin $branches 2>&1 | tail -3
echo "backup push done: $(date '+%F %T')"
