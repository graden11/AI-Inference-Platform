#include "../include/OnnxBackend.h"

#include <algorithm>
#include <muduo/base/Logging.h>

namespace inference {

OnnxBackend::OnnxBackend(const ModelConfig& config)
    : env_(ORT_LOGGING_LEVEL_WARNING, "onnx"),
      memInfo_("Cpu", OrtArenaAllocator, 0, OrtMemTypeDefault),
      inputName_(config.input.name),
      outputName_(config.output.name),
      maxBatchSize_(config.max_batch_size)
{
    sessionOpts_.SetIntraOpNumThreads(1);
    session_ = std::make_unique<Ort::Session>(env_, config.path.c_str(), sessionOpts_);

    // Auto-detect real tensor names from model (when config is still at defaults)
    Ort::AllocatorWithDefaultOptions alloc;
    if (inputName_ == "input") {
        auto namePtr = session_->GetInputNameAllocated(0, alloc);
        if (namePtr) {
            inputName_ = namePtr.get();
            LOG_INFO << "OnnxBackend: auto-detected input name = " << inputName_;
        }
    }
    if (outputName_ == "output") {
        auto namePtr = session_->GetOutputNameAllocated(0, alloc);
        if (namePtr) {
            outputName_ = namePtr.get();
            LOG_INFO << "OnnxBackend: auto-detected output name = " << outputName_;
        }
    }

    // Auto-detect input shape from model
    auto inputTypeInfo = session_->GetInputTypeInfo(0);
    auto inputTensorInfo = inputTypeInfo.GetTensorTypeAndShapeInfo();
    auto inputShape = inputTensorInfo.GetShape();

    // If the ONNX model has a fixed batch dimension (shape[0] > 0, e.g. 1 for
    // squeezenet or yolo exported with explicit batch=1), cap maxBatchSize_ so
    // that ModelPipeline::predictBatch falls back to per-sample inference
    // instead of passing N>1 to session_->Run() and hitting a dimension error.
    // Dynamic-batch models have shape[0] == -1 or 0 and are not capped.
    if (!inputShape.empty() && inputShape[0] > 0) {
        int staticBatch = static_cast<int>(inputShape[0]);
        maxBatchSize_ = std::min(maxBatchSize_, staticBatch);
        LOG_INFO << "OnnxBackend: static batch dim = " << staticBatch
                 << " (maxBatchSize capped to " << maxBatchSize_ << ")";
    }

    if (inputShape.size() >= 3) {
        detectedChannels_ = static_cast<int>(inputShape[1]);
        detectedHeight_   = static_cast<int>(inputShape[2]);
        detectedWidth_    = static_cast<int>(inputShape[3]);
        // Infer layout: NHWC has C in last dim, NCHW has C in dim 1
        if (inputShape[1] > 4 && inputShape[inputShape.size()-1] <= 4) {
            // NHWC layout: spatial dims come before channels
            detectedHeight_   = static_cast<int>(inputShape[1]);
            detectedWidth_    = static_cast<int>(inputShape[2]);
            detectedChannels_ = static_cast<int>(inputShape[3]);
            detectedLayout_ = "hwc";
        } else {
            detectedLayout_ = "chw";
        }
        detectedShape_ = true;
        LOG_INFO << "OnnxBackend: auto-detected shape: " << detectedChannels_
                 << "x" << detectedHeight_ << "x" << detectedWidth_
                 << " layout=" << detectedLayout_;
    }

    // Auto-detect task type from output shape
    auto outputTypeInfo = session_->GetOutputTypeInfo(0);
    auto outputTensorInfo = outputTypeInfo.GetTensorTypeAndShapeInfo();
    auto outShape = outputTensorInfo.GetShape();
    int dims = static_cast<int>(outShape.size());
    if (dims == 2) {
        // 2D output could be classification [N, classes] or feature_extraction [N, emb_dim]
        // Heuristic: >2000 dims is almost certainly embedding; also check output name
        int64_t outDim = outShape.size() >= 2 ? outShape[1] : 0;
        std::string outName = outputName_;
        std::transform(outName.begin(), outName.end(), outName.begin(), ::tolower);
        if (outDim > 2000 || outName.find("embed") != std::string::npos) {
            detectedTask_ = "feature_extraction";
        } else {
            detectedTask_ = "classification";
        }
    } else if (dims == 3) {
        detectedTask_ = "detection";
    } else if (dims >= 4) {
        detectedTask_ = "segmentation";
    }
    if (!detectedTask_.empty()) {
        LOG_INFO << "OnnxBackend: auto-detected task = " << detectedTask_ << " (output dims=" << dims << ")";
    }

    LOG_INFO << "OnnxBackend initialized, model: " << config.path
             << ", maxBatchSize: " << maxBatchSize_;
}

std::vector<float> OnnxBackend::infer(const std::vector<float>& input,
                                       const std::vector<int64_t>& inputShape,
                                       std::vector<int64_t>& outputShape)
{
    Ort::Value inputValue = Ort::Value::CreateTensor<float>(
        memInfo_,
        const_cast<float*>(input.data()),
        input.size(),
        inputShape.data(),
        inputShape.size());

    const char* inputNames[]  = {inputName_.c_str()};
    const char* outputNames[] = {outputName_.c_str()};

    std::lock_guard<std::mutex> lock(inferenceMutex_);
    auto outputValues = session_->Run(Ort::RunOptions{},
                                      inputNames, &inputValue, 1,
                                      outputNames, 1);

    float* logits = outputValues[0].GetTensorMutableData<float>();
    auto typeInfo = outputValues[0].GetTensorTypeAndShapeInfo();
    auto outShape = typeInfo.GetShape();
    size_t elemCount = typeInfo.GetElementCount();

    outputShape.assign(outShape.begin(), outShape.end());
    return std::vector<float>(logits, logits + elemCount);
}

std::vector<float> OnnxBackend::inferBatch(const std::vector<float>& batchInput,
                                            const std::vector<int64_t>& batchShape,
                                            std::vector<int64_t>& outputShape)
{
    Ort::Value inputValue = Ort::Value::CreateTensor<float>(
        memInfo_,
        const_cast<float*>(batchInput.data()),
        batchInput.size(),
        batchShape.data(),
        batchShape.size());

    const char* inputNames[]  = {inputName_.c_str()};
    const char* outputNames[] = {outputName_.c_str()};

    std::lock_guard<std::mutex> lock(inferenceMutex_);
    auto outputValues = session_->Run(Ort::RunOptions{},
                                      inputNames, &inputValue, 1,
                                      outputNames, 1);

    float* logits = outputValues[0].GetTensorMutableData<float>();
    auto typeInfo = outputValues[0].GetTensorTypeAndShapeInfo();
    auto outShape = typeInfo.GetShape();
    size_t elemCount = typeInfo.GetElementCount();

    outputShape.assign(outShape.begin(), outShape.end());
    return std::vector<float>(logits, logits + elemCount);
}

InferenceOutput OnnxBackend::inferMulti(const std::vector<float>& input,
                                         const std::vector<int64_t>& inputShape)
{
    InferenceOutput io;
    if (!session_) return io;

    Ort::Value inputValue = Ort::Value::CreateTensor<float>(
        memInfo_,
        const_cast<float*>(input.data()),
        input.size(),
        inputShape.data(),
        inputShape.size());

    const char* inputNames[]  = {inputName_.c_str()};
    const char* outputNames[] = {outputName_.c_str()};

    {
        std::lock_guard<std::mutex> lock(inferenceMutex_);
        auto outputValues = session_->Run(Ort::RunOptions{},
                                          inputNames, &inputValue, 1,
                                          outputNames, 1);

        // Transfer Ort::Value ownership into a shared_ptr so InferenceOutput
        // can outlive this stack frame without copying the tensor data.
        auto holder = std::make_shared<std::vector<Ort::Value>>(std::move(outputValues));
        float* logits = (*holder)[0].GetTensorMutableData<float>();
        auto typeInfo = (*holder)[0].GetTensorTypeAndShapeInfo();
        auto outShape = typeInfo.GetShape();
        size_t elemCount = typeInfo.GetElementCount();

        io.shape.assign(outShape.begin(), outShape.end());
        io.backendHandle = holder;
        io.dataPtr = logits;
        io.dataSize = elemCount;
    }
    return io;
}

InferenceOutput OnnxBackend::inferBatchMulti(const std::vector<float>& batchInput,
                                              const std::vector<int64_t>& batchShape)
{
    InferenceOutput io;
    if (!session_) return io;

    Ort::Value inputValue = Ort::Value::CreateTensor<float>(
        memInfo_,
        const_cast<float*>(batchInput.data()),
        batchInput.size(),
        batchShape.data(),
        batchShape.size());

    const char* inputNames[]  = {inputName_.c_str()};
    const char* outputNames[] = {outputName_.c_str()};

    {
        std::lock_guard<std::mutex> lock(inferenceMutex_);
        auto outputValues = session_->Run(Ort::RunOptions{},
                                          inputNames, &inputValue, 1,
                                          outputNames, 1);

        auto holder = std::make_shared<std::vector<Ort::Value>>(std::move(outputValues));
        float* logits = (*holder)[0].GetTensorMutableData<float>();
        auto typeInfo = (*holder)[0].GetTensorTypeAndShapeInfo();
        auto outShape = typeInfo.GetShape();
        size_t elemCount = typeInfo.GetElementCount();

        io.shape.assign(outShape.begin(), outShape.end());
        io.backendHandle = holder;
        io.dataPtr = logits;
        io.dataSize = elemCount;
    }
    return io;
}

} // namespace inference
