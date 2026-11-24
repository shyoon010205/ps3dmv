#pragma once
#include "holo.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace holo {
// A global complex-field phase, shared by all spatial samples and RGB wavelengths.
// This is not an optical path length model (which would use wavelength-dependent angles).
inline json apply_frame_phase(Frame& f,const json& p) {
  if(!p.value("enabled",false))return {{"enabled",false},{"raw_source_bit_exact",true}};
  if(p.at("model")!="frame_index_ramp" || p.at("sign").get<int>()!=1 ||
     p.at("rgb_policy")!="same_angle" || !p.at("divisions").is_number_integer() ||
     !p.at("origin_frame").is_number_integer())throw std::runtime_error("Unsupported frame phase model");
  const int divisions=p.at("divisions").get<int>();
  const auto origin=p.at("origin_frame").get<int64_t>();
  if(divisions<1 || divisions>1048576 || origin<0 || f.frame_index<uint64_t(origin))
    throw std::runtime_error("Invalid phase divisions/origin");
  if((f.scalar_bits!=32&&f.scalar_bits!=64)||f.real.size()!=f.samples()||f.imag.size()!=f.samples())
    throw std::runtime_error("Invalid frame storage for phase transform");
  const auto n=f.frame_index-uint64_t(origin), tick=n%uint64_t(divisions);
  constexpr double tau=6.283185307179586476925286766559;
  const double angle=tau*double(tick)/double(divisions);
  double c=1,s=0,max_amplitude=0,max_error=0;
  if(tick){
    if(divisions%4==0 && tick%uint64_t(divisions/4)==0){
      const unsigned quadrant=unsigned(tick/uint64_t(divisions/4));
      c=quadrant==2?-1:0;s=quadrant==1?1:quadrant==3?-1:0;
    }else{c=std::cos(angle);s=std::sin(angle);}
  }
  for(size_t i=0;i<f.samples();++i){
    const double re=f.real[i],im=f.imag[i],amp=std::hypot(re,im);
    if(!std::isfinite(amp))throw std::runtime_error("Nonfinite complex input");
    // Preserve n=0 and exact multiples of the full cycle, including signed zero.
    if(tick){
      double r=c*re-s*im,j=s*re+c*im;
      if(f.scalar_bits==32){r=double(float(r));j=double(float(j));}
      if(!std::isfinite(r)||!std::isfinite(j))throw std::runtime_error("Phase transform exceeds source precision");
      f.real[i]=r;f.imag[i]=j;
    }
    max_amplitude=std::max(max_amplitude,amp);
    max_error=std::max(max_error,std::abs(std::hypot(f.real[i],f.imag[i])-amp));
  }
  const double eps=f.scalar_bits==32?double(std::numeric_limits<float>::epsilon()):std::numeric_limits<double>::epsilon();
  const double bound=8*eps*max_amplitude+std::numeric_limits<float>::denorm_min();
  if(max_error>bound)throw std::runtime_error("Phase amplitude-invariance audit failed");
  return {{"enabled",true},{"model","frame_index_ramp"},{"formula","H_n = H_source,n * exp(+i * (n-origin) * 2*pi/divisions)"},
    {"divisions",divisions},{"origin_frame",origin},{"unwrapped_step_index",n},{"wrapped_step_index",tick},
    {"phase_radians",angle},{"phase_degrees",360.0*double(tick)/divisions},{"rgb_policy","same_angle"},
    {"precision_bits",f.scalar_bits},{"raw_source_bit_exact",tick==0},{"amplitude_max_before",max_amplitude},
    {"amplitude_max_abs_error",max_error},{"amplitude_error_bound",bound},{"amplitude_invariance_passed",true},
    {"forward_mapping",false},{"clipping","none"}};
}
}
