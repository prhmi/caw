#!/usr/bin/env bash
# CAW - build CAW's own SuperCollider runtime (see supercollider/). Wrapper so all engines live in tools/.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
exec "$here/../supercollider/build-libscsynth.sh" "$@"
