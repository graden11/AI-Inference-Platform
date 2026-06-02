#include <stb_image.h>
#include <stb_image_resize2.h>

#include "../include/ResNet50TRTEngine.h"

#include <NvOnnxParser.h>

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <muduo/base/Logging.h>

#include "../../../HttpServer/include/utils/JsonUtil.h"

void ResNet50TRTEngine::Logger::log(Severity severity, const char *msg) noexcept
{
    if (severity <= Severity::kWARNING)
        LOG_WARN << "[TensorRT] " << msg;
}

static bool initCuda()
{
    cudaError_t err = cudaFree(nullptr);
    return err == cudaSuccess;
}

ResNet50TRTEngine::ResNet50TRTEngine(const std::string &enginePath, const std::string &labelsPath,
                                     int maxBatchSize)
    : maxBatchSize_(maxBatchSize)
{
    if (!initCuda())
    {
        LOG_ERROR << "CUDA initialization failed";
        return;
    }

    labels_ = loadLabels(labelsPath);

    if (!loadEngine(enginePath))
    {
        LOG_ERROR << "Failed to load TensorRT engine: " << enginePath;
        std::string onnxPath = std::filesystem::path(enginePath).replace_extension(".onnx").string();
        if (!std::ifstream(onnxPath).good())
        {
            // Fallback: use base ONNX model in same directory
            size_t slash = enginePath.rfind('/');
            std::string dir = (slash != std::string::npos) ? enginePath.substr(0, slash + 1) : "";
            onnxPath = dir + "resnet50_classification.onnx";
        }
        LOG_INFO << "Attempting to build TRT engine from ONNX: " << onnxPath;
        if (buildEngine(onnxPath, enginePath, maxBatchSize_))
        {
            LOG_INFO << "Successfully built TRT engine, reloading";
            if (!loadEngine(enginePath))
            {
                LOG_ERROR << "Failed to load newly built TRT engine";
                return;
            }
        }
        else
        {
            LOG_ERROR << "Failed to build TRT engine from ONNX: " << onnxPath;
            return;
        }
    }

    // Create CUDA stream for async execution
    cudaError_t err = cudaStreamCreate(&stream_);
    if (err != cudaSuccess)
        LOG_ERROR << "cudaStreamCreate failed: " << cudaGetErrorString(err);

    // Allocate double-buffered pinned + device memory slots
    size_t resizeBytes = INPUT_H * INPUT_W * INPUT_C * sizeof(uint8_t);
    for (int i = 0; i < kNumSlots; ++i)
    {
        BufferSlot &s = slots_[i];
        cudaMallocHost(&s.h_resized_pinned, resizeBytes);
        cudaMallocHost(&s.h_input_pinned, inputSize_);
        cudaMallocHost(&s.h_output_pinned, outputSize_);
        cudaMalloc(&s.d_input, inputSize_);
        cudaMalloc(&s.d_output, outputSize_);
    }

    if (maxBatchSize_ > 1)
        allocateBatchBuffers();

    LOG_INFO << "ResNet50TRTEngine initialized, engine: " << enginePath
             << ", labels: " << labels_.size()
             << ", maxBatchSize: " << maxBatchSize_
             << ", stream + " << kNumSlots << " pinned-memory slots";
}

ResNet50TRTEngine::~ResNet50TRTEngine()
{
    cudaStreamDestroy(stream_);
    for (int i = 0; i < kNumSlots; ++i)
    {
        BufferSlot &s = slots_[i];
        if (s.h_resized_pinned) cudaFreeHost(s.h_resized_pinned);
        if (s.h_input_pinned)   cudaFreeHost(s.h_input_pinned);
        if (s.h_output_pinned)  cudaFreeHost(s.h_output_pinned);
        if (s.d_input)          cudaFree(s.d_input);
        if (s.d_output)         cudaFree(s.d_output);
    }
    if (h_batch_input_)   cudaFreeHost(h_batch_input_);
    if (h_batch_output_)  cudaFreeHost(h_batch_output_);
    if (d_batch_input_)   cudaFree(d_batch_input_);
    if (d_batch_output_)  cudaFree(d_batch_output_);
}

int ResNet50TRTEngine::acquireSlot()
{
    std::unique_lock<std::mutex> lock(slot_pool_mutex_);
    slot_pool_cv_.wait(lock, [this] {
        for (int i = 0; i < kNumSlots; ++i)
            if (!slots_[i].in_use) return true;
        return false;
    });
    for (int i = 0; i < kNumSlots; ++i)
    {
        if (!slots_[i].in_use)
        {
            slots_[i].in_use = true;
            return i;
        }
    }
    return -1; // unreachable
}

void ResNet50TRTEngine::releaseSlot(int idx)
{
    {
        std::lock_guard<std::mutex> lock(slot_pool_mutex_);
        slots_[idx].in_use = false;
    }
    slot_pool_cv_.notify_one();
}

