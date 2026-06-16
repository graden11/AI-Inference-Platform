#include "../../include/handlers/PredictHandler.h"
#include "../../include/ModelFactory.h"
#include "../../include/InferenceEngine.h"
#include "../../include/DynamicBatchScheduler.h"
#include "../../include/RequestSlotPool.h"

#include "../../../../HttpServer/include/http/HttpResponse.h"
#include "../../../../HttpServer/include/utils/PathValidator.h"
#include "../../../../HttpServer/include/utils/Base64.h"

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>

#include <fstream>
#include <future>
#include <thread>
#include <utility>
#include <vector>

namespace
{

const std::vector<std::string> kAllowedReadDirs = {"models", "images"};

std::vector<uint8_t> readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    auto size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    f.read(reinterpret_cast<char *>(data.data()), size);
    return data;
}

void sendPredictError(const http::HttpRequest &req, http::HttpResponse *resp,
                      http::HttpResponse::HttpStatusCode code,
                      const std::string &message)
{
    json err;
    err["status"] = "error";
    err["message"] = message;
    std::string errBody = err.dump();
    resp->setStatusLine(req.getVersion(), code,
        code == http::HttpResponse::k400BadRequest ? "Bad Request" : "Internal Server Error");
    resp->setContentType("application/json");
    resp->setContentLength(errBody.size());
    resp->setBody(std::move(errBody));
    resp->setCloseConnection(code != http::HttpResponse::k200Ok);
}

} // anonymous namespace

