#include "../../include/handlers/BatchPredictHandler.h"
#include "../../include/ModelFactory.h"
#include "../../include/InferenceEngine.h"
#include "../../include/DynamicBatchScheduler.h"
#include "../../include/RequestSlotPool.h"

#include "../../../../HttpServer/include/http/HttpResponse.h"
#include "../../../../HttpServer/include/utils/JsonUtil.h"
#include "../../../../HttpServer/include/utils/PathValidator.h"
#include "../../../../HttpServer/include/utils/Base64.h"
#include "../../../../HttpServer/include/utils/MetricsCollector.h"

#include <chrono>
#include <future>
#include <thread>
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>

#include <fstream>
#include <utility>
#include <vector>

namespace
{

const std::vector<std::string> kAllowedReadDirs = {"models", "images"};

void sendError(const http::HttpRequest &req, http::HttpResponse *resp,
               http::HttpResponse::HttpStatusCode code,
               const std::string &message)
{
    json err;
    err["status"] = "error";
    err["message"] = message;
    std::string body = err.dump();
    resp->setStatusLine(req.getVersion(), code,
        code == http::HttpResponse::k400BadRequest ? "Bad Request" : "Internal Server Error");
    resp->setContentType("application/json");
    resp->setContentLength(body.size());
    resp->setBody(std::move(body));
    resp->setCloseConnection(code != http::HttpResponse::k200Ok);
}

} // anonymous namespace

