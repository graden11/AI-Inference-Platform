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

    int elemCount = targetC_ * targetH_ * targetW_;
    std::vector<float> input(elemCount);

    // One-pass float resize: stbi decodes uint8; resize directly into float tensor.
    // STBIR_TYPE_UINT8: no sRGB decoding, raw uint8→float conversion in stbir.
    // Output is float values in [0,255] range — normalize with /255.0f below.
    // Eliminates the intermediate uint8 resized buffer (~150 KB savings).
    stbir_resize(data, w, h, 0,
                 input.data(), targetW_, targetH_, 0,
                 static_cast<stbir_pixel_layout>(targetC_),
                 STBIR_TYPE_UINT8,
                 STBIR_EDGE_CLAMP, STBIR_FILTER_DEFAULT);
    stbi_image_free(data);

    // Normalize + transpose. stbir output is float [0,255] — divide by 255.
    if (hwcLayout_) {
        for (int i = 0; i < elemCount; ++i) {
            int c = i % targetC_;
            input[i] = (input[i] / 255.0f - mean_[c]) / std_[c];
        }
    } else {
        // HWC source → CHW target: fused transpose + normalize.
        // Loop order y→x→c for cache-friendly sequential HWC reads.
        std::vector<float> transposed(elemCount);
        for (int y = 0; y < targetH_; ++y) {
            for (int x = 0; x < targetW_; ++x) {
                int hwcBase = (y * targetW_ + x) * targetC_;
                for (int c = 0; c < targetC_; ++c) {
                    int chwIdx = (c * targetH_ + y) * targetW_ + x;
                    float val = input[hwcBase + c] / 255.0f;
                    transposed[chwIdx] = (val - mean_[c]) / std_[c];
                }
            }
        }
        return transposed;
    }

    return input;
}

} // namespace inference
