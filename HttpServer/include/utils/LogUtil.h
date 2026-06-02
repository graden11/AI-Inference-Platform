#pragma once

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <memory>
#include <string>

inline void initSpdLog(const std::string &logPath = "server.log",
                       const std::string &level = "INFO")
{
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath, true);

    spdlog::level::level_enum lv = spdlog::level::info;
    if (level == "TRACE")      lv = spdlog::level::trace;
    else if (level == "DEBUG") lv = spdlog::level::debug;
    else if (level == "WARN")  lv = spdlog::level::warn;
    else if (level == "ERROR") lv = spdlog::level::err;

    auto logger = std::make_shared<spdlog::logger>("server",
        spdlog::sinks_init_list{consoleSink, fileSink});
    logger->set_level(lv);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(logger);

    spdlog::info("spdlog initialized, level={}", level);
}

inline void initAccessLog(const std::string &logPath = "access.log")
{
    auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath, true);
    auto accessLogger = std::make_shared<spdlog::logger>("access", fileSink);
    accessLogger->set_pattern("%v");
    accessLogger->flush_on(spdlog::level::info);
    spdlog::register_logger(accessLogger);
}

inline std::string generateRequestId()
{
    static std::atomic<uint64_t> counter{0};
    uint64_t id = counter.fetch_add(1);
    std::ostringstream oss;
    oss << std::hex << id;
    return oss.str();
}

#define LOG_ACCESS(...) spdlog::get("access")->info(__VA_ARGS__)