bool ResNet50TRTEngine::loadEngine(const std::string &enginePath)
{
    std::ifstream file(enginePath, std::ios::binary);
    if (!file)
    {
        LOG_ERROR << "Cannot open engine file: " << enginePath;
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> engineData(size);
    file.read(engineData.data(), size);

    runtime_.reset(nvinfer1::createInferRuntime(logger_));
    if (!runtime_)
    {
        LOG_ERROR << "Failed to create TensorRT runtime";
        return false;
    }

    engine_.reset(runtime_->deserializeCudaEngine(engineData.data(), size));
    if (!engine_)
    {
        LOG_ERROR << "Failed to deserialize CUDA engine";
        return false;
    }

    context_.reset(engine_->createExecutionContext());
    if (!context_)
    {
        LOG_ERROR << "Failed to create execution context";
        return false;
    }

    // Find input/output binding info
    int nbTensors = engine_->getNbIOTensors();
    for (int i = 0; i < nbTensors; i++)
    {
        const char *name = engine_->getIOTensorName(i);
        auto mode = engine_->getTensorIOMode(name);
        auto dims = engine_->getTensorShape(name);
        size_t tensorSize = sizeof(float);
        for (int d = 0; d < dims.nbDims; d++)
            tensorSize *= static_cast<size_t>(dims.d[d]);

        if (mode == nvinfer1::TensorIOMode::kINPUT)
        {
            inputIndex_ = i;
            inputSize_ = tensorSize;
        }
        else
        {
            outputIndex_ = i;
            outputSize_ = tensorSize;
        }
    }

    return true;
}

bool ResNet50TRTEngine::buildEngine(const std::string &onnxPath, const std::string &enginePath, int maxBatchSize)
{
    Logger localLogger;
    auto builder = std::unique_ptr<nvinfer1::IBuilder>(nvinfer1::createInferBuilder(localLogger));
    if (!builder)
    {
        LOG_ERROR << "Failed to create TensorRT builder";
        return false;
    }

    const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    auto network = std::unique_ptr<nvinfer1::INetworkDefinition>(builder->createNetworkV2(explicitBatch));
    if (!network)
    {
        LOG_ERROR << "Failed to create network definition";
        return false;
    }

    auto parser = std::unique_ptr<nvonnxparser::IParser>(nvonnxparser::createParser(*network, localLogger));
    if (!parser)
    {
        LOG_ERROR << "Failed to create ONNX parser";
        return false;
    }

    if (!parser->parseFromFile(onnxPath.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kWARNING)))
    {
        LOG_ERROR << "Failed to parse ONNX model: " << onnxPath;
        return false;
    }

    auto config = std::unique_ptr<nvinfer1::IBuilderConfig>(builder->createBuilderConfig());
    config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 1ULL << 30);

    if (enginePath.find("int8") != std::string::npos)
    {
        LOG_WARN << "TRT builder: INT8 model requested but calibration not supported; "
                    "building FP16 engine, saved as '" << enginePath << "' (filename implies INT8)";
    }
    if (builder->platformHasFastFp16())
    {
        config->setFlag(nvinfer1::BuilderFlag::kFP16);
        LOG_INFO << "TRT builder: FP16 enabled";
    }

    // Dynamic batch: allow engine to handle batch sizes up to at least 8
    int maxBatch = std::max(8, maxBatchSize);
    auto profile = builder->createOptimizationProfile();
    profile->setDimensions(INPUT_NAME, nvinfer1::OptProfileSelector::kMIN,
                           nvinfer1::Dims4{1, INPUT_C, INPUT_H, INPUT_W});
    profile->setDimensions(INPUT_NAME, nvinfer1::OptProfileSelector::kOPT,
                           nvinfer1::Dims4{maxBatch / 2, INPUT_C, INPUT_H, INPUT_W});
    profile->setDimensions(INPUT_NAME, nvinfer1::OptProfileSelector::kMAX,
                           nvinfer1::Dims4{maxBatch, INPUT_C, INPUT_H, INPUT_W});
    config->addOptimizationProfile(profile);
    LOG_INFO << "TRT builder: dynamic batch profile [" << 1 << ", "
             << maxBatch / 2 << ", " << maxBatch << "]";

    auto plan = std::unique_ptr<nvinfer1::IHostMemory>(builder->buildSerializedNetwork(*network, *config));
    if (!plan)
    {
        LOG_ERROR << "Failed to build serialized TRT engine";
        return false;
    }

    std::ofstream out(enginePath, std::ios::binary);
    if (!out)
    {
        LOG_ERROR << "Cannot write engine file: " << enginePath;
        return false;
    }
    out.write(static_cast<const char *>(plan->data()), static_cast<std::streamsize>(plan->size()));
    if (!out)
    {
        LOG_ERROR << "Failed to write engine file: " << enginePath;
        return false;
    }

    LOG_INFO << "TRT engine built: " << enginePath
             << " (" << plan->size() / 1024 / 1024 << " MB)";
    return true;
}

