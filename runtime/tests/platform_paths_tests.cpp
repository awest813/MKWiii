#include "platform/host_platform.h"

#include <iostream>

int main() {
    if (!RuntimePlatform::ExecutableDirectory()) {
        std::cerr << "unable to resolve the current executable directory\n";
        return 1;
    }

    const auto userData = RuntimePlatform::ApplicationDataDirectory("WiiCompiledPlatformPathsTest");
    if (userData.filename() != "WiiCompiledPlatformPathsTest") {
        std::cerr << "application-data directory lost its application name: " << userData << '\n';
        return 1;
    }
    if (RuntimePlatform::LogDirectory("WiiCompiledPlatformPathsTest") != userData / "Logs") {
        std::cerr << "log directory is not derived from application data\n";
        return 1;
    }
#if defined(__EMSCRIPTEN__)
    if (*RuntimePlatform::ExecutableDirectory() != RuntimePlatform::kWebAppRoot ||
        userData.parent_path() != RuntimePlatform::kWebUserDataRoot) {
        std::cerr << "web paths left the fixed virtual layout: " << userData << '\n';
        return 1;
    }
#elif defined(__APPLE__)
    if (userData.parent_path().filename() != "Application Support" ||
        userData.parent_path().parent_path().filename() != "Library") {
        std::cerr << "macOS application-data directory is not under ~/Library/Application Support: "
                  << userData << '\n';
        return 1;
    }
#endif

    return 0;
}
