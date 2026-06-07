#include "../include/TRTBackend.h"

#include <NvOnnxParser.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <muduo/base/Logging.h>

namespace inference {

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------
void TRTBackend::Logger::log(Severity severity, const char* msg) noexcept
{
    if (severity <= Severity::kWARNING)
        LOG_WARN << "[TensorRT] " << msg;
}

// ---------------------------------------------------------------------------
// CUDA init helper
// ---------------------------------------------------------------------------
static bool initCuda()
{
    cudaError_t err = cudaFree(nullptr);
    return err == cudaSuccess;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
TRTBackend::TRTBackend(const ModelConfig& config)
    : config_(config),
      maxBatchSize_(config.max_batch_size)
{
    if (!initCuda())
    {
        throw std::runtime_error("CUDA initialization failed");
    }

    if (!loadEngine(config.path))
    {
        throw std::runtime_error("Failed to load TensorRT engine: " + config.path
                                 + " -- use POST /models/convert to build from ONNX first");
    }

    // Create CUDA stream for async execution
    cudaError_t err = cudaStreamCreate(&stream_);
    if (err != cudaSuccess)
        LOG_ERROR << "cudaStreamCreate failed: " << cudaGetErrorString(err);

    // Allocate double-buffered pinned + device memory slots
    for (int i = 0; i < kNumSlots; ++i)
    {
        BufferSlot& s = slots_[i];
        cudaMallocHost(&s.h_input_pinned, inputSize_);
        cudaMallocHost(&s.h_output_pinned, outputSize_);
        cudaMalloc(&s.d_input, inputSize_);
        cudaMalloc(&s.d_output, outputSize_);
    }

    if (maxBatchSize_ > 1)
        allocateBatchBuffers();

    LOG_INFO << "TRTBackend initialized, engine: " << config.path
             << ", maxBatchSize: " << maxBatchSize_
             << ", stream + " << kNumSlots << " pinned-memory slots";
}

TRTBackend::~TRTBackend()
{
    cudaStreamDestroy(stream_);
    for (int i = 0; i < kNumSlots; ++i)
    {
        BufferSlot& s = slots_[i];
        if (s.h_input_pinned)  cudaFreeHost(s.h_input_pinned);
        if (s.h_output_pinned) cudaFreeHost(s.h_output_pinned);
        if (s.d_input)         cudaFree(s.d_input);
        if (s.d_output)        cudaFree(s.d_output);
    }
    if (h_batch_input_)  cudaFreeHost(h_batch_input_);
    if (h_batch_output_) cudaFreeHost(h_batch_output_);
    if (d_batch_input_)  cudaFree(d_batch_input_);
    if (d_batch_output_) cudaFree(d_batch_output_);
}

// ---------------------------------------------------------------------------
// Slot management
// ---------------------------------------------------------------------------
int TRTBackend::acquireSlot()
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
    return -1;
}

void TRTBackend::releaseSlot(int idx)
{
    {
        std::lock_guard<std::mutex> lock(slot_pool_mutex_);
        slots_[idx].in_use = false;
    }
    slot_pool_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// Engine loading
// ---------------------------------------------------------------------------
bool TRTBackend::loadEngine(const std::string& enginePath)
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

    // Find input/output binding info (use config-driven names)
    int nbTensors = engine_->getNbIOTensors();
    for (int i = 0; i < nbTensors; i++)
    {
        const char* name = engine_->getIOTensorName(i);
        auto mode = engine_->getTensorIOMode(name);
        auto dims = engine_->getTensorShape(name);

        // Dynamic engine: getTensorShape returns -1 for dynamic dims
        if (dims.nbDims > 0 && dims.d[0] == -1)
        {
            dims.d[0] = std::max(8, maxBatchSize_);
            LOG_INFO << "Dynamic engine detected, using max batch " << dims.d[0]
                     << " for tensor: " << name;
        }

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

// ---------------------------------------------------------------------------
// Engine builder (static)
// ---------------------------------------------------------------------------
bool TRTBackend::buildEngine(const std::string& onnxPath,
                              const std::string& enginePath,
                              const ModelConfig& config)
{
    Logger localLogger;
    auto builder = std::unique_ptr<nvinfer1::IBuilder>(nvinfer1::createInferBuilder(localLogger));
    if (!builder)
    {
        LOG_ERROR << "Failed to create TensorRT builder";
        return false;
    }

    const auto explicitBatch = 1U << static_cast<uint32_t>(
        nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    auto network = std::unique_ptr<nvinfer1::INetworkDefinition>(
        builder->createNetworkV2(explicitBatch));
    if (!network)
    {
        LOG_ERROR << "Failed to create network definition";
        return false;
    }

    auto parser = std::unique_ptr<nvonnxparser::IParser>(
        nvonnxparser::createParser(*network, localLogger));
    if (!parser)
    {
        LOG_ERROR << "Failed to create ONNX parser";
        return false;
    }

    if (!parser->parseFromFile(onnxPath.c_str(),
                                static_cast<int>(nvinfer1::ILogger::Severity::kWARNING)))
    {
        LOG_ERROR << "Failed to parse ONNX model: " << onnxPath;
        return false;
    }

    auto trtConfig = std::unique_ptr<nvinfer1::IBuilderConfig>(builder->createBuilderConfig());
    trtConfig->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 1ULL << 30);

    if (enginePath.find("int8") != std::string::npos)
    {
        LOG_WARN << "TRT builder: INT8 model requested but calibration not supported; "
                    "building FP16 engine, saved as '" << enginePath << "' (filename implies INT8)";
    }
    if (builder->platformHasFastFp16())
    {
        trtConfig->setFlag(nvinfer1::BuilderFlag::kFP16);
        LOG_INFO << "TRT builder: FP16 enabled";
    }

    // Dynamic batch optimization profile — use config-driven dim names and sizes
    int c = config.input.channels;
    int h = config.input.preferred_height;
    int w = config.input.preferred_width;
    int maxBatch = std::max(8, config.max_batch_size);

    const char* inputName = config.input.name.c_str();

    auto profile = builder->createOptimizationProfile();
    profile->setDimensions(inputName, nvinfer1::OptProfileSelector::kMIN,
                           nvinfer1::Dims4{1, c, h, w});
    profile->setDimensions(inputName, nvinfer1::OptProfileSelector::kOPT,
                           nvinfer1::Dims4{maxBatch / 2, c, h, w});
    profile->setDimensions(inputName, nvinfer1::OptProfileSelector::kMAX,
                           nvinfer1::Dims4{maxBatch, c, h, w});
    trtConfig->addOptimizationProfile(profile);
    LOG_INFO << "TRT builder: dynamic batch profile [" << 1 << ", "
             << maxBatch / 2 << ", " << maxBatch << "]";

    auto plan = std::unique_ptr<nvinfer1::IHostMemory>(
        builder->buildSerializedNetwork(*network, *trtConfig));
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
    out.write(static_cast<const char*>(plan->data()),
              static_cast<std::streamsize>(plan->size()));
    if (!out)
    {
        LOG_ERROR << "Failed to write engine file: " << enginePath;
        return false;
    }

    LOG_INFO << "TRT engine built: " << enginePath
             << " (" << plan->size() / 1024 / 1024 << " MB)";
    return true;
}

// ---------------------------------------------------------------------------
// Batch buffer allocation
// ---------------------------------------------------------------------------
void TRTBackend::allocateBatchBuffers()
{
    int c = config_.input.channels;
    int h = config_.input.preferred_height;
    int w = config_.input.preferred_width;
    perSampleInputSize_ = static_cast<size_t>(c) * h * w * sizeof(float);
    perSampleOutputSize_ = outputSize_;

    size_t batchInputBytes  = static_cast<size_t>(maxBatchSize_) * perSampleInputSize_;
    size_t batchOutputBytes = static_cast<size_t>(maxBatchSize_) * perSampleOutputSize_;

    cudaMallocHost(&h_batch_input_, batchInputBytes);
    cudaMallocHost(&h_batch_output_, batchOutputBytes);
    cudaMalloc(&d_batch_input_, batchInputBytes);
    cudaMalloc(&d_batch_output_, batchOutputBytes);
}

// ---------------------------------------------------------------------------
// Inference
// ---------------------------------------------------------------------------
std::vector<float> TRTBackend::infer(const std::vector<float>& input,
                                      const std::vector<int64_t>& inputShape,
                                      std::vector<int64_t>& outputShape)
{
    if (!context_)
        return {};

    int slotIdx = acquireSlot();

    int c = config_.input.channels;
    int h = config_.input.preferred_height;
    int w = config_.input.preferred_width;

    // Copy preprocessed input to pinned memory
    size_t byteCount = input.size() * sizeof(float);
    std::memcpy(slots_[slotIdx].h_input_pinned, input.data(), byteCount);

    // GPU section
    {
        std::lock_guard<std::mutex> lock(gpu_mutex_);

        cudaMemcpyAsync(slots_[slotIdx].d_input, slots_[slotIdx].h_input_pinned,
                        byteCount, cudaMemcpyHostToDevice, stream_);

        nvinfer1::Dims4 inputDims{1, c, h, w};
        context_->setInputShape(config_.input.name.c_str(), inputDims);
        context_->setInputTensorAddress(config_.input.name.c_str(), slots_[slotIdx].d_input);
        context_->setOutputTensorAddress(config_.output.name.c_str(), slots_[slotIdx].d_output);
        context_->enqueueV3(stream_);

        cudaMemcpyAsync(slots_[slotIdx].h_output_pinned, slots_[slotIdx].d_output,
                        outputSize_, cudaMemcpyDeviceToHost, stream_);

        cudaStreamSynchronize(stream_);
    }

    size_t numElements = outputSize_ / sizeof(float);
    std::vector<float> result(slots_[slotIdx].h_output_pinned,
                              slots_[slotIdx].h_output_pinned + numElements);

    releaseSlot(slotIdx);

    outputShape = {1, static_cast<int64_t>(numElements)};
    return result;
}

std::vector<float> TRTBackend::inferBatch(const std::vector<float>& batchInput,
                                           const std::vector<int64_t>& batchShape,
                                           std::vector<int64_t>& outputShape)
{
    int batchSize = static_cast<int>(batchShape[0]);
    if (batchSize == 0)
        return {};

    if (batchSize > maxBatchSize_ || !context_ || !h_batch_input_)
    {
        // Fallback: chunk into maxBatchSize_ slices, each sent to the GPU in one
        // enqueue. Avoids per-sample slot acquire/release + H2D/D2H round-trips.
        int c = config_.input.channels;
        int h = config_.input.preferred_height;
        int w = config_.input.preferred_width;
        size_t perSampleElems = static_cast<size_t>(c) * h * w;

        std::vector<float> allResults;
        size_t numOut = outputSize_ / sizeof(float);
        allResults.reserve(batchSize * numOut);

        int chunkSize = maxBatchSize_;
        if (chunkSize < 1) chunkSize = 1;
        for (int i = 0; i < batchSize; i += chunkSize)
        {
            int cur = std::min(chunkSize, batchSize - i);
            auto chunkBegin = batchInput.begin() + i * perSampleElems;
            auto chunkEnd   = chunkBegin + cur * perSampleElems;
            std::vector<float> chunkData(chunkBegin, chunkEnd);
            std::vector<int64_t> chunkShape = {cur, c, h, w};
            std::vector<int64_t> chunkOutShape;
            auto chunkResult = inferBatch(chunkData, chunkShape, chunkOutShape);
            allResults.insert(allResults.end(), chunkResult.begin(), chunkResult.end());
        }
        outputShape = {batchSize, static_cast<int64_t>(allResults.size() / batchSize)};
        return allResults;
    }

    int c = config_.input.channels;
    int h = config_.input.preferred_height;
    int w = config_.input.preferred_width;

    // Copy preprocessed batch input to pinned memory
    size_t batchInputBytes = batchInput.size() * sizeof(float);
    std::memcpy(h_batch_input_, batchInput.data(), batchInputBytes);

    // GPU section
    {
        std::lock_guard<std::mutex> lock(gpu_mutex_);

        cudaMemcpyAsync(d_batch_input_, h_batch_input_,
                        batchInputBytes, cudaMemcpyHostToDevice, stream_);

        nvinfer1::Dims4 inputDims{batchSize, c, h, w};
        if (!context_->setInputShape(config_.input.name.c_str(), inputDims))
        {
            LOG_ERROR << "TRT setInputShape failed for batch " << batchSize
                      << " — engine may lack dynamic batch support";
            return {};
        }
        context_->setInputTensorAddress(config_.input.name.c_str(), d_batch_input_);
        context_->setOutputTensorAddress(config_.output.name.c_str(), d_batch_output_);
        if (!context_->enqueueV3(stream_))
        {
            LOG_ERROR << "TRT enqueueV3 failed for batch " << batchSize;
            return {};
        }

        size_t batchOutputBytes = static_cast<size_t>(batchSize) * perSampleOutputSize_;
        auto cudaErr = cudaMemcpyAsync(h_batch_output_, d_batch_output_,
                                       batchOutputBytes, cudaMemcpyDeviceToHost, stream_);
        if (cudaErr != cudaSuccess)
            LOG_ERROR << "cudaMemcpyAsync D2H failed: " << cudaGetErrorString(cudaErr);

        cudaErr = cudaStreamSynchronize(stream_);
        if (cudaErr != cudaSuccess)
            LOG_ERROR << "cudaStreamSynchronize failed: " << cudaGetErrorString(cudaErr);
    }

    size_t numClasses = perSampleOutputSize_ / sizeof(float);
    size_t totalElements = static_cast<size_t>(batchSize) * numClasses;
    std::vector<float> result(h_batch_output_, h_batch_output_ + totalElements);
    outputShape = {batchSize, static_cast<int64_t>(numClasses)};
    return result;
}

} // namespace inference
