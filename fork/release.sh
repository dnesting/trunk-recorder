#!/bin/sh
# Build, test and push the image for HEAD, then tag the commit with the image
# tag so the deployed image always maps to a reachable commit.
#
#   fork/release.sh            # registry.lab.lan/trunk-recorder:trunkd-<YYYYMMDD>-<sha8>
#
# Requires a clean tree and HEAD pushed to origin. BUILDER, PLATFORM and
# IMAGE override the buildx builder (lab), platform (linux/amd64) and image
# repository (registry.lab.lan/trunk-recorder); MAKE_JOBS as in fork/test.sh.
set -eu
cd "$(dirname "$0")/.."

builder="${BUILDER:-lab}"
platform="${PLATFORM:-linux/amd64}"
image="${IMAGE:-registry.lab.lan/trunk-recorder}"

if [ -n "$(git status --porcelain)" ]; then
  echo "release: working tree is not clean" >&2
  exit 1
fi
sha="$(git rev-parse HEAD)"
if [ -z "$(git branch -r --contains "$sha" --list 'origin/*')" ]; then
  echo "release: HEAD $sha is not on any origin branch; push it first" >&2
  exit 1
fi

tag="trunkd-$(git show -s --format=%cd --date=format:%Y%m%d HEAD)-$(git rev-parse --short=8 HEAD)"

fork/test.sh
docker buildx build \
  --builder "$builder" \
  --platform "$platform" \
  --build-arg MAKE_JOBS="${MAKE_JOBS:-4}" \
  --tag "$image:$tag" \
  --push \
  --progress plain \
  .

git tag -a "$tag" -m "Image $image:$tag" HEAD
git push origin "refs/tags/$tag"
git push github "refs/tags/$tag" || echo "release: could not push tag to github" >&2

echo "$image:$tag"
docker buildx imagetools inspect "$image:$tag" --format '{{json .Manifest.Digest}}'
