FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake libgtest-dev ninja-build && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DARA_COM_USE_VSOMEIP=OFF -DARA_COM_BUILD_TESTS=ON && \
    cmake --build build && ctest --test-dir build --output-on-failure

CMD ["./build/demo/demo_server", "config/services.json"]
