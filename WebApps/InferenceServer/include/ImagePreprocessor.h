#pragma once

#include "Preprocessor.h"

namespace inference {

/// Image preprocessor that decodes JPEG/PNG via stb_image,
/// resizes, normalizes, and outputs CHW float tensor.
class ImagePreprocessor : public Preprocessor
{
public:
    explicit ImagePreprocessor(const ModelConfig& config);

    std::vector<float> preprocess(const std::vector<uint8_t>& imageBytes) override;

private:
    int targetW_, targetH_, targetC_;
    bool hwcLayout_;
    std::vector<float> mean_;
    std::vector<float> std_;
};

} // namespace inference
