#!/usr/bin/env bash
# Clones aurora's web-build dependencies, at the versions aurora pins, into a directory for
# MKW_WEB_DEPS_DIR (runtime/web/cmake/AuroraWebOptions.cmake). Only needed where the pinned GitHub
# release archives cannot be downloaded; git checkouts of the same tags are used instead.
#
#   runtime/web/fetch-deps.sh <dir>
#
# Keep the tags in step with aurora-main/cmake/AuroraDependencyVersions.cmake,
# aurora-main/extern/CMakeLists.txt and aurora-main/cmake/AuroraDearImguiProvider.cmake.
set -euo pipefail
dir="${1:?usage: fetch-deps.sh <dir>}"
mkdir -p "$dir"
clone() {
    local name="$1" url="$2" tag="$3"
    if [[ -d "$dir/$name/.git" ]]; then
        echo "fetch-deps: $name already present"
        return
    fi
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" "$url" "$dir/$name"
    echo "fetch-deps: $name $tag"
}
clone sdl3       https://github.com/libsdl-org/SDL.git        release-3.4.10
clone abseil-cpp https://github.com/abseil/abseil-cpp.git     20240722.0
clone xxhash     https://github.com/Cyan4973/xxHash.git       v0.8.4
clone fmt        https://github.com/fmtlib/fmt.git            12.1.0
clone zlib-ng    https://github.com/zlib-ng/zlib-ng.git       2.3.3
clone libpng     https://github.com/pnggroup/libpng.git       v1.6.58
clone tracy      https://github.com/wolfpld/tracy.git         v0.14.1
clone imgui      https://github.com/ocornut/imgui.git         v1.91.9b-docking
