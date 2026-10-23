#!/bin/sh
# Uses the SDK already installed in ps5-payload-sdk-pldmgr.
# Optional override: tools/build-elf-docker.sh /path/to/sdk [image]
set -eu
SDK="${1:-}"
IMAGE="${2:-ps5-payload-sdk-pldmgr}"
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
# A worktree's .git points outside the Docker bind mount.
BUILD_REF="$(GIT_MASTER=1 git -C "$PROJ" describe --always --dirty)"
if [ -n "$SDK" ]; then
    SDK="$(cd "$SDK" && pwd)"
    set -- -v "$SDK:/opt/ps5-payload-sdk:ro"
else
    set --
fi
docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" \
    -e PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk -e BUILD_REF="$BUILD_REF" \
    -e WHATEVERPAD_PROBE="${WHATEVERPAD_PROBE:-0}" \
    -v "$PROJ":/src -w /src "$@" "$IMAGE" sh -ec '
    test -f "$PS5_PAYLOAD_SDK/toolchain/prospero.mk" || {
        echo "PS5 payload SDK missing at $PS5_PAYLOAD_SDK" >&2
        exit 1
    }
    exec make ps5
'