void PredictHandler::handle(const http::HttpRequest &req, http::HttpResponse *resp)
{
    try
    {
        json body = json::parse(req.getBody());

        // ── Perf trace T5: JSON parse done ──
        if (auto* pt = resp->getPerfTrace().get())
        {
            pt->t5_json_parse_done = pt->nowUs();
            pt->endpoint = req.path();
        }

        bool hasPath = body.contains("image_path");
        bool hasData = body.contains("image_data");

        if (!hasPath && !hasData)
        {
            sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                             "missing image_path or image_data");
            return;
        }

        std::string modelName = body.value("model_name", "resnet50");

        // ── Acquire slot from pool ──
        auto slot = slotPool_ ? slotPool_->acquire() : nullptr;
        if (!slot)
        {
            // Pool exhausted — fall back to allocating path below
        }

        // Batching path
        if (batcher_)
        {
            // Validate model exists BEFORE submitting to scheduler
            auto engine = factory_->getModel(modelName);
            if (!engine)
            {
                sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                                 "unknown model: " + modelName);
                return;
            }

            // Decode image bytes into slot (or temp if pool exhausted)
            if (slot)
            {
                if (hasPath)
                {
                    const auto& imagePath = body["image_path"].get_ref<const std::string&>();
                    if (!http::utils::isPathSafeInDirs(imagePath, kAllowedReadDirs))
                    {
                        sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                                         "image_path is outside allowed directories");
                        return;
                    }
                    slot->imageBytes = readFile(imagePath);
                    if (slot->imageBytes.empty())
                    {
                        sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                                         "failed to read image file");
                        return;
                    }
                }
                else
                {
                    const auto& b64 = body["image_data"].get_ref<const std::string&>();
                    slot->imageBytes = http::utils::base64Decode(b64);
                    if (slot->imageBytes.empty())
                    {
                        sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                                         "failed to decode base64 image");
                        return;
                    }
                }
                slot->perfTrace = resp->getPerfTrace();
            }

            std::vector<uint8_t> imageBytes;  // fallback when pool exhausted
            if (!slot)
            {
                if (hasPath)
                {
                    const auto& imagePath = body["image_path"].get_ref<const std::string&>();
                    if (!http::utils::isPathSafeInDirs(imagePath, kAllowedReadDirs))
                    {
                        sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                                         "image_path is outside allowed directories");
                        return;
                    }
                    imageBytes = readFile(imagePath);
                    if (imageBytes.empty())
                    {
                        sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                                         "failed to read image file");
                        return;
                    }
                }
                else
                {
                    const auto& b64 = body["image_data"].get_ref<const std::string&>();
                    imageBytes = http::utils::base64Decode(b64);
                    if (imageBytes.empty())
                    {
                        sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                                         "failed to decode base64 image");
                        return;
                    }
                }
            }

            // ── T6: base64 decode done ──
            if (auto* pt = resp->getPerfTrace().get())
                pt->t6_base64_decode_done = pt->nowUs();

            std::future<std::string> future;
            if (slot)
            {
                future = batcher_->submit(modelName, slot);
            }
            else
            {
                // Pool exhausted — allocate a one-off slot for this request.
                // Keep a shared_ptr copy for the async lambda; batcher gets its own.
                auto fallbackSlot = std::make_shared<RequestSlot>();
                fallbackSlot->imageBytes = std::move(imageBytes);
                fallbackSlot->perfTrace = resp->getPerfTrace();
                future = batcher_->submit(modelName, fallbackSlot);  // batcher copies
                slot = fallbackSlot;  // async lambda keeps this copy alive
            }

            // ── T7: submit done, T8: future.get begin ──
            if (auto* pt = resp->getPerfTrace().get())
            {
                pt->t7_batcher_submit_done = pt->nowUs();
                pt->t8_future_get_begin = pt->nowUs();
            }

            // ── Phase 3: Async response — don't block IO thread ──
            resp->setDeferred(true);
            bool keepAlive = !resp->closeConnection();
            auto conn = resp->getTcpConnection();
            auto version = req.getVersion();
            auto complete = resp->takeCompleteCallback();
            auto perfTrace = resp->getPerfTrace();

            std::thread([conn = std::move(conn),
                         version = std::move(version),
                         future = std::move(future),
                         slot,     // keep slot alive until response is sent
                         keepAlive,
                         perfTrace = std::move(perfTrace),
                         complete = std::move(complete)]() mutable {
                // Wait for inference to complete (promise signals "ok")
                std::string resultJson;
                try
                {
                    future.get();  // synchronize — data is in slot->resultJson
                    if (slot && slot->resultJson.empty())
                        resultJson = R"({"status":"error","message":"empty result"})";
                    else if (slot)
                        resultJson = std::move(slot->resultJson);  // zero-copy move
                }
                catch (const std::exception& e)
                {
                    resultJson = R"({"status":"error","message":")"
                               + std::string(e.what()) + "\"}";
                }

                if (perfTrace)
                    perfTrace->t9_future_get_return = perfTrace->nowUs();

                auto buf = std::make_shared<muduo::net::Buffer>();
                {
                    http::HttpResponse r(false);
                    r.setStatusLine(version, http::HttpResponse::k200Ok, "OK");
                    r.setContentType("application/json");
                    r.setContentLength(resultJson.size());
                    r.setBody(std::move(resultJson));
                    r.setPerfTrace(perfTrace);
                    if (perfTrace)
                        perfTrace->t10_response_set = perfTrace->nowUs();
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
                // slot shared_ptr drops here → returned to pool
            }).detach();

            return;
        }

        // Direct path (no batching)
        auto engine = factory_->getModel(modelName);
        if (!engine)
        {
            sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                             "unknown model: " + modelName);
            return;
        }

        std::string resultJson;
        if (hasPath)
        {
            const auto& imagePath = body["image_path"].get_ref<const std::string&>();
            if (!http::utils::isPathSafeInDirs(imagePath, kAllowedReadDirs))
            {
                sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                                 "image_path is outside allowed directories");
                return;
            }
            resultJson = engine->predict(imagePath);
        }
        else
        {
            const auto& b64 = body["image_data"].get_ref<const std::string&>();
            auto imageBytes = http::utils::base64Decode(b64);
            if (imageBytes.empty())
            {
                sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                                 "failed to decode base64 image");
                return;
            }
            resultJson = engine->predictFromBytes(imageBytes);
        }

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setContentLength(resultJson.size());
        resp->setBody(std::move(resultJson));
        resp->setCloseConnection(false);
    }
    catch (const json::exception &e)
    {
        LOG_ERROR << "PredictHandler JSON parse error: " << e.what();
        sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                         std::string("invalid JSON: ") + e.what());
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "PredictHandler error: " << e.what();
        sendPredictError(req, resp, http::HttpResponse::k500InternalServerError,
                         std::string("internal error: ") + e.what());
    }
    catch (...)
    {
        LOG_ERROR << "PredictHandler unknown error";
        sendPredictError(req, resp, http::HttpResponse::k500InternalServerError,
                         "internal error");
    }
}
