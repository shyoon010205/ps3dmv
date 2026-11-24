#include "frame_phase.hpp"
#include <iostream>
#include <stdexcept>
using namespace holo;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try{
  const json phase={{"enabled",true},{"model","frame_index_ramp"},{"sign",1},{"divisions",16},{"origin_frame",0},{"rgb_policy","same_angle"}};
  Frame a;a.width=7;a.height=5;a.channels=3;a.scalar_bits=64;a.real.resize(a.samples());a.imag.resize(a.samples());
  for(size_t i=0;i<a.samples();++i){a.real[i]=std::sin(i*.37);a.imag[i]=std::cos(i*.13);}
  auto b=a;apply_frame_phase(b,phase);check(identical(a,b),"Frame0 must be bit exact");
  b=a;b.frame_index=16;apply_frame_phase(b,phase);auto cycle=a;cycle.frame_index=16;check(identical(b,cycle),"Cycle must wrap exactly");
  b=a;b.frame_index=4;apply_frame_phase(b,phase);for(size_t i=0;i<a.samples();++i)check(b.real[i]==-a.imag[i]&&b.imag[i]==a.real[i],"90 degree sign/layout");
  // Independent propagation covariance: a global phase cannot change reconstructed amplitude.
  OpticalConfig cfg;cfg.padding_factor=2;cfg.threads=1;cfg.cache_mib=1;cfg.bandlimit=true;Propagator prop(7,5,cfg);
  double largest=0;for(uint64_t n=0;n<16;++n){b=a;b.frame_index=n;auto audit=apply_frame_phase(b,phase);
    check(audit.at("wrapped_step_index")==n,"Wrong frame step");
    for(unsigned channel=0;channel<3;++channel){std::vector<Complex> raw(a.pixels()),shift(a.pixels());
      for(size_t i=0;i<a.pixels();++i){const auto j=channel*a.pixels()+i;raw[i]={a.real[j],a.imag[j]};shift[i]={b.real[j],b.imag[j]};}
      auto r=prop.propagate(prop.spectrum(raw),cfg.wavelengths[channel],.001);
      auto t=prop.propagate(prop.spectrum(shift),cfg.wavelengths[channel],.001);
      for(size_t i=0;i<r.size();++i)largest=std::max(largest,std::abs(std::abs(r[i])-std::abs(t[i])));
    }
  }check(largest<1e-12,"Propagation amplitude invariance");
  b=a;b.scalar_bits=32;for(auto& v:b.real)v=double(float(v));for(auto& v:b.imag)v=double(float(v));b.frame_index=1;
  auto audit=apply_frame_phase(b,phase);check(audit.at("amplitude_invariance_passed"),"float32 amplitude audit");
  for(auto v:b.real)check(v==double(float(v)),"float32 rounding must precede storage verification");
  auto bad=phase;bad["divisions"]=0;bool rejected=false;try{apply_frame_phase(b,bad);}catch(...){rejected=true;}check(rejected,"Invalid division must fail");
  std::cout<<"PASS: zero/cycle bit exact, positive phase sign, RGB layout, all16 angles, source float32 precision, propagated amplitude invariant; max error="<<largest<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