void BatchPredictHandler::handle(const http::HttpRequest &req, http::HttpResponse *resp)
{
    try
    {
        json body = json::parse(req.getBody());

        bool hasPaths = body.contains("image_paths");
        bool hasData  = body.contains("images");

        if (!hasPaths && !hasData)
        {
            sendError(req, resp, http::HttpResponse::k400BadRequest,
                      "missing 'images' (base64 array) or 'image_paths' (path array)");
            return;
        }

        if (hasPaths && hasData)
        {
            sendError(req, resp, http::HttpResponse::k400BadRequest,
                      "use either 'images' or 'image_paths', not both");
            return;
        }

        std::string modelName = body.value("model_name", "resnet50");
        LOG_INFO << "BatchPredictHandler: model=" << modelName << " bodySize=" << req.getBody().size();

        // ── Check model exists ──
        auto engine = factory_->getModel(modelName);
        if (!engine)
        {
            sendError(req, resp, http::HttpResponse::k400BadRequest,
                      "unknown model: " + modelName);
            return;
        }

        // ── Decode all images into slots ──
        std::vector<std::shared_ptr<RequestSlot>> slots;
        std::vector<std::future<std::string>> futures;

        auto submitImage = [&](std::vector<uint8_t> imageBytes) -> bool {
            auto slot = slotPool_ ? slotPool_->acquire() : nullptr;
            if (!slot)
                slot = std::make_shared<RequestSlot>();
            slot->imageBytes = std::move(imageBytes);
            slot->perfTrace = resp->getPerfTrace();
            auto future = batcher_->submit(modelName, slot);
            futures.push_back(std::move(future));
            slots.push_back(std::move(slot));
            return true;
        };

        if (hasPaths)
        {
            auto paths = body["image_paths"];
            if (!paths.is_array() || paths.empty())
            {
                sendError(req, resp, http::HttpResponse::k400BadRequest,
                          "'image_paths' must be a non-empty array");
                return;
            }

            for (auto &p : paths)
            {
                const auto& pathStr = p.get_ref<const std::string&>();
                if (!http::utils::isPathSafeInDirs(pathStr, kAllowedReadDirs))
                {
                    sendError(req, resp, http::HttpResponse::k400BadRequest,
                              "image_path is outside allowed directories: " + pathStr);
                    return;
                }
                std::ifstream f(pathStr, std::ios::binary | std::ios::ate);
                if (!f)
                {
                    sendError(req, resp, http::HttpResponse::k400BadRequest,
                              "failed to read image: " + pathStr);
                    return;
                }
                auto size = f.tellg();
                f.seekg(0, std::ios::beg);
                std::vector<uint8_t> data(static_cast<size_t>(size));
                f.read(reinterpret_cast<char *>(data.data()), size);
                submitImage(std::move(data));
            }
        }
        else
        {
            auto images = body["images"];
            if (!images.is_array() || images.empty())
            {
                sendError(req, resp, http::HttpResponse::k400BadRequest,
                          "'images' must be a non-empty array");
                return;
            }

            for (auto &img : images)
            {
                const auto& b64 = img.get_ref<const std::string&>();
                auto data = http::utils::base64Decode(b64);
                if (data.empty())
                {
                    sendError(req, resp, http::HttpResponse::k400BadRequest,
                              "failed to decode base64 image at index");
                    return;
                }
                submitImage(std::move(data));
            }
        }

        // ── Async deferred response ──
        int count = static_cast<int>(slots.size());

        resp->setDeferred(true);
        bool keepAlive = !resp->closeConnection();
        auto conn = resp->getTcpConnection();
        auto version = req.getVersion();
        auto complete = resp->takeCompleteCallback();
        auto perfTrace = resp->getPerfTrace();

        LOG_INFO << "BatchPredictHandler: deferring " << count << " images, conn=" << conn.get();

        std::thread([conn = std::move(conn),
                     version = std::move(version),
                     modelName = std::move(modelName),
                     count,
                     futures = std::move(futures),
                     slots = std::move(slots),
                     keepAlive,
                     perfTrace = std::move(perfTrace),
                     complete = std::move(complete)]() mutable {
            try {
            LOG_INFO << "BatchPredictHandler[async]: waiting on " << count << " futures";
            json response;
            response["status"] = "ok";
            response["model_name"] = modelName;
            response["count"] = count;

            json results = json::array();
            for (int i = 0; i < count; ++i)
            {
                try
                {
                    LOG_INFO << "BatchPredictHandler[async]: future[" << i << "].get()...";
                    futures[i].get();  // synchronize with batcher dispatch
                    LOG_INFO << "BatchPredictHandler[async]: future[" << i << "] done, resultJson="
                             << (slots[i] ? slots[i]->resultJson.size() : -1) << " bytes";
                    std::string resultJson = slots[i] ? std::move(slots[i]->resultJson) : "{}";
                    try
                    {
                        results.push_back(json::parse(resultJson));
                    }
                    catch (...)
                    {
                        json wrapper;
                        wrapper["status"] = "error";
                        wrapper["message"] = resultJson;
                        results.push_back(wrapper);
                    }
                }
                catch (const std::exception& e)
                {
                    LOG_ERROR << "BatchPredictHandler[async]: future[" << i << "] exception: " << e.what();
                    json err;
                    err["status"] = "error";
                    err["message"] = std::string("batch inference failed: ") + e.what();
                    results.push_back(err);
                }
            }
            response["results"] = results;

            std::string respBody = response.dump();
            LOG_INFO << "BatchPredictHandler[async]: sending response " << respBody.size() << " bytes";

            auto buf = std::make_shared<muduo::net::Buffer>();
            {
                http::HttpResponse r(!keepAlive);
                r.setStatusLine(version, http::HttpResponse::k200Ok, "OK");
                r.setContentType("application/json");
                r.setContentLength(respBody.size());
                r.setBody(std::move(respBody));
                r.setPerfTrace(perfTrace);
                r.appendToBuffer(buf.get());
            }

            conn->getLoop()->runInLoop([conn, buf]() {
                conn->send(buf.get());
            });

            if (perfTrace)
                perfTrace->dump(100);

            if (!keepAlive)
            {
                conn->getLoop()->runAfter(0.01, [conn]() {
                    conn->shutdown();
                });
            }

            complete();
            LOG_INFO << "BatchPredictHandler[async]: done";
            } catch (const std::exception& e) {
                LOG_ERROR << "BatchPredictHandler[async]: CRASH in async thread: " << e.what();
                try { complete(); } catch (...) {}
            } catch (...) {
                LOG_ERROR << "BatchPredictHandler[async]: CRASH in async thread (unknown)";
                try { complete(); } catch (...) {}
            }
        }).detach();
    }
    catch (const json::exception &e)
    {
        LOG_ERROR << "BatchPredictHandler JSON parse error: " << e.what();
        sendError(req, resp, http::HttpResponse::k400BadRequest,
                  std::string("invalid JSON: ") + e.what());
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "BatchPredictHandler error: " << e.what();
        sendError(req, resp, http::HttpResponse::k500InternalServerError,
                  std::string("internal error: ") + e.what());
    }
    catch (...)
    {
        LOG_ERROR << "BatchPredictHandler unknown error";
        sendError(req, resp, http::HttpResponse::k500InternalServerError, "internal error");
    }
}
