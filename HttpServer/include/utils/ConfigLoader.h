#pragma once

#include <cstdlib>
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

    // Per-IP rate limiting: token bucket (0 = disabled).
    int rate_limit_req_per_sec = 100;
    int rate_limit_burst = 200;
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

    // Layout
    std::string input_layout = "chw";
    std::string output_layout = "chw";

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

    // Layout
    std::string input_layout = "chw";
    std::string output_layout = "chw";

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

// --- Adaptive Hardware Config ---
struct RecommendationParams {
    int  server_threads = 0;
    int  max_batch_size = 0;
    int  max_delay_ms = 0;
    int  workspace_mb = 0;
    bool fp16 = false;
    int  rate_limit_req_per_sec = 0;
    int  rate_limit_burst = 0;
};

struct RecommendationProfile {
    std::string label;
    std::string risk_level;
    std::string best_for;
    std::string reason;
    RecommendationParams params;
};

struct SystemProfileInfo {
    std::string cpu;
    int ram_gb = 0;
    std::string gpu;
    float per_sample_mb = 0.0f;
    int gpu_count = 0;
    bool has_gpu = false;
};

struct Recommendations {
    bool valid = false;  // true when recommendations have been generated
    std::string generated_at;
    std::string scenario;          // "CPU_ONLY", "GPU_ONLY", or "MIXED"
    SystemProfileInfo system_profile;
    std::unordered_map<std::string, RecommendationProfile> profiles; // "stable", "aggressive"
};

struct AppConfig {
    ServerConfig server;
    MysqlConfig mysql;
    RedisConfig redis;
    BatchingConfig batching;
    LoggingConfig logging;
    std::string labels_path;
    bool restore_dynamic_on_startup = true;
    std::unordered_map<std::string, ModelEntryConfig> models;
    std::vector<DynamicModelEntry> dynamic_engines;
    Recommendations recommendations;
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
        if (s.contains("port")) {
            int p = s["port"].get<int>();
            if (p < 1 || p > 65535) {
                std::cerr << "Invalid server.port: " << p << " (must be 1-65535)" << std::endl;
                std::exit(1);
            }
            cfg.server.port = p;
        }
        if (s.contains("name"))     cfg.server.name = s["name"].get<std::string>();
        if (s.contains("threads")) {
            int t = s["threads"].get<int>();
            if (t < 1) {
                std::cerr << "Invalid server.threads: " << t << " (must be >= 1)" << std::endl;
                std::exit(1);
            }
            cfg.server.threads = t;
        }
        if (s.contains("log_level")) cfg.server.log_level = s["log_level"].get<std::string>();
        if (s.contains("shutdown_timeout_ms")) cfg.server.shutdown_timeout_ms = s["shutdown_timeout_ms"].get<int>();
        if (s.contains("rate_limit_req_per_sec")) cfg.server.rate_limit_req_per_sec = s["rate_limit_req_per_sec"].get<int>();
        if (s.contains("rate_limit_burst")) cfg.server.rate_limit_burst = s["rate_limit_burst"].get<int>();
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
        if (m.contains("restore_dynamic_on_startup"))
            cfg.restore_dynamic_on_startup = m["restore_dynamic_on_startup"].get<bool>();
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
                    mec.input_layout   = in.value("layout", "chw");
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
                    mec.output_name   = out.value("name", "output");
                    mec.output_layout = out.value("layout", "chw");
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
        if (b.contains("max_batch_size")) {
            int bs = b["max_batch_size"].get<int>();
            if (bs < 1) {
                std::cerr << "Invalid batching.max_batch_size: " << bs << " (must be >= 1)" << std::endl;
                std::exit(1);
            }
            cfg.batching.max_batch_size = bs;
        }
        if (b.contains("max_delay_ms")) {
            int dm = b["max_delay_ms"].get<int>();
            if (dm < 1) {
                std::cerr << "Invalid batching.max_delay_ms: " << dm << " (must be >= 1)" << std::endl;
                std::exit(1);
            }
            cfg.batching.max_delay_ms = dm;
        }
    }

    // Recommendations (generated by HardwareDetector + ConfigAdvisor on startup)
    if (j.contains("recommendations"))
    {
        auto &rec = j["recommendations"];
        cfg.recommendations.valid = true;
        cfg.recommendations.generated_at = rec.value("generated_at", "");
        cfg.recommendations.scenario = rec.value("scenario", "");
        if (rec.contains("system_profile"))
        {
            auto &sp = rec["system_profile"];
            cfg.recommendations.system_profile.cpu = sp.value("cpu", "");
            cfg.recommendations.system_profile.ram_gb = sp.value("ram_gb", 0);
            cfg.recommendations.system_profile.gpu = sp.value("gpu", "");
            cfg.recommendations.system_profile.per_sample_mb = sp.value("per_sample_mb", 0.0f);
            cfg.recommendations.system_profile.gpu_count = sp.value("gpu_count", 0);
            cfg.recommendations.system_profile.has_gpu = sp.value("has_gpu", false);
        }
        if (rec.contains("profiles"))
        {
            for (auto &[key, prof] : rec["profiles"].items())
            {
                RecommendationProfile rp;
                rp.label     = prof.value("label", "");
                rp.risk_level = prof.value("risk_level", "");
                rp.best_for  = prof.value("best_for", "");
                rp.reason    = prof.value("reason", "");
                if (prof.contains("params"))
                {
                    auto &p = prof["params"];
                    rp.params.server_threads      = p.value("server_threads", 0);
                    rp.params.max_batch_size      = p.value("max_batch_size", 0);
                    rp.params.max_delay_ms        = p.value("max_delay_ms", 0);
                    rp.params.workspace_mb        = p.value("workspace_mb", 0);
                    rp.params.fp16               = p.value("fp16", false);
                    rp.params.rate_limit_req_per_sec = p.value("rate_limit_req_per_sec", 0);
                    rp.params.rate_limit_burst    = p.value("rate_limit_burst", 0);
                }
                cfg.recommendations.profiles[key] = rp;
            }
        }
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
            dme.input_layout  = entry.value("input_layout", "chw");
            dme.output_layout = entry.value("output_layout", "chw");
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
