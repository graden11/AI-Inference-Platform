#include "../../include/handlers/PredictHandler.h"
#include "../../include/ModelFactory.h"
#include "../../include/InferenceEngine.h"
#include "../../include/RequestBatcher.h"

#include <muduo/base/Logging.h>

#include <fstream>
#include <vector>

namespace
{

std::vector<uint8_t> base64Decode(const std::string &encoded)
{
    static const std::string kChars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::vector<int> decode(256, -1);
    for (int i = 0; i < 64; ++i)
        decode[static_cast<unsigned char>(kChars[i])] = i;

    std::vector<uint8_t> result;
    int val = 0, valb = -8;
    for (unsigned char c : encoded)
    {
        if (decode[c] == -1)
            break;
        val = (val << 6) + decode[c];
        valb += 6;
        if (valb >= 0)
        {
            result.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return result;
}

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

} // anonymous namespace

void PredictHandler::handle(const http::HttpRequest &req, http::HttpResponse *resp)
{
    try
    {
        json body = json::parse(req.getBody());

        bool hasPath = body.contains("image_path");
        bool hasData = body.contains("image_data");

        if (!hasPath && !hasData)
        {
            json err;
            err["status"] = "error";
            err["message"] = "missing image_path or image_data";
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(false);
            return;
        }

        std::string modelName = body.value("model_name", "resnet50");

        // Batching path
        if (batcher_)
        {
            std::vector<uint8_t> imageBytes;
            if (hasPath)
            {
                imageBytes = readFile(body["image_path"]);
                if (imageBytes.empty())
                {
                    json err;
                    err["status"] = "error";
                    err["message"] = "failed to read image file";
                    std::string errBody = err.dump();
                    resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
                    resp->setContentType("application/json");
                    resp->setContentLength(errBody.size());
                    resp->setBody(errBody);
                    resp->setCloseConnection(false);
                    return;
                }
            }
            else
            {
                imageBytes = base64Decode(body["image_data"]);
            }

            auto future = batcher_->submit(modelName, imageBytes);
            std::string resultJson = future.get();

            resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
            resp->setContentType("application/json");
            resp->setContentLength(resultJson.size());
            resp->setBody(resultJson);
            resp->setCloseConnection(false);
            return;
        }

        // Direct path (no batching)
        auto engine = factory_->getModel(modelName);
        if (!engine)
        {
            json err;
            err["status"] = "error";
            err["message"] = "unknown model: " + modelName;
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(false);
            return;
        }

        std::string resultJson;
        if (hasPath)
        {
            std::string imagePath = body["image_path"];
            resultJson = engine->predict(imagePath);
        }
        else
        {
            std::string b64 = body["image_data"];
            auto imageBytes = base64Decode(b64);
            resultJson = engine->predictFromBytes(imageBytes);
        }

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setContentLength(resultJson.size());
        resp->setBody(resultJson);
        resp->setCloseConnection(false);
    }
    catch (const json::exception &e)
    {
        LOG_ERROR << "PredictHandler JSON parse error: " << e.what();
        json err;
        err["status"] = "error";
        err["message"] = std::string("invalid JSON: ") + e.what();
        std::string errBody = err.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setContentType("application/json");
        resp->setContentLength(errBody.size());
        resp->setBody(errBody);
        resp->setCloseConnection(false);
    }
}
