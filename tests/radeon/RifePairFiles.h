// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
inline std::vector<std::array<unsigned char, 4>>
readRifePpm(const std::filesystem::path &file, unsigned w, unsigned h) {
  std::ifstream f(file, std::ios::binary);
  std::string magic;
  unsigned fw = 0, fh = 0, maximum = 0;
  f >> magic >> fw >> fh >> maximum;
  if (magic != "P6" || fw != w || fh != h || maximum != 255)
    throw std::runtime_error("PPM extent/format mismatch: " + file.string());
  f.get();
  std::vector<std::array<unsigned char, 4>> p(size_t(w) * h);
  for (auto &x : p) {
    f.read(reinterpret_cast<char *>(x.data()), 3);
    x[3] = 255;
  }
  if (!f)
    throw std::runtime_error("truncated PPM");
  return p;
}
