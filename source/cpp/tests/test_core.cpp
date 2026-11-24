#include "holo.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace holo;
namespace {
constexpr double pi=3.14159265358979323846;
void require(bool ok,const std::string& message) {if (!ok) throw std::runtime_error(message);}
template<typename F> void must_fail(F fn,const std::string& message) {bool failed=false;try {fn();} catch (const std::exception&) {failed=true;}require(failed,message);}
void put(std::vector<uint8_t>& b,uint32_t x) {for (int i=0;i<4;++i)b.push_back(uint8_t(x>>(8*i)));}
void tag(std::vector<uint8_t>& b,uint32_t type,const std::vector<uint8_t>& data) {
  put(b,type);put(b,uint32_t(data.size()));b.insert(b.end(),data.begin(),data.end());while(b.size()%8)b.push_back(0);
}
void save(const fs::path& p,const std::vector<uint8_t>& b) {std::ofstream s(p,std::ios::binary);s.write(reinterpret_cast<const char*>(b.data()),std::streamsize(b.size()));}
std::vector<uint8_t> fixture(bool single,const std::string& name) {
  std::vector<uint8_t> b(128,0);const std::string header="MATLAB 5.0 MAT-file, independently constructed asymmetric test array";
  std::copy(header.begin(),header.end(),b.begin());b[125]=1;b[126]='I';b[127]='M';
  std::vector<uint8_t> matrix,flags,dims;put(flags,0x0800|(single?7u:6u));put(flags,0);tag(matrix,6,flags);
  put(dims,2);put(dims,3);put(dims,3);tag(matrix,5,dims);
  if(name.size()<=4){put(matrix,(uint32_t(name.size())<<16)|1);for(size_t i=0;i<4;++i)matrix.push_back(i<name.size()?uint8_t(name[i]):0);}
  else tag(matrix,1,std::vector<uint8_t>(name.begin(),name.end()));
  for (int component=0;component<2;++component) {
    std::vector<uint8_t> values;
    // MATLAB column-major payload, with channel/row/column distinguishable in each value.
    for (int c=0;c<3;++c)for(int x=0;x<3;++x)for(int y=0;y<2;++y){
      const double v=(component?-1:1)*(100*c+10*y+x+0.25);
      if(single){const float z=float(v);const auto* p=reinterpret_cast<const uint8_t*>(&z);values.insert(values.end(),p,p+4);}
      else{const auto* p=reinterpret_cast<const uint8_t*>(&v);values.insert(values.end(),p,p+8);}
    }
    tag(matrix,single?7:9,values);
  }
  tag(b,14,matrix);return b;
}
std::vector<Complex> direct_asm(const std::vector<Complex>& a,size_t w,size_t h,double dx,double dy,double wl,double z) {
  const size_t n=w*h;std::vector<Complex> freq(n),out(n);
  for(size_t ky=0;ky<h;++ky)for(size_t kx=0;kx<w;++kx){
    for(size_t y=0;y<h;++y)for(size_t x=0;x<w;++x)
      freq[ky*w+kx]+=a[y*w+x]*std::polar(1.0,-2*pi*(double(kx*x)/w+double(ky*y)/h));
    const double fx=double(kx<=(w-1)/2?int64_t(kx):int64_t(kx)-int64_t(w))/(w*dx);
    const double fy=double(ky<=(h-1)/2?int64_t(ky):int64_t(ky)-int64_t(h))/(h*dy);
    const double arg=1-wl*wl*(fx*fx+fy*fy);
    freq[ky*w+kx]*=arg>0?std::polar(1.0,2*pi*z/wl*std::sqrt(arg)):Complex{};
  }
  for(size_t y=0;y<h;++y)for(size_t x=0;x<w;++x)for(size_t ky=0;ky<h;++ky)for(size_t kx=0;kx<w;++kx)
    out[y*w+x]+=freq[ky*w+kx]*std::polar(1.0,2*pi*(double(kx*x)/w+double(ky*y)/h))/double(n);
  return out;
}
void near_arrays(const std::vector<Complex>& a,const std::vector<Complex>& b,double tolerance,const std::string& label){
  require(a.size()==b.size(),label+" dimensions");for(size_t i=0;i<a.size();++i)require(std::abs(a[i]-b[i])<tolerance,label+" sample "+std::to_string(i));
}
}
int main(){
 try {
  const auto test_id=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const fs::path dir=fs::current_path()/("holo_test_"+test_id);fs::create_directories(dir);
  for(bool single:{true,false})for(const std::string name:{std::string("complex_tensor"),std::string("x")}){
    const auto bytes=fixture(single,name);const auto matpath=dir/(name+(single?"_32.mat":"_64.mat"));save(matpath,bytes);
    const auto parsed=read_mat5(matpath,name);auto f=parsed.frame;f.frame_index=42;
    require(f.width==3 && f.height==2 && f.channels==3 && f.scalar_bits==(single?32u:64u),"MAT dimensions/precision");
    for(size_t c=0;c<3;++c)for(size_t y=0;y<2;++y)for(size_t x=0;x<3;++x){
      const auto i=(c*2+y)*3+x;const double expected=100*c+10*y+x+0.25;
      require(f.real[i]==expected && f.imag[i]==-expected,"Column/row/channel layout");
    }
    f.real[0]=-0.0;const auto converted=dir/(name+(single?"_32.holo":"_64.holo"));
    write_holo(converted,f);require(identical(f,read_holo(converted)),"Exact .holo roundtrip including negative zero");
    must_fail([&]{read_mat5(matpath,"wrong_name");},"Missing MAT variable must fail");
    auto bad=bytes;bad.resize(bad.size()-3);save(dir/"truncated.mat",bad);
    must_fail([&]{read_mat5(dir/"truncated.mat",name);},"Truncated MAT must fail");
    auto corrupt=read_bytes(converted);corrupt[52]=1;save(dir/"mapped.holo",corrupt);
    must_fail([&]{read_holo(dir/"mapped.holo");},"Mapped/unknown flags must fail");
  }
  auto compressed=fixture(true,"complex_tensor");compressed[128]=15;save(dir/"compressed.mat",compressed);
  must_fail([&]{read_mat5(dir/"compressed.mat","complex_tensor");},"Unsupported compressed MAT must fail");
  OpticalConfig cfg;cfg.field_scale=1;cfg.threads=2;cfg.cache_mib=1;cfg.bandlimit=false;
  for(auto dims:{std::pair<uint32_t,uint32_t>{7,5},{8,6}}){
    const auto [w,h]=dims;std::vector<Complex> a(size_t(w)*h);
    for(size_t i=0;i<a.size();++i)a[i]={std::sin(0.37*i),std::cos(0.19*i)};
    cfg.padding_factor=2;Propagator padded(w,h,cfg);
    near_arrays(padded.propagate(padded.spectrum(a),532e-9,0),a,1e-12,"z=0 padding/crop identity");
    cfg.padding_factor=1;Propagator p(w,h,cfg);
    const auto actual=p.propagate(p.spectrum(a),532e-9,0.0013);
    near_arrays(actual,direct_asm(a,w,h,cfg.pitch_x,cfg.pitch_y,532e-9,0.0013),1e-10,"Independent direct DFT propagation");
    near_arrays(p.propagate(p.spectrum(actual),532e-9,-0.0013),a,1e-11,"Positive/negative propagation inverse");
    double input_energy=0,output_energy=0;for(size_t i=0;i<a.size();++i){input_energy+=std::norm(a[i]);output_energy+=std::norm(actual[i]);}
    require(std::abs(input_energy-output_energy)<1e-10,"Energy conservation without bandlimit/crop");
  }
  cfg.padding_factor=1;cfg.bandlimit=true;Propagator limited(8,6,cfg);
  std::vector<Complex> dc(48,Complex(0.5,0.25));const double z=0.0004,wl=638e-9;
  const auto dc_out=limited.propagate(limited.spectrum(dc),wl,z);
  for(auto v:dc_out) require(std::abs(v-dc[0]*std::polar(1.0,2*pi*z/wl))<1e-11,"DC phase convention");
  std::vector<Complex> checker(48);
  for(size_t y=0;y<6;++y)for(size_t x=0;x<8;++x)checker[y*8+x]=(x%2)?1.0:-1.0;
  for(auto v:limited.propagate(limited.spectrum(checker),wl,1.0))require(std::abs(v)<1e-12,"Bandlimit rejects undersampled transfer phase");
  cfg.pitch_x=1e-7;cfg.pitch_y=1e-7;cfg.bandlimit=false;Propagator evanescent(8,6,cfg);
  for(auto v:evanescent.propagate(evanescent.spectrum(checker),wl,-0.1))require(std::abs(v)<1e-12,"Negative-z evanescent suppression");
  std::vector<float> amplitudes{0,0.5f,1,2,0.25f,0.75f};
  write_rgb_png(dir/"preview.png",2,1,amplitudes,0,1);const auto png=read_bytes(dir/"preview.png");
  require(png.size()>32 && png[0]==137 && png[1]=='P' && png[2]=='N' && png[3]=='G',"PNG output");
  require(amplitudes[3]==2,"Display clipping must not mutate scientific values");
  // Remove only regular files created in this test's unique directory.
  for (const auto& entry:fs::directory_iterator(dir)) if (entry.is_regular_file()) fs::remove(entry.path());
  fs::remove(dir);
  std::cout<<"PASS: MAT layout/precision, original bit-exact conversion, invalid inputs, ASM identity/direct DFT/inverse/energy/DC, in-memory amplitude and PNG output\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
