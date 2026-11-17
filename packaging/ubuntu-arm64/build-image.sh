#!/usr/bin/env bash
set -euo pipefail

src=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
version=${TD_VERSION:-3.4.3.9}
image=${1:-"tdengine-community:$version-ubuntu24.04-arm64"}
engine=${CONTAINER_ENGINE:-docker}

if [[ $image == -h || $image == --help ]]; then
  printf '%s\n' 'Usage: build-image.sh [image:tag]' \
    'Environment: CONTAINER_ENGINE=docker|podman TD_VERSION=3.4.3.9 TD_JOBS=4 TD_BUILD_ROCKSDB=OFF'
  exit 0
fi

case $engine in
  docker) build=(docker buildx build --load) ;;
  podman) build=(podman build) ;;
  *) printf '%s\n' 'CONTAINER_ENGINE must be docker or podman.' >&2; exit 1 ;;
esac

context=$(mktemp -d)
trap 'rm -rf -- "$context"' EXIT
# Copy working-tree sources, including hydrated binary dependencies and any
# local edits. Keep build caches and Git history out of the build context.
git -C "$src" ls-files -z --cached --others --exclude-standard --deduplicate |
  while IFS= read -r -d '' path; do
    if [[ -f "$src/$path" || -L "$src/$path" ]]; then
      printf '%s\0' "$path"
    fi
  done |
  tar -C "$src" --null --no-recursion -T - -cf - |
  tar -C "$context" -xf -

"${build[@]}" --platform linux/arm64 \
  --file "$context/packaging/ubuntu-arm64/Dockerfile" \
  --build-arg "TD_VERSION=$version" \
  --build-arg "TD_JOBS=${TD_JOBS:-4}" \
  --build-arg "TD_BUILD_ROCKSDB=${TD_BUILD_ROCKSDB:-OFF}" \
  --label "org.opencontainers.image.revision=$(git -C "$src" rev-parse HEAD)" \
  --tag "$image" "$context"

test "$("$engine" image inspect --format '{{.Os}}/{{.Architecture}}' "$image")" = linux/arm64
printf 'Image: %s (linux/arm64)\n' "$image"
