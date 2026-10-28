# Browser smoke test for aurora's web backend: builds aurora (aurora-main, with
# runtime/web/aurora/aurora-web.patch applied) for Emscripten plus a small GX app, which
# run-smoke.mjs renders in headless Chromium and checks.
#
#   runtime/web/apply-aurora-patch.sh
#   emcmake cmake -S runtime/web/smoke -B build-web-smoke -G Ninja [-DMKW_WEB_DEPS_DIR=...]
#   cmake --build build-web-smoke
#   node runtime/web/smoke/run-smoke.mjs build-web-smoke
cmake_minimum_required(VERSION 3.25)
project(mkw_web_smoke C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "The web smoke test builds with Emscripten (emcmake cmake ...)")
endif()

# The same code-generation flags as the runtime's web build (runtime/CMakeLists.txt).
add_compile_options(-msimd128 -mno-nontrapping-fptoint -fwasm-exceptions -pthread)
add_link_options(-fwasm-exceptions -pthread)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/AuroraWebOptions.cmake")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../../aurora-main" aurora EXCLUDE_FROM_ALL)

add_executable(aurora_smoke aurora_smoke.cpp)
target_compile_features(aurora_smoke PRIVATE cxx_std_20)
target_link_libraries(aurora_smoke PRIVATE aurora::core aurora::gx aurora::main aurora::vi)
# JSPI: main() runs on a suspendable stack so presenting can yield to the browser each frame.
target_link_options(aurora_smoke PRIVATE
    -sJSPI -sPTHREAD_POOL_SIZE=4 -sINITIAL_MEMORY=512MB -sSTACK_SIZE=4MB -sASSERTIONS=1 -g2
    -sALLOW_BLOCKING_ON_MAIN_THREAD=0
    "-sEXPORTED_RUNTIME_METHODS=['ccall']")
configure_file(aurora_smoke.html "${CMAKE_CURRENT_BINARY_DIR}/aurora_smoke.html" COPYONLY)
