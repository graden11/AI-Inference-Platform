#pragma once

#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct ServerConfig {
    int port = 80;
    std::string name = "HttpServer";
    int threads = 4;
    std::string log_level = "WARN";
    int shutdown_timeout_ms = 30000;
};

struct MysqlConfig {
    std::string host = "tcp://mysql:3306";
    std::string user = "";
    std::string password = "";
    std::string database = "inference_platform";
    int pool_size = 10;
};

struct ModelEntryConfig {
    std::string type;    // "onnx" or "tensorrt"
    std::string version; // 为空时默认 "1"
    std::string path;

    // --- Extended fields (all optional, ImageNet defaults) ---
    std::string task;              // "classification" (default)
    std::string labels;            // per-model labels path; empty = use global fallback
    int top_k = 5;

    // Input
    std::string input_name = "input";
    int input_width  = 224;
    int input_height = 224;
    int input_channels = 3;
    std::vector<float> input_mean = {0.485f, 0.456f, 0.406f};
    std::vector<float> input_std  = {0.229f, 0.224f, 0.225f};

    // Output
    std::string output_name = "output";

    // Detection / Segmentation
    float confidence_threshold = 0.5f;
    float nms_threshold = 0.45f;
    int   max_detections = 100;
};

struct DynamicModelEntry {
    std::string name;
    std::string version;
    std::string type;
    std::string path;
    std::string task;
    std::string labels;
    int top_k = 5;
    int input_width = 224;
    int input_height = 224;
    int input_channels = 3;
    std::string input_name = "input";
    std::string output_name = "output";
    std::vector<float> input_mean = {0.485f, 0.456f, 0.406f};
    std::vector<float> input_std  = {0.229f, 0.224f, 0.225f};

    // Detection
    float confidence_threshold = 0.5f;
    float nms_threshold = 0.45f;
    int   max_detections = 100;
};

struct RedisConfig {
    std::string host;   // 空 = 使用内存模式
    int port = 6379;
    int pool_size = 5;
};

struct BatchingConfig {
    bool enabled = false;
    int max_batch_size = 8;
    int max_delay_ms = 10;
};

struct LoggingConfig {
    std::string level = "INFO";
    std::string file  = "server.log";
    std::string access_log = "access.log";
};

struct AppConfig {
    ServerConfig server;
    MysqlConfig mysql;
    RedisConfig redis;
    BatchingConfig batching;
    LoggingConfig logging;
    std::string labels_path;
    std::unordered_map<std::string, ModelEntryConfig> models;
    std::vector<DynamicModelEntry> dynamic_engines;
};

