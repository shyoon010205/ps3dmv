#include "holo.hpp"
#include "pocketfft_hdronly.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#ifdef _OPENMP
#include <omp.h>
#endif

namespace holo {
namespace {
constexpr double pi=3.1415926535897932384626433832795;
void fft2(std::vector<Complex>& a,uint32_t h,uint32_t w,bool forward,size_t threads) {
  const pocketfft::shape_t shape{h,w},axes{0,1};
  const pocketfft::stride_t strides{std::ptrdiff_t(w*sizeof(Complex)),std::ptrdiff_t(sizeof(Complex))};
  pocketfft::c2c(shape,strides,strides,axes,forward,a.data(),a.data(),
                forward?1.0:1.0/(double(w)*h),threads);
}
double frequency(uint32_t i,uint32_t n,double pitch) {
  const int64_t k=i<=(n-1)/2?int64_t(i):int64_t(i)-n;
  return double(k)/(double(n)*pitch);
}
}
Propagator::Propagator(uint32_t w,uint32_t h,OpticalConfig c):width_(w),height_(h),config_(std::move(c)) {
  if (!w || !h || config_.padding_factor<1 || config_.padding_factor>4 ||
      !std::isfinite(config_.pitch_x) || !std::isfinite(config_.pitch_y) ||
      config_.pitch_x<=0 || config_.pitch_y<=0 || config_.threads<1 || config_.threads>256 ||
      uint64_t(w)*config_.padding_factor>UINT32_MAX || uint64_t(h)*config_.padding_factor>UINT32_MAX)
    throw std::runtime_error("Invalid propagation configuration");
  pw_=w*config_.padding_factor;ph_=h*config_.padding_factor;
  if (uint64_t(pw_)*ph_>500000000ull) throw std::runtime_error("Padded FFT is too large");
#ifdef _OPENMP
  omp_set_num_threads(int(config_.threads));
#endif
}
std::vector<Complex> Propagator::spectrum(const std::vector<Complex>& field) const {
  if (field.size()!=size_t(width_)*height_) throw std::runtime_error("Wrong field dimensions");
  std::vector<Complex> a(size_t(pw_)*ph_);
  const size_t xoff=(pw_-width_)/2,yoff=(ph_-height_)/2;
  for (size_t y=0;y<height_;++y)
    std::copy_n(field.data()+y*width_,width_,a.data()+(y+yoff)*pw_+xoff);
  fft2(a,ph_,pw_,true,config_.threads); return a;
}
std::vector<Complex> Propagator::kernel(double wl,double z) const {
  if (!std::isfinite(wl) || wl<=0 || !std::isfinite(z)) throw std::runtime_error("Invalid wavelength/distance");
  std::vector<Complex> a(size_t(pw_)*ph_);
  const double lx=double(pw_)*config_.pitch_x,ly=double(ph_)*config_.pitch_y;
  // Frequencies remain in native FFT order. No fftshift/crop conventions are imported from MATLAB.
#pragma omp parallel for
  for (int64_t y=0;y<int64_t(ph_);++y) {
    const double fy=frequency(uint32_t(y),ph_,config_.pitch_y);
    for (uint32_t x=0;x<pw_;++x) {
      const double fx=frequency(x,pw_,config_.pitch_x);
      const double s=1.0-wl*wl*(fx*fx+fy*fy);
      if (s<=0) continue; // Evanescent components are suppressed, including for negative z.
      const double root=std::sqrt(s);
      // Conservative sampling bounds from each partial derivative of transfer phase.
      if (config_.bandlimit && (std::abs(z*wl*fx)>0.5*lx*root || std::abs(z*wl*fy)>0.5*ly*root)) continue;
      const double phase=(2*pi/wl)*z*root;
      a[size_t(y)*pw_+x]={std::cos(phase),std::sin(phase)};
    }
  }
  return a;
}
std::vector<Complex> Propagator::propagate(const std::vector<Complex>& s,double wl,double z) {
  if (s.size()!=size_t(pw_)*ph_) throw std::runtime_error("Wrong spectrum dimensions");
  const auto key=std::make_pair(wl,z); auto hit=kernels_.find(key);
  std::vector<Complex> temporary;
  const std::vector<Complex>* transfer=nullptr;
  if (hit!=kernels_.end()) transfer=&hit->second;
  else {
    temporary=kernel(wl,z);
    ++kernel_builds_;
    const size_t bytes=temporary.size()*sizeof(Complex);
    if (config_.cache_mib<=std::numeric_limits<size_t>::max()/(1024*1024) &&
        cache_bytes_+bytes<=config_.cache_mib*1024*1024) {
      cache_bytes_+=bytes; transfer=&kernels_.emplace(key,std::move(temporary)).first->second;
    } else transfer=&temporary;
  }
  std::vector<Complex> a(s.size());
#pragma omp parallel for
  for (int64_t i=0;i<int64_t(s.size());++i) a[size_t(i)]=s[size_t(i)]*(*transfer)[size_t(i)];
  fft2(a,ph_,pw_,false,config_.threads);
  std::vector<Complex> out(size_t(width_)*height_);
  const size_t xoff=(pw_-width_)/2,yoff=(ph_-height_)/2;
  for (size_t y=0;y<height_;++y)
    std::copy_n(a.data()+(y+yoff)*pw_+xoff,width_,out.data()+y*width_);
  return out;
}
}
