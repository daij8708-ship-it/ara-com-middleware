#include "communication/runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>

using namespace ara::com;

int main(int argc, char** argv) {
    const std::size_t iterations = argc > 1
        ? static_cast<std::size_t>(std::stoul(argv[1])) : 10000U;
    if (iterations == 0) {
        std::cerr << "iterations must be greater than zero\n";
        return 2;
    }

    auto backend = runtime::create_backend(runtime::BackendType::NULL_BACKEND);
    ServiceConfig config;
    config.service_id = 0x1234;
    config.instance_id = 1;
    backend->init(config);
    backend->register_method_handler(
        config.service_id, config.instance_id, 0x0101,
        [](const std::vector<std::uint8_t>& request) { return request; });
    backend->start();

    const std::vector<std::uint8_t> request(64, 0x5A);
    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::size_t i = 0; i < iterations + 1000U; ++i) {
        std::vector<std::uint8_t> response;
        const auto begin = std::chrono::steady_clock::now();
        const auto status = backend->call_method_sync(
            config.service_id, config.instance_id, 0x0101,
            request, response, 1000);
        const auto end = std::chrono::steady_clock::now();
        if (status != ComRet::OK || response != request) return 1;
        if (i >= 1000U) {
            samples.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
        }
    }
    backend->stop();

    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](double p) {
        const auto index = static_cast<std::size_t>((samples.size() - 1U) * p);
        return samples[index];
    };
    const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) /
                        static_cast<double>(samples.size());

    std::cout << std::fixed << std::setprecision(3)
              << "backend=null payload_bytes=64 iterations=" << iterations << '\n'
              << "latency_us mean=" << mean
              << " p50=" << percentile(0.50)
              << " p95=" << percentile(0.95)
              << " p99=" << percentile(0.99) << '\n';
    return 0;
}