inline AppConfig loadConfig(const std::string &filePath)
{
    AppConfig cfg;

    std::ifstream f(filePath);
    if (!f.good())
    {
        std::cerr << "Config file not found: " << filePath
                  << ", using defaults" << std::endl;
        return cfg;
    }

    json j;
    try {
        f >> j;
    } catch (const std::exception &e) {
        std::cerr << "Failed to parse config: " << e.what() << std::endl;
        return cfg;
    }

    // Server
    if (j.contains("server"))
    {
        auto &s = j["server"];
        if (s.contains("port"))     cfg.server.port = s["port"].get<int>();
        if (s.contains("name"))     cfg.server.name = s["name"].get<std::string>();
        if (s.contains("threads"))  cfg.server.threads = s["threads"].get<int>();
        if (s.contains("log_level")) cfg.server.log_level = s["log_level"].get<std::string>();
        if (s.contains("shutdown_timeout_ms")) cfg.server.shutdown_timeout_ms = s["shutdown_timeout_ms"].get<int>();
    }

    // Logging
    if (j.contains("logging"))
    {
        auto &l = j["logging"];
        if (l.contains("level"))      cfg.logging.level = l["level"].get<std::string>();
        if (l.contains("file"))       cfg.logging.file  = l["file"].get<std::string>();
        if (l.contains("access_log")) cfg.logging.access_log = l["access_log"].get<std::string>();
    }

    // MySQL
    if (j.contains("mysql"))
    {
        auto &m = j["mysql"];
        if (m.contains("host"))      cfg.mysql.host      = m["host"].get<std::string>();
        if (m.contains("user"))      cfg.mysql.user      = m["user"].get<std::string>();
        if (m.contains("password"))  cfg.mysql.password  = m["password"].get<std::string>();
        if (m.contains("database"))  cfg.mysql.database  = m["database"].get<std::string>();
        if (m.contains("pool_size")) cfg.mysql.pool_size = m["pool_size"].get<int>();
    }

    // Redis
    if (j.contains("redis"))
    {
        auto &r = j["redis"];
        if (r.contains("host"))      cfg.redis.host      = r["host"].get<std::string>();
        if (r.contains("port"))      cfg.redis.port      = r["port"].get<int>();
        if (r.contains("pool_size")) cfg.redis.pool_size = r["pool_size"].get<int>();
    }

    // Models
    if (j.contains("models"))
    {
        auto &m = j["models"];
        // Support both old "labels_path" and new "global_labels_path"
        if (m.contains("labels_path"))
            cfg.labels_path = m["labels_path"].get<std::string>();
        else if (m.contains("global_labels_path"))
            cfg.labels_path = m["global_labels_path"].get<std::string>();

        if (m.contains("engines"))
        {
            for (auto &[name, entry] : m["engines"].items())
            {
                ModelEntryConfig mec;
                mec.type    = entry.value("type", "onnx");
                mec.version = entry.value("version", "");
                mec.path    = entry.value("path", "");

                // Extended fields (optional, ImageNet defaults)
                mec.task     = entry.value("task", "classification");
                mec.labels   = entry.value("labels", "");
                mec.top_k    = entry.value("top_k", 5);
                mec.input_name     = entry.value("input_name", "input");
                mec.output_name    = entry.value("output_name", "output");
                mec.input_width    = entry.value("input_width", 224);
                mec.input_height   = entry.value("input_height", 224);
                mec.input_channels = entry.value("input_channels", 3);

                if (entry.contains("input"))
                {
                    auto &in = entry["input"];
                    mec.input_name     = in.value("name", "input");
                    mec.input_width    = in.value("width", 224);
                    mec.input_height   = in.value("height", 224);
                    mec.input_channels = in.value("channels", 3);
                    if (in.contains("mean"))
                    {
                        mec.input_mean.clear();
                        for (auto &v : in["mean"])
                            mec.input_mean.push_back(v.get<float>());
                    }
                    if (in.contains("std"))
                    {
                        mec.input_std.clear();
                        for (auto &v : in["std"])
                            mec.input_std.push_back(v.get<float>());
                    }
                }
                if (entry.contains("output"))
                {
                    auto &out = entry["output"];
                    mec.output_name = out.value("name", "output");
                }

                cfg.models[name] = mec;
            }
        }
    }

    // Batching
    if (j.contains("batching"))
    {
        auto &b = j["batching"];
        if (b.contains("enabled"))         cfg.batching.enabled = b["enabled"].get<bool>();
        if (b.contains("max_batch_size"))  cfg.batching.max_batch_size = b["max_batch_size"].get<int>();
        if (b.contains("max_delay_ms"))    cfg.batching.max_delay_ms = b["max_delay_ms"].get<int>();
    }

    // Dynamic engines (persisted by /models/load API)
    if (j.contains("dynamic_engines"))
    {
        for (auto &entry : j["dynamic_engines"])
        {
            DynamicModelEntry dme;
            dme.name    = entry.value("name", "");
            dme.version = entry.value("version", "");
            dme.type    = entry.value("type", "");
            dme.path    = entry.value("path", "");
            dme.task    = entry.value("task", "classification");
            dme.labels  = entry.value("labels", "");
            dme.top_k   = entry.value("top_k", 5);
            dme.input_name     = entry.value("input_name", "input");
            dme.output_name    = entry.value("output_name", "output");
            dme.input_width    = entry.value("input_width", 224);
            dme.input_height   = entry.value("input_height", 224);
            dme.input_channels = entry.value("input_channels", 3);
            dme.confidence_threshold = entry.value("confidence_threshold", 0.5f);
            dme.nms_threshold  = entry.value("nms_threshold", 0.45f);
            dme.max_detections = entry.value("max_detections", 100);
            if (entry.contains("input_mean"))
            {
                dme.input_mean.clear();
                for (auto &v : entry["input_mean"])
                    dme.input_mean.push_back(v.get<float>());
            }
            if (entry.contains("input_std"))
            {
                dme.input_std.clear();
                for (auto &v : entry["input_std"])
                    dme.input_std.push_back(v.get<float>());
            }
            if (!dme.name.empty() && !dme.path.empty())
                cfg.dynamic_engines.push_back(dme);
        }
    }

    return cfg;
}
