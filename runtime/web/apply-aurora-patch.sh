#!/usr/bin/env bash
# Applies runtime/web/aurora/aurora-web.patch (aurora's web backend) to the aurora-main submodule.
# Idempotent: does nothing when the patch is already applied. The patch is meant to be upstreamed to
# the aurora fork behind its __EMSCRIPTEN__ / WEBGPU_DAWN switches; native builds are unaffected by
# it (every change is either web-only or a comment), so applying it to a native checkout is harmless.
#
# It was made against the submodule commit recorded in this repository (git submodule status).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
patch="$root/runtime/web/aurora/aurora-web.patch"
cd "$root/aurora-main"
if git apply --reverse --check "$patch" 2>/dev/null; then
    echo "apply-aurora-patch: already applied"
    exit 0
fi
git apply "$patch"
echo "apply-aurora-patch: applied"
