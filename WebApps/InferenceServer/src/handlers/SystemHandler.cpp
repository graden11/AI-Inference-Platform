#include "../../include/handlers/SystemHandler.h"
#include "../../include/InferenceServer.h"
#include "../../../../HttpServer/include/http/HttpResponse.h"
#include <fstream>
#include <csignal>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

void SystemHandler::handle(const http::HttpRequest &req, http::HttpResponse *resp)
{
    if (req.path() == "/system/hardware")
        handleGetHardware(req, resp);
    else if (req.path() == "/system/config/apply")
        handleApplyConfig(req, resp);
    else if (req.path() == "/system/restart")
        handleRestart(req, resp);
    else
    {
        json err;
        err["status"] = "error";
        err["message"] = "Not Found";
        std::string body = err.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k404NotFound, "Not Found");
        resp->setContentType("application/json");
        resp->setContentLength(body.size());
        resp->setBody(body);
        resp->setCloseConnection(true);
    }
}

void SystemHandler::handleGetHardware(const http::HttpRequest &req, http::HttpResponse *resp)
{
    try
    {
        const AppConfig &cfg = server_->config_;
        const auto &rec = cfg.recommendations;

        json j;

        // system_profile
        {
            json sp;
            sp["cpu"]           = rec.system_profile.cpu;
            sp["ram_gb"]        = rec.system_profile.ram_gb;
            sp["gpu"]           = rec.system_profile.gpu;
            sp["per_sample_mb"] = rec.system_profile.per_sample_mb;
            sp["gpu_count"]     = rec.system_profile.gpu_count;
            sp["has_gpu"]       = rec.system_profile.has_gpu;
            j["system_profile"] = sp;
        }
        j["generated_at"] = rec.generated_at;
        j["scenario"]     = rec.scenario;
        j["valid"]        = rec.valid;

        // profiles
        {
            json profiles = json::object();
            for (auto &[key, prof] : rec.profiles)
            {
                json pj;
                pj["label"]      = prof.label;
                pj["risk_level"] = prof.risk_level;
                pj["best_for"]   = prof.best_for;
                pj["reason"]     = prof.reason;
                {
                    json params;
                    params["server_threads"]       = prof.params.server_threads;
                    params["max_batch_size"]       = prof.params.max_batch_size;
                    params["max_delay_ms"]         = prof.params.max_delay_ms;
                    params["max_queue_delay_us"]   = prof.params.max_queue_delay_us;
                    params["preferred_batch_sizes"] = prof.params.preferred_batch_sizes;
                    params["workspace_mb"]         = prof.params.workspace_mb;
                    params["fp16"]                 = prof.params.fp16;
                    params["rate_limit_req_per_sec"] = prof.params.rate_limit_req_per_sec;
                    params["rate_limit_burst"]     = prof.params.rate_limit_burst;
                    pj["params"] = params;
                }
                profiles[key] = pj;
            }
            j["profiles"] = profiles;
        }

        // current config (for comparison table on frontend)
        {
            json cur;
            cur["server_threads"] = cfg.server.threads;
            cur["max_batch_size"] = cfg.batching.max_batch_size;
            cur["max_delay_ms"]   = cfg.batching.max_delay_ms;
            cur["max_queue_delay_us"] = cfg.batching.max_queue_delay_us;
            if (!cfg.batching.preferred_batch_sizes.empty())
                cur["preferred_batch_sizes"] = cfg.batching.preferred_batch_sizes;
            cur["rate_limit_req_per_sec"] = cfg.server.rate_limit_req_per_sec;
            cur["rate_limit_burst"]      = cfg.server.rate_limit_burst;
            cur["fp16"]            = rec.system_profile.has_gpu && rec.system_profile.gpu_count > 0;
            j["current"] = cur;
        }

        std::string body = j.dump(2);
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setContentLength(body.size());
        resp->setBody(body);
        resp->setCloseConnection(false);
    }
    catch (const std::exception &e)
    {
        json err;
        err["status"]  = "error";
        err["message"] = e.what();
        std::string body = err.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k500InternalServerError,
                           "Internal Server Error");
        resp->setContentType("application/json");
        resp->setContentLength(body.size());
        resp->setBody(body);
        resp->setCloseConnection(true);
    }
}

