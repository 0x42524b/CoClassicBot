#include "log.h"

#include <windows.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <string>

extern HMODULE g_hModule;

namespace Log {

void Init(const char* logPath, size_t maxSizeMb, size_t maxFiles)
{
    std::string path;
    if (logPath && logPath[0]) {
        path = logPath;
    } else {
        char buf[MAX_PATH];
        GetModuleFileNameA(g_hModule, buf, MAX_PATH);
        path = buf;
        auto pos = path.find_last_of("\\/");
        if (pos != std::string::npos)
            path = path.substr(0, pos + 1);
        path += "coclassic.log";
    }

    try {
        auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            path, maxSizeMb * 1024 * 1024, maxFiles);

        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        auto logger = std::make_shared<spdlog::logger>("cc",
            spdlog::sinks_init_list{fileSink, consoleSink});

        logger->set_level(spdlog::level::trace);
        logger->set_pattern("[%H:%M:%S.%e] [%l] %v");
        logger->flush_on(spdlog::level::debug);

        spdlog::set_default_logger(logger);
        spdlog::info("Logger initialized — file: {}", path);
    } catch (const spdlog::spdlog_ex& ex) {
        // Fallback: if file logging fails, just use console
        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("cc", consoleSink);
        logger->set_level(spdlog::level::trace);
        logger->set_pattern("[%H:%M:%S.%e] [%l] %v");
        spdlog::set_default_logger(logger);
        spdlog::warn("File logger failed ({}), using console only", ex.what());
    }
}

void Shutdown()
{
    spdlog::info("Logger shutting down");
    spdlog::default_logger()->flush();
    spdlog::shutdown();
}

}  // namespace Log
