// Clears the screen to a known color through GX every frame and reports frame progress to the
// page, so run-smoke.mjs can check that aurora initializes WebGPU, presents frames on the canvas,
// and keeps yielding to the browser.
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/gx.h>

#include <emscripten.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <typeinfo>

namespace {
// What run-smoke.mjs expects to find in the middle of the canvas.
constexpr GXColor kClearColor{.r = 32, .g = 160, .b = 96, .a = 255};
constexpr int kFrames = 120;

void Log(AuroraLogLevel level, const char* module, const char* message, unsigned int) {
    static const char* const kNames[] = {"DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
    std::fprintf(level >= LOG_ERROR ? stderr : stdout, "[%s: %s; %s]\n", kNames[level], module, message);
    if (level == LOG_FATAL) {
        std::fflush(stdout);
        std::abort();
    }
}

EM_JS(void, ReportFrame, (int frame), { globalThis.mkwSmokeFrames = frame; });
EM_JS(void, ReportDone, (int status), { globalThis.mkwSmokeStatus = status; });

int Run(int argc, char** argv) {
    // Headless and CI browsers only offer a software (SwiftShader) adapter.
    const AuroraConfig config{.appName = "aurora_smoke", .allowCpuAdapter = true, .logCallback = &Log};
    aurora_initialize(argc, argv, &config);
    for (int frame = 1; frame <= kFrames; ++frame) {
        for (const AuroraEvent* event = aurora_update(); event && event->type != AURORA_NONE; ++event) {
            if (event->type == AURORA_EXIT) return 0;
        }
        if (!aurora_begin_frame()) continue;
        GXSetCopyClear(kClearColor, GX_MAX_Z24);
        aurora_end_frame();
        ReportFrame(frame);
        if (frame % 30 == 0) std::printf("aurora_smoke: frame %d\n", frame);
    }
    // Leave the last frame on screen for the screenshot.
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    int status = 1;
    try {
        status = Run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "aurora_smoke: uncaught %s: %s\n", typeid(e).name(), e.what());
    } catch (...) {
        std::fprintf(stderr, "aurora_smoke: uncaught exception\n");
    }
    ReportDone(status);
    return status;
}
