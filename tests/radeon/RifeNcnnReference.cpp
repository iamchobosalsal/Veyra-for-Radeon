// SPDX-License-Identifier: GPL-3.0-only
// Independent CPU reference. The external warp header contains the unchanged
// CPU forward function from the pinned MIT rife-ncnn-vulkan source.
#include "RifePairFiles.h"
#include "ncnn-warp-reference.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <net.h>
DEFINE_LAYER_CREATOR(Warp)
int main(int argc, char **argv) {
  try {
    if (argc != 5)
      throw std::runtime_error(
          "usage: ncnn_reference model pair-directory width height");
    unsigned w = std::stoi(argv[3]), h = std::stoi(argv[4]),
             pw = (w + 31) & ~31u, ph = (h + 31) & ~31u;
    std::filesystem::path root = argv[1], dir = argv[2];
    auto a = readRifePpm(dir / "source-a.ppm", w, h),
         b = readRifePpm(dir / "source-b.ppm", w, h);
    std::cerr << "reference input loaded\n";
    ncnn::Net net;
    net.opt.num_threads = 2;
    net.opt.use_packing_layout = false;
    net.opt.use_fp16_arithmetic = false;
    net.opt.use_fp16_storage = false;
    net.register_custom_layer("rife.Warp", Warp_layer_creator);
    if (net.load_param((root / "flownet.param").string().c_str()))
      throw std::runtime_error("ncnn parameters");
    std::cerr << "reference graph loaded\n";
    if (net.load_model((root / "flownet.bin").string().c_str()))
      throw std::runtime_error("ncnn weights");
    std::cerr << "reference weights loaded\n";
    ncnn::Mat ma(int(pw), int(ph), 3), mb(int(pw), int(ph), 3),
        mt(int(pw), int(ph), 1);
    ma.fill(0.f);
    mb.fill(0.f);
    mt.fill(.5f);
    for (unsigned c = 0; c < 3; c++)
      for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
          ma.channel(c).row(y)[x] = a[size_t(y) * w + x][c] / 255.f;
          mb.channel(c).row(y)[x] = b[size_t(y) * w + x][c] / 255.f;
        }
    auto ex = net.create_extractor();
    ex.input("in0", ma);
    ex.input("in1", mb);
    ex.input("in2", mt);
    ncnn::Mat output;
    if (ex.extract("out0", output))
      throw std::runtime_error("ncnn extraction");
    std::ofstream f(dir / "ncnn-reference.ppm", std::ios::binary);
    f << "P6\n" << w << ' ' << h << "\n255\n";
    for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++)
        for (unsigned c = 0; c < 3; c++) {
          unsigned char value = (unsigned char)std::clamp(
              std::round(output.channel(c).row(y)[x] * 255), 0.f, 255.f);
          f.put(char(value));
        }
    if (!f)
      throw std::runtime_error("ncnn output write");
    std::cout << "NCNN reference complete\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
