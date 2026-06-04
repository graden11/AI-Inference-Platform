#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include <stb_image_resize2.h>

#include "../include/ImagePreprocessor.h"

#include <algorithm>
#include <muduo/base/Logging.h>

namespace inference {

ImagePreprocessor::ImagePreprocessor(const ModelConfig& config)
    : targetW_(config.input.preferred_width),
      targetH_(config.input.preferred_height),
      targetC_(config.input.channels),
      hwcLayout_(config.input.layout == "hwc"),
      mean_(config.input.mean),
      std_(config.input.std)
{
}

std::vector<float> ImagePreprocessor::preprocess(const std::vector<uint8_t>& imageBytes)
{
    int w, h, channels;
    unsigned char* data = stbi_load_from_memory(
        imageBytes.data(), static_cast<int>(imageBytes.size()),
        &w, &h, &channels, targetC_);
    if (!data)
    {
        LOG_ERROR << "ImagePreprocessor: failed to decode image";
        return {};
    }

    // Resize to target dimensions using bilinear interpolation
    std::vector<uint8_t> resized(targetH_ * targetW_ * targetC_);
    stbir_resize_uint8_srgb(data, w, h, 0,
                            resized.data(), targetW_, targetH_, 0,
                            static_cast<stbir_pixel_layout>(targetC_));
    stbi_image_free(data);

    // Normalize, standardize
    int elemCount = targetC_ * targetH_ * targetW_;
    std::vector<float> input(elemCount);

    if (hwcLayout_) {
        for (int y = 0; y < targetH_; ++y)
            for (int x = 0; x < targetW_; ++x)
                for (int c = 0; c < targetC_; ++c) {
                    int idx = (y * targetW_ + x) * targetC_ + c;
                    float val = resized[idx] / 255.0f;
                    input[idx] = (val - mean_[c]) / std_[c];
                }
    } else {
        for (int c = 0; c < targetC_; ++c)
            for (int y = 0; y < targetH_; ++y)
                for (int x = 0; x < targetW_; ++x) {
                    int hwcIdx = (y * targetW_ + x) * targetC_ + c;
                    int chwIdx = (c * targetH_ + y) * targetW_ + x;
                    float val = resized[hwcIdx] / 255.0f;
                    input[chwIdx] = (val - mean_[c]) / std_[c];
                }
    }

    return input;
}

} // namespace inference
