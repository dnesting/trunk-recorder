#!/bin/sh
# Build the fork and run its offline tests in Docker (the Dockerfile's
# `test` stage). Usage: fork/test.sh [extra docker buildx build args]
# BUILDER selects the buildx builder (default: lab); PLATFORM the platform
# (default: linux/amd64, what the cluster runs); MAKE_JOBS the compile
# parallelism (default 4: more has OOM-killed the lab buildkitd).
set -eu
cd "$(dirname "$0")/.."
exec docker buildx build \
  --builder "${BUILDER:-lab}" \
  --platform "${PLATFORM:-linux/amd64}" \
  --build-arg MAKE_JOBS="${MAKE_JOBS:-4}" \
  --target test \
  --progress plain \
  "$@" .