void SystemHandler::handleApplyConfig(const http::HttpRequest &req, http::HttpResponse *resp)
{
    try
    {
        if (!server_->ensureAuthenticated(req, resp))
            return;

        json reqBody = json::parse(req.getBody());
        std::string profileKey = reqBody.value("profile", "");

        if (profileKey != "stable" && profileKey != "aggressive")
        {
            json err;
            err["status"]  = "error";
            err["message"] = "Invalid profile key. Use 'stable' or 'aggressive'.";
            std::string body = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
            resp->setContentType("application/json");
            resp->setContentLength(body.size());
            resp->setBody(body);
            resp->setCloseConnection(false);
            return;
        }

        const AppConfig &cfg = server_->config_;
        auto it = cfg.recommendations.profiles.find(profileKey);
        if (it == cfg.recommendations.profiles.end() || !cfg.recommendations.valid)
        {
            json err;
            err["status"]  = "error";
            err["message"] = "No recommendations available. Recommendations are computed at startup.";
            std::string body = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
            resp->setContentType("application/json");
            resp->setContentLength(body.size());
            resp->setBody(body);
            resp->setCloseConnection(false);
            return;
        }

        const auto &p = it->second.params;

        // Read existing config, update values, write back
        json j;
        {
            std::ifstream f(server_->persistConfigPath_);
            if (f.good())
            {
                try { f >> j; }
                catch (...) { j = json::object(); }
            }
        }

        if (!j.contains("server")) j["server"] = json::object();
        j["server"]["threads"] = p.server_threads;
        if (p.rate_limit_req_per_sec > 0)
        {
            j["server"]["rate_limit_req_per_sec"] = p.rate_limit_req_per_sec;
            j["server"]["rate_limit_burst"]      = p.rate_limit_burst;
        }
        else
        {
            j["server"]["rate_limit_req_per_sec"] = 0;
            j["server"]["rate_limit_burst"]      = 0;
        }

        if (!j.contains("batching")) j["batching"] = json::object();
        j["batching"]["enabled"]       = (p.max_batch_size > 1);
        j["batching"]["max_batch_size"] = p.max_batch_size;
        j["batching"]["max_delay_ms"]   = p.max_delay_ms;
        j["batching"]["max_queue_delay_us"] = p.max_queue_delay_us;
        if (!p.preferred_batch_sizes.empty())
            j["batching"]["preferred_batch_sizes"] = p.preferred_batch_sizes;

        {
            std::ofstream of(server_->persistConfigPath_);
            of << j.dump(2) << std::endl;
        }

        // Also update in-memory config so GET /system/hardware returns the
        // new values immediately (no stale "Current" column until restart).
        server_->config_.server.threads              = p.server_threads;
        server_->config_.batching.enabled            = (p.max_batch_size > 1);
        server_->config_.batching.max_batch_size     = p.max_batch_size;
        server_->config_.batching.max_delay_ms       = p.max_delay_ms;
        server_->config_.batching.max_queue_delay_us = p.max_queue_delay_us;
        if (!p.preferred_batch_sizes.empty())
            server_->config_.batching.preferred_batch_sizes = p.preferred_batch_sizes;
        server_->config_.server.rate_limit_req_per_sec = p.rate_limit_req_per_sec;
        server_->config_.server.rate_limit_burst     = p.rate_limit_burst;

        spdlog::info("SystemHandler: applied '{}' profile to config.json", profileKey);

        json ok;
        ok["status"]    = "ok";
        ok["message"]   = "Applied '" + profileKey + "' profile. Restart server for changes to take effect.";
        ok["need_restart"] = true;
        std::string body = ok.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setContentLength(body.size());
        resp->setBody(body);
        resp->setCloseConnection(false);
    }
    catch (const json::parse_error &e)
    {
        json err;
        err["status"]  = "error";
        err["message"] = std::string("Invalid JSON: ") + e.what();
        std::string body = err.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setContentType("application/json");
        resp->setContentLength(body.size());
        resp->setBody(body);
        resp->setCloseConnection(false);
    }
    catch (const std::exception &e)
    {
        json err;
        err["status"]  = "error";
        err["message"] = e.what();
        std::string body = err.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k500InternalServerError,
                           "Internal Server Error");
        resp->setContentType("application/json");
        resp->setContentLength(body.size());
        resp->setBody(body);
        resp->setCloseConnection(true);
    }
}

void SystemHandler::handleRestart(const http::HttpRequest &req, http::HttpResponse *resp)
{
    try
    {
        if (!server_->ensureAuthenticated(req, resp))
            return;

        json ok;
        ok["status"]  = "ok";
        ok["message"] = "Server restarting — please wait ~10 seconds and refresh.";
        std::string body = ok.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setContentLength(body.size());
        resp->setBody(body);
        resp->setCloseConnection(true);  // flush and close so client reliably gets the body

        // Delay SIGTERM by 200ms so muduo has time to flush the response to the socket.
        // Otherwise the client sees a TCP RST instead of the "restarting" JSON.
        server_->getLoop()->runAfter(0.2, []() {
            spdlog::info("SystemHandler: sending SIGTERM to trigger restart");
            ::kill(::getpid(), SIGTERM);
        });
    }
    catch (const std::exception &e)
    {
        json err;
        err["status"]  = "error";
        err["message"] = e.what();
        std::string body = err.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k500InternalServerError,
                           "Internal Server Error");
        resp->setContentType("application/json");
        resp->setContentLength(body.size());
        resp->setBody(body);
        resp->setCloseConnection(true);
    }
}
