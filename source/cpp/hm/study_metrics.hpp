#pragma once
#include "holo.hpp"
#include <array>
namespace study {
struct ImageStats {
  uint64_t samples=0, windows=0, clipped_reference=0, clipped_test=0;
  double sse=0, absolute_error=0, reference_energy=0, ssim_sum=0;
  ImageStats& operator+=(const ImageStats& b);
  holo::json json(double peak) const;
};
ImageStats imageMetrics(const std::vector<float>& reference,const std::vector<float>& test,
                       unsigned width,unsigned height,double peak,bool ssim);
struct ComplexStats {
  uint64_t samples=0, phase_samples=0;
  double real_sse=0,imag_sse=0,energy=0,test_energy=0,normalized_sse=0;
  double correlation_real=0,correlation_imag=0,phase_sq=0,phase_weight=0;
  ComplexStats& operator+=(const ComplexStats& b);
  holo::json json() const;
};
ComplexStats complexMetrics(const std::vector<holo::Complex>& reference,const std::vector<holo::Complex>& test,
                            double component_peak_range,double phase_threshold);
struct Point {double rate, quality;};
holo::json bdComparison(std::vector<Point> baseline,std::vector<Point> ours,unsigned minimum_points=4);
}
