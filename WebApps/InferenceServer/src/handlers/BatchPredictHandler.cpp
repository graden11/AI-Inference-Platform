#include "../../include/handlers/BatchPredictHandler.h"
#include "../../include/ModelFactory.h"
#include "../../include/InferenceEngine.h"

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
    resp->setBody(body);
    resp->setCloseConnection(false);
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

        auto engine = factory_->getModel(modelName);
        if (!engine)
        {
            sendError(req, resp, http::HttpResponse::k400BadRequest,
                      "unknown model: " + modelName);
            return;
        }

        std::vector<std::vector<uint8_t>> imageBytes;

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
                auto data = readFile(p.get<std::string>());
                if (data.empty())
                {
                    sendError(req, resp, http::HttpResponse::k400BadRequest,
                              "failed to read image: " + p.get<std::string>());
                    return;
                }
                imageBytes.push_back(std::move(data));
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
                auto data = base64Decode(img.get<std::string>());
                if (data.empty())
                {
                    sendError(req, resp, http::HttpResponse::k400BadRequest,
                              "failed to decode base64 image at index");
                    return;
                }
                imageBytes.push_back(std::move(data));
            }
        }

        std::vector<std::string> resultJsons;
        try
        {
            resultJsons = engine->predictBatch(imageBytes);
        }
        catch (const std::exception &e)
        {
            LOG_ERROR << "BatchPredictHandler predictBatch error: " << e.what();
            sendError(req, resp, http::HttpResponse::k500InternalServerError,
                      std::string("inference error: ") + e.what());
            return;
        }

        json response;
        response["status"] = "ok";
        response["model_name"] = modelName;
        response["count"] = static_cast<int>(resultJsons.size());

        json results = json::array();
        for (auto &r : resultJsons)
        {
            try
            {
                results.push_back(json::parse(r));
            }
            catch (...)
            {
                results.push_back(r);
            }
        }
        response["results"] = results;

        std::string respBody = response.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setContentLength(respBody.size());
        resp->setBody(respBody);
        resp->setCloseConnection(false);
    }
    catch (const json::exception &e)
    {
        LOG_ERROR << "BatchPredictHandler JSON parse error: " << e.what();
        sendError(req, resp, http::HttpResponse::k400BadRequest,
                  std::string("invalid JSON: ") + e.what());
    }
}
