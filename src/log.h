#pragma once

#include <spdlog/spdlog.h>

namespace Log {

// Initialize the rotating file logger. Call once from InitThread.
// logPath: full path to the log file (e.g. next to the DLL)
// maxSizeMb: max file size before rotation (default 5 MB)
// maxFiles: number of rotated files to keep (default 3)
void Init(const char* logPath = nullptr, size_t maxSizeMb = 5, size_t maxFiles = 3);

// Shut down spdlog (flush + drop all loggers). Call on DLL_PROCESS_DETACH.
void Shutdown();

}  // namespace Log
