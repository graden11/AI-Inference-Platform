# ============================================
# GPU production image — NVIDIA TensorRT inference
# Multi-stage: builds in Docker, no pre-compilation needed
# ============================================

# --- Stage 1: Build ---
FROM nvidia/cuda:12.6.0-devel-ubuntu22.04 AS builder

# Remove any pre-installed TensorRT libs to avoid version conflicts
# with the 10.16.1.11 packages installed below
RUN rm -f /usr/lib/x86_64-linux-gnu/libnvinfer* \
          /usr/lib/x86_64-linux-gnu/libnvonnxparser* \
          /usr/lib/x86_64-linux-gnu/libnvinfer_plugin*

RUN apt-get update && apt-get install -y \
    cmake g++ make libssl-dev \
    libmysqlclient-dev libmysqlcppconn-dev \
    nlohmann-json3-dev protobuf-compiler libprotobuf-dev \
    libspdlog-dev libhiredis-dev \
    libnvinfer-headers-dev=10.16.1.11-1+cuda13.2 \
    libnvinfer-safe-headers-dev=10.16.1.11-1+cuda13.2 \
    libnvinfer-headers-plugin-dev=10.16.1.11-1+cuda13.2 \
    libnvinfer-headers-python-plugin-dev=10.16.1.11-1+cuda13.2 \
    libnvinfer-dev=10.16.1.11-1+cuda13.2 \
    libnvinfer-plugin-dev=10.16.1.11-1+cuda13.2 \
    libnvonnxparsers-dev=10.16.1.11-1+cuda13.2 \
    libnvinfer10=10.16.1.11-1+cuda13.2 \
    libnvinfer-plugin10=10.16.1.11-1+cuda13.2 \
    libnvonnxparsers10=10.16.1.11-1+cuda13.2 \
    && rm -rf /var/lib/apt/lists/*

COPY . /project
WORKDIR /project/build
RUN cmake .. -DENABLE_TENSORRT=ON && make -j$(nproc)

# --- Stage 2: Runtime ---
# Both stages use the same devel image so all runtime libs are already
# installed by the builder apt step — no network needed in stage 2.
FROM nvidia/cuda:12.6.0-devel-ubuntu22.04

# Remove any pre-installed TensorRT libs to avoid version conflicts
RUN rm -f /usr/lib/x86_64-linux-gnu/libnvinfer* \
          /usr/lib/x86_64-linux-gnu/libnvonnxparser* \
          /usr/lib/x86_64-linux-gnu/libnvinfer_plugin*

# Copy all runtime libs directly from builder (NO apt install — offline)
COPY --from=builder /usr/lib/x86_64-linux-gnu/libmysqlcppconn.so* \
                    /usr/lib/x86_64-linux-gnu/libmysqlclient.so* \
                    /usr/lib/x86_64-linux-gnu/libhiredis.so* \
                    /usr/lib/x86_64-linux-gnu/libprotobuf*.so* \
                    /usr/lib/x86_64-linux-gnu/libfmt.so* \
                    /usr/lib/x86_64-linux-gnu/libssl.so* \
                    /usr/lib/x86_64-linux-gnu/libcurl.so* \
                    /usr/lib/x86_64-linux-gnu/libnghttp2.so* \
                    /usr/lib/x86_64-linux-gnu/libbrotli*.so* \
                    /usr/lib/x86_64-linux-gnu/libssh.so* \
                    /usr/lib/x86_64-linux-gnu/libpsl.so* \
                    /usr/lib/x86_64-linux-gnu/librtmp.so* \
                    /usr/lib/x86_64-linux-gnu/

# TensorRT runtime libs (from builder, matches linked version)
COPY --from=builder /usr/lib/x86_64-linux-gnu/libnvinfer.so* \
                    /usr/lib/x86_64-linux-gnu/libnvinfer_plugin.so* \
                    /usr/lib/x86_64-linux-gnu/libnvonnxparser.so* \
                    /usr/lib/x86_64-linux-gnu/libnvinfer_builder* \
                    /usr/lib/x86_64-linux-gnu/

# Matching libstdc++
COPY --from=builder /usr/lib/x86_64-linux-gnu/libstdc++.so.6 \
                    /usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.30 \
                    /usr/lib/x86_64-linux-gnu/

# ONNX Runtime libs
COPY third_party/onnxruntime/lib/      /usr/local/lib/
RUN ldconfig

# Binary + data
COPY --from=builder /project/build/simple_server /app/simple_server
COPY models/ /app/models/
COPY WebApps/InferenceServer/resource/ /WebApps/InferenceServer/resource/
COPY WebApps/InferenceServer/config.json /app/config.json
# Fix model paths — they point to ../WebApps/InferenceServer/models/ in the source config,
# inside container they are at /app/models/
RUN sed -i 's|../WebApps/InferenceServer/models/|models/|g; s|/project/WebApps/InferenceServer/models/|models/|g' /app/config.json

COPY docker-entrypoint.sh /app/
RUN chmod +x /app/docker-entrypoint.sh

# Non-root user (GPU access requires the container to be launched with --gpus)
RUN useradd -m -s /bin/bash appuser && \
    apt-get update && apt-get install -y curl && \
    rm -rf /var/lib/apt/lists/* && \
    chown -R appuser:appuser /app /WebApps/InferenceServer/resource /usr/local/lib
USER appuser

WORKDIR /app
EXPOSE 80

HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD curl -sf http://localhost:80/health || exit 1

ENTRYPOINT ["/app/docker-entrypoint.sh"]