void ResNet50TRTEngine::preprocess(const uint8_t *rgbData, int w, int h, int channels,
                                    uint8_t *h_resized_pinned, float *h_input_pinned)
{
    stbir_resize_uint8_srgb(rgbData, w, h, 0,
                            h_resized_pinned, INPUT_W, INPUT_H, 0,
                            static_cast<stbir_pixel_layout>(INPUT_C));

    const int pixels = INPUT_H * INPUT_W;
    for (int c = 0; c < INPUT_C; ++c)
    {
        const float inv_std = 1.0f / STD[c];
        float *dst = h_input_pinned + c * pixels;
        for (int p = 0; p < pixels; ++p)
        {
            float val = h_resized_pinned[p * INPUT_C + c] / 255.0f;
            dst[p] = (val - MEAN[c]) * inv_std;
        }
    }
}

void ResNet50TRTEngine::runInference(int slotIdx,
                                      std::vector<std::pair<int, float>> &results)
{
    BufferSlot &slot = slots_[slotIdx];

    // GPU section — serialized across threads
    {
        std::lock_guard<std::mutex> lock(gpu_mutex_);

        cudaMemcpyAsync(slot.d_input, slot.h_input_pinned,
                        inputSize_, cudaMemcpyHostToDevice, stream_);

        context_->setInputTensorAddress(INPUT_NAME, slot.d_input);
        context_->setOutputTensorAddress(OUTPUT_NAME, slot.d_output);
        context_->enqueueV3(stream_);

        cudaMemcpyAsync(slot.h_output_pinned, slot.d_output,
                        outputSize_, cudaMemcpyDeviceToHost, stream_);

        cudaStreamSynchronize(stream_);
    }

    // CPU post-processing — no lock needed, per-slot output already ready
    size_t numClasses = outputSize_ / sizeof(float);

    results = softmaxTopK(slot.h_output_pinned, numClasses);
}

std::string ResNet50TRTEngine::predict(const std::string &imagePath)
{
    if (!context_)
    {
        json err;
        err["status"] = "error";
        err["message"] = "TensorRT engine not loaded";
        return err.dump();
    }

    int slotIdx = acquireSlot();

    int w, h, channels;
    unsigned char *data = stbi_load(imagePath.c_str(), &w, &h, &channels, 3);
    if (!data)
    {
        LOG_ERROR << "Failed to load image: " << imagePath;
        releaseSlot(slotIdx);
        json err;
        err["status"] = "error";
        err["message"] = std::string("failed to load image: ") + imagePath;
        return err.dump();
    }

    preprocess(data, w, h, 3,
               slots_[slotIdx].h_resized_pinned,
               slots_[slotIdx].h_input_pinned);
    stbi_image_free(data);

    std::vector<std::pair<int, float>> results;
    runInference(slotIdx, results);

    releaseSlot(slotIdx);

    return buildPredictionsJson(results, labels_).dump();
}

std::string ResNet50TRTEngine::predictFromBytes(const std::vector<uint8_t> &imageData)
{
    if (!context_)
    {
        json err;
        err["status"] = "error";
        err["message"] = "TensorRT engine not loaded";
        return err.dump();
    }

    int slotIdx = acquireSlot();

    int w, h, channels;
    unsigned char *data = stbi_load_from_memory(
        imageData.data(), static_cast<int>(imageData.size()),
        &w, &h, &channels, 3);
    if (!data)
    {
        LOG_ERROR << "Failed to decode image from memory";
        releaseSlot(slotIdx);
        json err;
        err["status"] = "error";
        err["message"] = "failed to decode image from memory";
        return err.dump();
    }

    preprocess(data, w, h, 3,
               slots_[slotIdx].h_resized_pinned,
               slots_[slotIdx].h_input_pinned);
    stbi_image_free(data);

    std::vector<std::pair<int, float>> results;
    runInference(slotIdx, results);

    releaseSlot(slotIdx);

    return buildPredictionsJson(results, labels_).dump();
}

void ResNet50TRTEngine::allocateBatchBuffers()
{
    perSampleInputSize_ = INPUT_C * INPUT_H * INPUT_W * sizeof(float);
    perSampleOutputSize_ = outputSize_;

    size_t batchInputBytes = maxBatchSize_ * perSampleInputSize_;
    size_t batchOutputBytes = maxBatchSize_ * perSampleOutputSize_;

    cudaMallocHost(&h_batch_input_, batchInputBytes);
    cudaMallocHost(&h_batch_output_, batchOutputBytes);
    cudaMalloc(&d_batch_input_, batchInputBytes);
    cudaMalloc(&d_batch_output_, batchOutputBytes);
}

