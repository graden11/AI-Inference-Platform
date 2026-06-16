#include "../../include/handlers/RawPredictHandler.h"
#include "../../include/ModelFactory.h"
#include "../../include/InferenceEngine.h"
#include "../../include/DynamicBatchScheduler.h"
#include "../../include/RequestSlotPool.h"

#include "../../../../HttpServer/include/http/HttpResponse.h"
#include "../../../../HttpServer/include/utils/MetricsCollector.h"

#include <chrono>
#include <future>
#include <thread>
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>

namespace
{

void sendRawError(const http::HttpRequest &req, http::HttpResponse *resp,
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

void RawPredictHandler::handle(const http::HttpRequest &req, http::HttpResponse *resp)
{
    try
    {
        // DEBUG
        static std::atomic<int> callCount{0};
        int n = callCount.fetch_add(1);
        if (n % 10 == 0)
            LOG_INFO << "RawPredictHandler called #" << n;

        std::string modelName = req.getQueryParameters("model_name");
        if (modelName.empty())
            modelName = "resnet50";

        // ── T5: raw body available (no JSON parse needed) ──
        if (auto* pt = resp->getPerfTrace().get())
        {
            pt->t5_json_parse_done = pt->nowUs();
            pt->endpoint = req.path();
        }

        const std::string& body = req.getBody();
        if (body.empty())
        {
            sendRawError(req, resp, http::HttpResponse::k400BadRequest, "empty body");
            return;
        }

        std::string ct = req.getHeader("Content-Type");
        if (ct.find("image/") == std::string::npos)
        {
            sendRawError(req, resp, http::HttpResponse::k400BadRequest,
                         "Content-Type must be an image type (image/jpeg, image/png, etc.)");
            return;
        }

        // ── Acquire slot from pool, copy body directly into slot ──
        auto slot = slotPool_ ? slotPool_->acquire() : nullptr;
        if (slot)
        {
            slot->imageBytes.assign(body.begin(), body.end());
            slot->perfTrace = resp->getPerfTrace();
        }

        // ── T6: no base64 decode needed ──
        if (auto* pt = resp->getPerfTrace().get())
            pt->t6_base64_decode_done = pt->nowUs();

        // Batching path
        if (batcher_)
        {
            // Validate model exists BEFORE submitting to scheduler.
            // Without this check, every unknown model_name would leak a
            // permanent queue entry in the scheduler's queues_ map.
            auto engine = factory_->getModel(modelName);
            if (!engine)
            {
                sendRawError(req, resp, http::HttpResponse::k400BadRequest,
                             "unknown model: " + modelName);
                return;
            }

            std::vector<uint8_t> imageBytes;  // fallback
            std::future<std::string> future;

            if (slot)
            {
                future = batcher_->submit(modelName, slot);
            }
            else
            {
                // Pool exhausted — allocate a one-off slot
                auto fallbackSlot = std::make_shared<RequestSlot>();
                fallbackSlot->imageBytes.assign(body.begin(), body.end());
                fallbackSlot->perfTrace = resp->getPerfTrace();
                future = batcher_->submit(modelName, fallbackSlot);  // batcher copies
                slot = fallbackSlot;  // async lambda keeps this copy alive
            }

            // ── T7-T8 ──
            if (auto* pt = resp->getPerfTrace().get())
            {
                pt->t7_batcher_submit_done = pt->nowUs();
                pt->t8_future_get_begin = pt->nowUs();
            }

            // ── Async response — don't block IO thread ──
            // Shallow-copy the response's closeConnection() decision so the
            // async lambda can honour it.  The deferred path skips
            // HttpServer::onRequest's normal close/shutdown logic, so the
            // lambda must do it itself.  Without this, Keep-Alive is
            // silently broken: every deferred response leaves the connection
            // without a Content-Length→closeConnection=true and muduo tears
            // it down, so the client never reuses it.
            resp->setDeferred(true);
            bool keepAlive = !resp->closeConnection();  // capture BEFORE moving conn
            auto conn = resp->getTcpConnection();
            auto version = req.getVersion();
            auto complete = resp->takeCompleteCallback();
            auto perfTrace = resp->getPerfTrace();

            std::thread([conn = std::move(conn),
                         version = std::move(version),
                         future = std::move(future),
                         slot,     // keep slot alive
                         keepAlive,
                         perfTrace = std::move(perfTrace),
                         complete = std::move(complete)]() mutable {
                std::string resultJson;
                try
                {
                    future.get();  // synchronize; data is in slot->resultJson
                    if (slot && !slot->resultJson.empty())
                        resultJson = std::move(slot->resultJson);
                    else if (!slot)
                        resultJson = R"({"status":"error","message":"no slot"})";
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

                // Honour Keep-Alive: if the request indicated keep-alive,
                // the response has Content-Length and we must NOT shut down.
                // Conversely, if closeConnection() was true, close cleanly.
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
            sendRawError(req, resp, http::HttpResponse::k400BadRequest,
                         "unknown model: " + modelName);
            return;
        }

        std::vector<uint8_t> imageBytes(body.begin(), body.end());
        std::string resultJson = engine->predictFromBytes(imageBytes);

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setContentLength(resultJson.size());
        resp->setBody(std::move(resultJson));
        resp->setCloseConnection(false);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "RawPredictHandler error: " << e.what();
        sendRawError(req, resp, http::HttpResponse::k500InternalServerError,
                     std::string("internal error: ") + e.what());
    }
    catch (...)
    {
        LOG_ERROR << "RawPredictHandler unknown error";
        sendRawError(req, resp, http::HttpResponse::k500InternalServerError, "internal error");
    }
}
