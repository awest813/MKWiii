# aurora configuration for the web build (Emscripten). Include before add_subdirectory(aurora-main).
#
# Matches what runtime/CMakeLists.txt uses natively (the runtime implements the Wii SDK itself, so
# aurora stays on its GameCube SDK with DVD/CARD/THP/RmlUi off) plus the web-specific choices:
# no zstd-compressed on-disk cache, static libraries only (Emscripten has no shared libraries in
# this sense), and SDL3 built from source (it has a native Emscripten backend).
#
# MKW_WEB_DEPS_DIR, when set, points at a directory of pre-fetched dependency checkouts (one per
# FetchContent name below, at the versions aurora pins) for hosts that cannot download the pinned
# GitHub archives; runtime/web/fetch-deps.sh creates one with git.
set(AURORA_TARGET_RVL OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AURORA_CACHE_USE_ZSTD OFF CACHE BOOL "" FORCE)
set(AURORA_USE_SYSTEM_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(AURORA_SDL3_PROVIDER vendor CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)

set(MKW_WEB_DEPS_DIR "" CACHE PATH "Pre-fetched aurora dependency checkouts (see runtime/web/fetch-deps.sh)")
if(MKW_WEB_DEPS_DIR)
    foreach(dep IN ITEMS SDL:sdl3 ABSEIL-CPP:abseil-cpp XXHASH:xxhash FMT:fmt ZLIB:zlib-ng PNG:libpng
                         TRACY:tracy IMGUI:imgui)
        string(REPLACE ":" ";" dep "${dep}")
        list(GET dep 0 name)
        list(GET dep 1 dir)
        if(EXISTS "${MKW_WEB_DEPS_DIR}/${dir}")
            set(FETCHCONTENT_SOURCE_DIR_${name} "${MKW_WEB_DEPS_DIR}/${dir}" CACHE PATH "" FORCE)
        endif()
    endforeach()
endif()