std::vector<std::string> ResNet50TRTEngine::predictBatch(
    const std::vector<std::vector<uint8_t>> &images)
{
    int batchSize = static_cast<int>(images.size());
    if (batchSize == 0)
        return {};
    if (batchSize > maxBatchSize_)
    {
        LOG_WARN << "Batch size " << batchSize << " exceeds maxBatchSize " << maxBatchSize_
                 << ", processing sequentially";
        std::vector<std::string> results;
        results.reserve(images.size());
        for (auto &img : images)
            results.push_back(predictFromBytes(img));
        return results;
    }

    if (!context_ || !h_batch_input_)
    {
        // Fallback: sequential inference
        std::vector<std::string> results;
        results.reserve(batchSize);
        for (int i = 0; i < batchSize; ++i)
            results.push_back(predictFromBytes(images[i]));
        return results;
    }

    // Decode and preprocess all images into the batch input buffer
    std::vector<unsigned char *> rawImages(batchSize);
    std::vector<int> widths(batchSize), heights(batchSize), channels(batchSize);
    for (int i = 0; i < batchSize; ++i)
    {
        rawImages[i] = stbi_load_from_memory(
            images[i].data(), static_cast<int>(images[i].size()),
            &widths[i], &heights[i], &channels[i], 3);
    }

    float *dst = h_batch_input_;
    for (int i = 0; i < batchSize; ++i)
    {
        if (rawImages[i])
        {
            stbir_resize_uint8_srgb(rawImages[i], widths[i], heights[i], 0,
                                    reinterpret_cast<uint8_t *>(dst),
                                    INPUT_W, INPUT_H, 0,
                                    static_cast<stbir_pixel_layout>(INPUT_C));
            stbi_image_free(rawImages[i]);
        }

        // Normalize: HWC uint8 → CHW float32
        const int pixels = INPUT_H * INPUT_W;
        thread_local std::vector<uint8_t> resized(INPUT_C * pixels);
        const uint8_t *src = reinterpret_cast<uint8_t *>(dst);
        std::copy(src, src + resized.size(), resized.begin());

        for (int c = 0; c < INPUT_C; ++c)
        {
            const float inv_std = 1.0f / STD[c];
            float *ch_dst = dst + c * pixels;
            for (int p = 0; p < pixels; ++p)
            {
                float val = resized[p * INPUT_C + c] / 255.0f;
                ch_dst[p] = (val - MEAN[c]) * inv_std;
            }
        }
        dst += perSampleInputSize_ / sizeof(float);
    }

    // GPU section
    {
        std::lock_guard<std::mutex> lock(gpu_mutex_);

        size_t batchInputBytes = batchSize * perSampleInputSize_;
        size_t batchOutputBytes = batchSize * perSampleOutputSize_;

        cudaMemcpyAsync(d_batch_input_, h_batch_input_,
                        batchInputBytes, cudaMemcpyHostToDevice, stream_);

        nvinfer1::Dims4 inputDims{batchSize, INPUT_C, INPUT_H, INPUT_W};
        if (!context_->setInputShape(INPUT_NAME, inputDims))
        {
            LOG_ERROR << "TRT setInputShape failed for batch " << batchSize
                      << " — engine may lack dynamic batch support, rebuild required";
            return std::vector<std::string>(images.size(),
                R"({"status":"error","message":"TRT engine does not support dynamic batch, rebuild engine"})");
        }
        context_->setInputTensorAddress(INPUT_NAME, d_batch_input_);
        context_->setOutputTensorAddress(OUTPUT_NAME, d_batch_output_);
        if (!context_->enqueueV3(stream_))
        {
            LOG_ERROR << "TRT enqueueV3 failed for batch " << batchSize;
            return {};
        }

        auto cudaErr = cudaMemcpyAsync(h_batch_output_, d_batch_output_,
                        batchOutputBytes, cudaMemcpyDeviceToHost, stream_);
        if (cudaErr != cudaSuccess)
            LOG_ERROR << "cudaMemcpyAsync D2H failed: " << cudaGetErrorString(cudaErr);

        cudaErr = cudaStreamSynchronize(stream_);
        if (cudaErr != cudaSuccess)
            LOG_ERROR << "cudaStreamSynchronize failed: " << cudaGetErrorString(cudaErr);
    }

    // Split output and build results
    size_t numClasses = perSampleOutputSize_ / sizeof(float);
    std::vector<std::string> results;
    results.reserve(batchSize);
    for (int i = 0; i < batchSize; ++i)
    {
        float *sampleLogits = h_batch_output_ + i * numClasses;
        auto topk = softmaxTopK(sampleLogits, numClasses);
        results.push_back(buildPredictionsJson(topk, labels_).dump());
    }
    return results;
}
