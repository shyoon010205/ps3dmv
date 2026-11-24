#pragma once
#include <complex>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "json.hpp"

namespace holo {
namespace fs = std::filesystem;
using json = nlohmann::json;
using Complex = std::complex<double>;
struct Frame {
  uint32_t width = 0, height = 0, channels = 0, scalar_bits = 0;
  uint64_t frame_index = 0;
  // Both arrays are [channel][row][column]. Source precision is preserved on disk.
  std::vector<double> real, imag;
  size_t pixels() const { return size_t(width) * height; }
  size_t samples() const { return pixels() * channels; }
};
struct MatFrame {
  Frame frame;
  std::string source_fingerprint;
  std::vector<uint32_t> matlab_dimensions;
};
struct OpticalConfig {
  double pitch_x = 3.6e-6, pitch_y = 3.6e-6, field_scale = 1e-8;
  std::vector<double> wavelengths{638e-9, 532e-9, 450e-9};
  std::vector<double> plane_z;
  double propagation_sign = -1.0;
  uint32_t padding_factor = 2;
  bool bandlimit = true;
  size_t threads = 8, cache_mib = 512;
};
std::vector<uint8_t> read_bytes(const fs::path& path);
std::string fingerprint(const std::vector<uint8_t>& data);
std::string fingerprint(const std::string& data);
MatFrame read_mat5(const fs::path& path, const std::string& variable);
void write_holo(const fs::path& path, const Frame& frame);
Frame read_holo(const fs::path& path);
bool identical(const Frame& a, const Frame& b);
json frame_statistics(const Frame& frame);
void write_json(const fs::path& path, const json& value);
json read_json(const fs::path& path);
void write_rgb_png(const fs::path& path, uint32_t width, uint32_t height,
                   const std::vector<float>& channel_major, double clip_low, double clip_high);
void commit_file(const fs::path& temporary, const fs::path& target);

class Propagator {
 public:
  Propagator(uint32_t width, uint32_t height, OpticalConfig config);
  std::vector<Complex> spectrum(const std::vector<Complex>& field) const;
  std::vector<Complex> propagate(const std::vector<Complex>& frequency,
                                 double wavelength, double distance);
  uint32_t padded_width() const { return pw_; }
  uint32_t padded_height() const { return ph_; }
  size_t cached_kernel_count() const { return kernels_.size(); }
  size_t cached_kernel_bytes() const { return cache_bytes_; }
  size_t kernel_build_count() const { return kernel_builds_; }
 private:
  uint32_t width_, height_, pw_, ph_;
  OpticalConfig config_;
  std::map<std::pair<double,double>, std::vector<Complex>> kernels_;
  size_t cache_bytes_ = 0;
  size_t kernel_builds_ = 0;
  std::vector<Complex> kernel(double wavelength, double distance) const;
};
}
