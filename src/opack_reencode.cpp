// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/opack.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>

// Interoperability test utility: no protocol values are printed.
int main(int argc, char** argv) try {
  if (argc != 3) throw std::invalid_argument("usage: headunit-opack-reencode INPUT NEW_OUTPUT");
  if (std::filesystem::file_size(argv[1]) > headunit::OpackLimits{}.wire_bytes)
    throw std::invalid_argument("input too large");
  std::ifstream file(argv[1], std::ios::binary);
  if (!file) throw std::runtime_error("cannot read input");
  headunit::OpackValue::Bytes bytes(headunit::OpackLimits{}.wire_bytes + 1);
  file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<std::size_t>(file.gcount()));
  if (bytes.size() > headunit::OpackLimits{}.wire_bytes)
    throw std::invalid_argument("input grew beyond limit");
  const auto output = headunit::encode_opack(headunit::decode_opack(bytes));
  umask(0077);
  FILE* target = std::fopen(argv[2], "wbx");
  if (!target) throw std::runtime_error("cannot create new output");
  const auto written = std::fwrite(output.data(), 1, output.size(), target);
  const auto closed = std::fclose(target);
  if (written != output.size() || closed) throw std::runtime_error("cannot write output");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
