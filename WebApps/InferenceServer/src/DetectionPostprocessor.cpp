#include "../include/DetectionPostprocessor.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace inference {

DetectionPostprocessor::DetectionPostprocessor(const ModelConfig& config)
    : confidenceThreshold_(config.confidence_threshold)
    , nmsThreshold_(config.nms_threshold)
    , maxDetections_(config.max_detections)
{}

float DetectionPostprocessor::iou(const float* a, const float* b) {
    float ix1 = std::max(a[0], b[0]), iy1 = std::max(a[1], b[1]);
    float ix2 = std::min(a[2], b[2]), iy2 = std::min(a[3], b[3]);
    float iw = std::max(0.0f, ix2 - ix1), ih = std::max(0.0f, iy2 - iy1);
    float iarea = iw * ih;
    float uarea = (a[2]-a[0])*(a[3]-a[1]) + (b[2]-b[0])*(b[3]-b[1]) - iarea;
    return uarea > 0 ? iarea / uarea : 0;
}

std::vector<int> DetectionPostprocessor::nms(
    const std::vector<float>& boxes, const std::vector<float>& scores,
    float nmsThresh, float minConf)
{
    int n = (int)scores.size();
    std::vector<int> idx(n);
    for (int i = 0; i < n; ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return scores[a] > scores[b]; });
    std::vector<int> keep;
    for (int i : idx) {
        if (scores[i] < minConf) continue;
        bool ok = true;
        for (int k : keep) {
            if (iou(&boxes[i*4], &boxes[k*4]) >= nmsThresh) { ok = false; break; }
        }
        if (ok) keep.push_back(i);
    }
    return keep;
}

static inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

nlohmann::json DetectionPostprocessor::postprocess(
    const InferenceOutput& output, const std::vector<std::string>& labels)
{
    // YOLOv8 ONNX output: [1, 84, 8400]
    // 84 = 4 bbox (cx,cy,w,h) + 80 class scores
    // PyTorch layout: field-major (first 8400 = all anchors' field[0], etc.)
    // 8400 anchors: scale 0 80x80(stride=8), scale 1 40x40(stride=16), scale 2 20x20(stride=32)

    const float* raw = output.data.data();
    size_t total = output.totalElements();

    if (total == 0) {
        nlohmann::json r; r["status"]="ok"; r["task_type"]="detection";
        r["summary"]="no objects"; r["detections"]=nlohmann::json::array();
        return r;
    }

    size_t nc = 0, na = 0;
    if (output.shape.size() >= 3) { nc = (size_t)output.shape[1]; na = (size_t)output.shape[2]; }
    else { na = total / 6; nc = 6; }

    size_t numClasses = (nc > 5) ? (nc - 4) : 1;

    // YOLOv8 strides and grid sizes
    const int strides[3] = {8, 16, 32};
    const int gridSizes[3] = {80, 40, 20};
    int anchorOffsets[3] = {0, 6400, 8000}; // cumulative anchor counts

    std::vector<float> boxesBuf, scoresBuf;
    struct Det { float x1,y1,x2,y2,score; int cls; };
    std::vector<Det> cands;

    for (int scale = 0; scale < 3; ++scale) {
        int stride = strides[scale];
        int gs = gridSizes[scale];
        int offset = anchorOffsets[scale];

        for (int gy = 0; gy < gs; ++gy) {
            for (int gx = 0; gx < gs; ++gx) {
                int a = offset + gy * gs + gx;

                // Read box fields: raw[f*8400 + a] for field f=0..3
                float cx_raw = raw[0*8400 + a];
                float cy_raw = raw[1*8400 + a];
                float w_raw  = raw[2*8400 + a];
                float h_raw  = raw[3*8400 + a];

                // Decode to pixels
                float cx = (sigmoid(cx_raw) * 2.0f - 0.5f + (float)gx) * (float)stride;
                float cy = (sigmoid(cy_raw) * 2.0f - 0.5f + (float)gy) * (float)stride;
                float w  = std::pow(sigmoid(w_raw) * 2.0f, 2.0f) * (float)stride;
                float h  = std::pow(sigmoid(h_raw) * 2.0f, 2.0f) * (float)stride;

                float bestScore = 0;
                int bestCls = 0;
                for (size_t c = 0; c < numClasses; ++c) {
                    float s = sigmoid(raw[(4 + c) * 8400 + a]);
                    if (s > bestScore) { bestScore = s; bestCls = (int)c; }
                }
                if (bestScore < confidenceThreshold_) continue;

                float x1 = cx - w * 0.5f, y1 = cy - h * 0.5f;
                float x2 = cx + w * 0.5f, y2 = cy + h * 0.5f;

                cands.push_back({x1,y1,x2,y2,bestScore,bestCls});
                boxesBuf.insert(boxesBuf.end(), {x1,y1,x2,y2});
                scoresBuf.push_back(bestScore);
            }
        }
    }

    auto keep = nms(boxesBuf, scoresBuf, nmsThreshold_, confidenceThreshold_);
    if ((int)keep.size() > maxDetections_) {
        std::sort(keep.begin(), keep.end(), [&](int a,int b){return scoresBuf[a]>scoresBuf[b];});
        keep.resize(maxDetections_);
    }

    nlohmann::json detArr = nlohmann::json::array();
    std::ostringstream sum; sum << std::fixed << std::setprecision(1);
    for (size_t i = 0; i < keep.size(); ++i) {
        auto& d = cands[keep[i]];
        auto& lbl = (d.cls < (int)labels.size()) ? labels[d.cls] : std::string("unknown");
        nlohmann::json det;
        det["class_id"]=d.cls; det["label"]=lbl; det["confidence"]=d.score*100.0f;
        det["bbox"]["x1"]=d.x1; det["bbox"]["y1"]=d.y1;
        det["bbox"]["x2"]=d.x2; det["bbox"]["y2"]=d.y2;
        detArr.push_back(det);
        if (i==0) sum << "检测到 " << lbl << "（" << (d.score*100.f) << "%）";
        else if (i==1) sum << "、" << lbl << "（" << (d.score*100.f) << "%）";
    }
    if (keep.empty()) sum << "no objects detected";

    nlohmann::json r;
    r["status"]="ok"; r["task_type"]="detection";
    r["summary"]=sum.str(); r["detections"]=detArr;
    return r;
}

std::vector<nlohmann::json> DetectionPostprocessor::postprocessBatch(
    const InferenceOutput& out, int bs, const std::vector<std::string>& labels)
{
    std::vector<nlohmann::json> r; r.reserve(bs);
    for (int i=0;i<bs;++i) {
        InferenceOutput s; s.data=out.data; s.shape=out.shape;
        r.push_back(postprocess(s,labels));
    }
    return r;
}

} // namespace inference
