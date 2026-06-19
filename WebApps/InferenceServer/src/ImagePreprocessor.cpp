#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include <stb_image_resize2.h>

#include "../include/ImagePreprocessor.h"

#include <algorithm>
#include <mutex>
#include <muduo/base/Logging.h>

namespace inference {

// stbi_load_from_memory() uses global/static state — serialize across threads.
// Scope is intentionally narrow: only the JPEG decode call, not resize/normalize.
static std::mutex gStbMutex;

ImagePreprocessor::ImagePreprocessor(const ModelConfig& config)
    : targetW_(config.input.preferred_width),
      targetH_(config.input.preferred_height),
      targetC_(config.input.channels),
      hwcLayout_(config.input.layout == "hwc"),
      mean_(config.input.mean),
      std_(config.input.std)
{
    scale_.resize(targetC_);
    bias_.resize(targetC_);
    for (int c = 0; c < targetC_; ++c) {
        float meanValue = c < static_cast<int>(mean_.size()) ? mean_[c] : 0.0f;
        float stdValue = c < static_cast<int>(std_.size()) ? std_[c] : 1.0f;
        scale_[c] = 1.0f / (255.0f * stdValue);
        bias_[c] = -meanValue / stdValue;
    }
}

bool ImagePreprocessor::preprocess(const std::vector<uint8_t>& imageBytes,
                                  std::vector<float>& output)
{
    const int elemCount = targetC_ * targetH_ * targetW_;
    if (elemCount <= 0)
    {
        LOG_ERROR << "ImagePreprocessor::preprocess: invalid input shape (c="
                  << targetC_ << " h=" << targetH_ << " w=" << targetW_ << ")";
        return false;
    }

    int w, h, channels;
    unsigned char* data;
    {
        std::lock_guard<std::mutex> lock(gStbMutex);
        data = stbi_load_from_memory(
            imageBytes.data(), static_cast<int>(imageBytes.size()),
            &w, &h, &channels, targetC_);
    }
    if (!data)
    {
        LOG_ERROR << "ImagePreprocessor: failed to decode image";
        return false;
    }

    // uint8 srgb → uint8 linear resize via stb. Cannot eliminate this intermediate
    // buffer because stbir_resize_uint8_srgb outputs uint8 only; a direct
    // uint8→float resize with stbir_resize(generic) proved unreliable at value range.
    // Thread-local: reused across calls on the same thread, avoiding 150KB malloc/free.
    thread_local static std::vector<uint8_t> resized;
    resized.resize(elemCount);
    stbir_resize_uint8_srgb(data, w, h, 0,
                            resized.data(), targetW_, targetH_, 0,
                            static_cast<stbir_pixel_layout>(targetC_));
    stbi_image_free(data);

    // Normalize + transpose into caller-owned buffer.
    output.resize(elemCount);

    if (hwcLayout_) {
        if (targetC_ == 3) {
            for (int i = 0; i < elemCount; i += 3) {
                output[i]     = resized[i]     * scale_[0] + bias_[0];
                output[i + 1] = resized[i + 1] * scale_[1] + bias_[1];
                output[i + 2] = resized[i + 2] * scale_[2] + bias_[2];
            }
        } else {
            for (int i = 0; i < elemCount; ++i) {
                int c = i % targetC_;
                output[i] = resized[i] * scale_[c] + bias_[c];
            }
        }
    } else {
        // HWC source → CHW target: fused transpose + normalize.
        if (targetC_ == 3) {
            float* c0 = output.data();
            float* c1 = c0 + targetH_ * targetW_;
            float* c2 = c1 + targetH_ * targetW_;
            for (int i = 0, p = 0; i < targetH_ * targetW_; ++i, p += 3) {
                c0[i] = resized[p]     * scale_[0] + bias_[0];
                c1[i] = resized[p + 1] * scale_[1] + bias_[1];
                c2[i] = resized[p + 2] * scale_[2] + bias_[2];
            }
        } else {
            for (int y = 0; y < targetH_; ++y) {
                for (int x = 0; x < targetW_; ++x) {
                    int hwcBase = (y * targetW_ + x) * targetC_;
                    for (int c = 0; c < targetC_; ++c) {
                        int chwIdx = (c * targetH_ + y) * targetW_ + x;
                        output[chwIdx] = resized[hwcBase + c] * scale_[c] + bias_[c];
                    }
                }
            }
        }
    }
    return true;
}

bool ImagePreprocessor::preprocessInto(const std::vector<uint8_t>& imageBytes,
                                       std::vector<float>& batchOutput,
                                       size_t offset)
{
    const int elemCount = targetC_ * targetH_ * targetW_;
    if (elemCount <= 0)
    {
        LOG_ERROR << "ImagePreprocessor::preprocessInto: invalid input shape (c="
                  << targetC_ << " h=" << targetH_ << " w=" << targetW_ << ")";
        return false;
    }
    const size_t elemCountZ = static_cast<size_t>(elemCount);
    if (offset > batchOutput.size() || elemCountZ > batchOutput.size() - offset)
    {
        LOG_ERROR << "ImagePreprocessor::preprocessInto: output buffer too small"
                  << " (size=" << batchOutput.size() << " offset=" << offset
                  << " elemCount=" << elemCountZ << ")";
        return false;
    }

    // Same decode+resize+normalize but writes directly into batchOutput[offset..]
    int w, h, channels;
    unsigned char* data;
    {
        std::lock_guard<std::mutex> lock(gStbMutex);
        data = stbi_load_from_memory(
            imageBytes.data(), static_cast<int>(imageBytes.size()),
            &w, &h, &channels, targetC_);
    }
    if (!data)
    {
        LOG_ERROR << "ImagePreprocessor: failed to decode image";
        return false;
    }

    thread_local static std::vector<uint8_t> resized;
    resized.resize(elemCount);
    stbir_resize_uint8_srgb(data, w, h, 0,
                            resized.data(), targetW_, targetH_, 0,
                            static_cast<stbir_pixel_layout>(targetC_));
    stbi_image_free(data);

    // batchOutput is already resized to hold the full batch.
    // Write this sample's normalized tensor directly at batchOutput[offset..].
    float* out = batchOutput.data() + offset;

    if (hwcLayout_) {
        if (targetC_ == 3) {
            for (int i = 0; i < elemCount; i += 3) {
                out[i]     = resized[i]     * scale_[0] + bias_[0];
                out[i + 1] = resized[i + 1] * scale_[1] + bias_[1];
                out[i + 2] = resized[i + 2] * scale_[2] + bias_[2];
            }
        } else {
            for (int i = 0; i < elemCount; ++i) {
                int c = i % targetC_;
                out[i] = resized[i] * scale_[c] + bias_[c];
            }
        }
    } else {
        if (targetC_ == 3) {
            float* c0 = out;
            float* c1 = c0 + targetH_ * targetW_;
            float* c2 = c1 + targetH_ * targetW_;
            for (int i = 0, p = 0; i < targetH_ * targetW_; ++i, p += 3) {
                c0[i] = resized[p]     * scale_[0] + bias_[0];
                c1[i] = resized[p + 1] * scale_[1] + bias_[1];
                c2[i] = resized[p + 2] * scale_[2] + bias_[2];
            }
        } else {
            for (int y = 0; y < targetH_; ++y) {
                for (int x = 0; x < targetW_; ++x) {
                    int hwcBase = (y * targetW_ + x) * targetC_;
                    for (int c = 0; c < targetC_; ++c) {
                        int chwIdx = (c * targetH_ + y) * targetW_ + x;
                        out[chwIdx] = resized[hwcBase + c] * scale_[c] + bias_[c];
                    }
                }
            }
        }
    }
    return true;
}

} // namespace inference
