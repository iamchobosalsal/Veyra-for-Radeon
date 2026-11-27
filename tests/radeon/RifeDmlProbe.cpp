// SPDX-License-Identifier: GPL-3.0-only
#include "FgProbeSupport.h"
#include "RifePairFiles.h"
#include "veyra/pipeline/RifeDmlBackend.h"
#include <chrono>

int main(int argc, char **argv) {
  try {
    require(argc == 6, "usage: rife_dml_probe runtime out-dir walk|orbit "
                       "height inferenceScale");
    const auto root = std::filesystem::absolute(argv[1]),
               out = std::filesystem::absolute(argv[2]);
    std::filesystem::create_directories(out);
    unsigned height = unsigned(std::stoi(argv[4])), width = height * 16 / 9;
    float scale = std::stof(argv[5]);
    require(height >= 360 && height <= 2160, "height");
    const bool files = std::string(argv[3]) == "pairs";
    Scene scene{width, height, std::string(argv[3]) == "orbit",
                8.f * height / 360};
    Host h;
    RifeDmlBackend backend;
    h.begin();
    auto started = std::chrono::steady_clock::now();
    backend.open(h.device.Get(), h.list.Get(), root, width, height, scale);
    h.complete();
    std::cout << "INITIALIZED graphs=" << backend.graphCount() << " seconds="
              << std::chrono::duration<double>(
                     std::chrono::steady_clock::now() - started)
                     .count()
              << std::endl;
    auto a = makeTexture(h.device.Get(), width, height,
                         DXGI_FORMAT_R8G8B8A8_UNORM, false),
         b = makeTexture(h.device.Get(), width, height,
                         DXGI_FORMAT_R8G8B8A8_UNORM, false),
         output = makeTexture(h.device.Get(), width, height,
                              DXGI_FORMAT_R8G8B8A8_UNORM, true);
    Transfer ua(h, a.Get(), 4, true), ub(h, b.Get(), 4, true),
        read(h, output.Get(), 4, false);
    StateTracker states;
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2;
    ComPtr<ID3D12QueryHeap> query;
    hr(h.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&query)), "timestamp heap");
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 16;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> timing;
    hr(h.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                         D3D12_RESOURCE_STATE_COPY_DEST,
                                         nullptr, IID_PPV_ARGS(&timing)),
       "timestamps");
    UINT64 freq = 0;
    hr(h.queue->GetTimestampFrequency(&freq), "timestamp frequency");
    std::vector<Pixel> previous(size_t(width) * height),
        current(previous.size()), truth(previous.size());
    Error edge, whole, background, hold;
    double gpuMs = 0;
    if (files) {
      previous = readRifePpm(out / "source-a.ppm", width, height);
      current = readRifePpm(out / "source-b.ppm", width, height);
      truth = readRifePpm(out / "truth.ppm", width, height);
    }
    for (unsigned frame = 0; frame < 14; ++frame) {
      if (!files)
        for (unsigned y = 0; y < height; y++)
          for (unsigned x = 0; x < width; x++) {
            size_t i = size_t(y) * width + x;
            previous[i] = scene.pixel(x, y, float(frame) - 1);
            current[i] = scene.pixel(x, y, float(frame));
            truth[i] = scene.pixel(x, y, float(frame) - .5f);
          }
      ua.write(previous.data());
      ub.write(current.data());
      h.begin();
      for (auto pair : {std::pair{a.Get(), &ua}, std::pair{b.Get(), &ub}}) {
        states.transition(h.list.Get(), pair.first,
                          D3D12_RESOURCE_STATE_COPY_DEST);
        pair.second->copy(h.list.Get(), pair.first, true);
        states.transition(h.list.Get(), pair.first,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      }
      states.transition(h.list.Get(), output.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      h.list->EndQuery(query.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
      backend.record(h.list.Get(), a.Get(), b.Get(), output.Get());
      h.list->EndQuery(query.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
      h.list->ResolveQueryData(query.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                               timing.Get(), 0);
      states.uavBarrier(h.list.Get(), output.Get());
      states.transition(h.list.Get(), output.Get(),
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
      read.copy(h.list.Get(), output.Get(), false);
      h.complete();
      auto generated = read.read();
      if (frame >= 4) {
        uint64_t *ticks = nullptr;
        hr(timing->Map(0, nullptr, reinterpret_cast<void **>(&ticks)),
           "timing map");
        gpuMs += double(ticks[1] - ticks[0]) * 1000 / freq;
        timing->Unmap(0, nullptr);
        for (unsigned y = 24; y < height - 24; y++)
          for (unsigned x = 24; x < width - 24; x++) {
            size_t i = size_t(y) * width + x;
            float d = scene.distance(x + .5f, y + .5f, float(frame) - .5f);
            whole.add(generated.data() + 4 * i, truth[i]);
            if (std::abs(d) < 12.f * height / 360) {
              edge.add(generated.data() + 4 * i, truth[i]);
              hold.add(current[i].data(), truth[i]);
            }
            if (d > 40.f * height / 360)
              background.add(generated.data() + 4 * i, truth[i]);
          }
      }
      if (frame == 10) {
        save(out / "generated.ppm", width, height, generated.data());
        save(out / "truth.ppm", width, height,
             reinterpret_cast<uint8_t *>(truth.data()));
        save(out / "source-a.ppm", width, height,
             reinterpret_cast<uint8_t *>(previous.data()));
        save(out / "source-b.ppm", width, height,
             reinterpret_cast<uint8_t *>(current.data()));
      }
    }
    std::cout << "PIXEL_RESULT {\"scene\":\"" << argv[3]
              << "\",\"width\":" << width << ",\"height\":" << height
              << ",\"scale\":" << scale << ",\"edgeMae\":" << edge.mean()
              << ",\"edgeBadRatio\":" << edge.badRatio()
              << ",\"wholeMae\":" << whole.mean()
              << ",\"farMae\":" << background.mean()
              << ",\"holdEdgeMae\":" << hold.mean()
              << ",\"gpuMs\":" << gpuMs / 10 << "}" << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "ERROR " << e.what() << std::endl;
    return 2;
  }
}
