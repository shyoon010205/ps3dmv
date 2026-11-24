#include "holo.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <chrono>
#include <thread>
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "stb_image_write.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace holo {
namespace {
uint32_t u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
}
uint64_t u64(const uint8_t* p) { return u32(p) | (uint64_t(u32(p+4))<<32); }
void put32(std::ostream& s, uint32_t v) {
  for (int i=0;i<4;++i) s.put(char((v>>(8*i))&255));
}
void put64(std::ostream& s, uint64_t v) {
  put32(s,uint32_t(v)); put32(s,uint32_t(v>>32));
}
bool little_endian() { const uint32_t i=1; return *reinterpret_cast<const uint8_t*>(&i)==1; }
struct Element { uint32_t type; size_t pos, size, next; };
Element element(const std::vector<uint8_t>& b, size_t p, size_t end) {
  if (p>end || end-p<8) throw std::runtime_error("Truncated MAT element tag");
  const auto tag=u32(&b[p]);
  const auto small=tag>>16;
  if (small) {
    if (small>4) throw std::runtime_error("Invalid MAT small element");
    return {tag&0xffff,p+4,small,p+8};
  }
  const size_t n=u32(&b[p+4]);
  if (n>end-p-8) throw std::runtime_error("MAT element exceeds container");
  const size_t padded=(n+7)&~size_t(7);
  if (padded>end-p-8) throw std::runtime_error("Truncated MAT padding");
  return {tag,p+8,n,p+8+padded};
}
template<typename T> double scalar(const uint8_t* p) { T v; std::memcpy(&v,p,sizeof(T)); return double(v); }
size_t checked_samples(uint32_t w,uint32_t h,uint32_t c) {
  if (!w || !h || !c || c>64) throw std::runtime_error("Invalid array dimensions");
  const uint64_t n=uint64_t(w)*h;
  if (n>uint64_t(std::numeric_limits<size_t>::max())/(c*16ull) || n*c>500000000ull)
    throw std::runtime_error("Array exceeds supported allocation limit");
  return size_t(n*c);
}
void ensure_frame(const Frame& f) {
  const auto n=checked_samples(f.width,f.height,f.channels);
  if ((f.scalar_bits!=32 && f.scalar_bits!=64) || f.real.size()!=n || f.imag.size()!=n)
    throw std::runtime_error("Invalid complex frame storage");
  for (size_t i=0;i<n;++i) if (!std::isfinite(f.real[i]) || !std::isfinite(f.imag[i]))
    throw std::runtime_error("Non-finite original complex sample");
}
void write_header(std::ostream& s, const char* magic, uint32_t w,uint32_t h,uint32_t c,
                  uint32_t bits,uint64_t frame,uint64_t bytes,uint32_t layout) {
  s.write(magic,8); put32(s,1); put32(s,64); put32(s,w); put32(s,h); put32(s,c);
  put32(s,bits); put64(s,frame); put64(s,bytes); put32(s,layout); put32(s,0); put64(s,0);
}
void check_header(const std::vector<uint8_t>& b,const char* magic,uint32_t layout) {
  if (b.size()<64 || std::memcmp(b.data(),magic,8)!=0 || u32(&b[8])!=1 || u32(&b[12])!=64)
    throw std::runtime_error("Invalid binary header or version");
  if (u32(&b[48])!=layout || u32(&b[52])!=0 || u64(&b[56])!=0 || u64(&b[40])!=b.size()-64)
    throw std::runtime_error("Invalid binary layout, flags, or payload length");
}
std::ofstream output(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream s(p,std::ios::binary|std::ios::trunc);
  s.exceptions(std::ios::badbit|std::ios::failbit); return s;
}
}

std::vector<uint8_t> read_bytes(const fs::path& p) {
  std::ifstream s(p,std::ios::binary|std::ios::ate);
  if (!s) throw std::runtime_error("Cannot open: "+p.string());
  auto end=s.tellg();
  if (end<0 || uint64_t(end)>8ull*1024*1024*1024) throw std::runtime_error("Unsupported file size");
  std::vector<uint8_t> b(static_cast<size_t>(end)); s.seekg(0);
  if (!b.empty()) s.read(reinterpret_cast<char*>(b.data()),std::streamsize(b.size()));
  if (!s) throw std::runtime_error("Incomplete read: "+p.string()); return b;
}
std::string fingerprint(const std::vector<uint8_t>& b) {
  uint64_t h=14695981039346656037ull;
  for (auto v:b) { h^=v; h*=1099511628211ull; }
  std::ostringstream s; s<<std::hex<<std::setfill('0')<<std::setw(16)<<h; return s.str();
}
std::string fingerprint(const std::string& b) { return fingerprint(std::vector<uint8_t>(b.begin(),b.end())); }

MatFrame read_mat5(const fs::path& path,const std::string& variable) {
  if (!little_endian()) throw std::runtime_error("This build requires a little-endian host");
  const auto b=read_bytes(path);
  if (b.size()<128 || std::memcmp(b.data(),"MATLAB 5.0 MAT-file",19)!=0 || b[126]!='I' || b[127]!='M')
    throw std::runtime_error("Expected uncompressed little-endian MAT v5 (v7.3/HDF5 is not supported)");
  if (b[124]!=0 || b[125]!=1) throw std::runtime_error("Unsupported MAT version");
  for (size_t pos=128;pos<b.size();) {
    const auto outer=element(b,pos,b.size()); pos=outer.next;
    if (outer.type==15) throw std::runtime_error("Compressed MAT elements are not supported; no automatic lossy conversion is performed");
    if (outer.type!=14) continue;
    const size_t end=outer.pos+outer.size;
    const auto flags=element(b,outer.pos,end);
    const auto dims=element(b,flags.next,end);
    const auto name=element(b,dims.next,end);
    if (name.type!=1 && name.type!=16) throw std::runtime_error("Invalid MAT variable name");
    const std::string found(reinterpret_cast<const char*>(&b[name.pos]),name.size);
    if (found!=variable) continue;
    if (flags.type!=6 || flags.size!=8 || dims.type!=5 || dims.size%4 || dims.size<8 || dims.size>12)
      throw std::runtime_error("Expected a dense HxW or HxWxC numeric MAT array");
    const auto f=u32(&b[flags.pos]);
    const auto cls=f&255;
    if ((cls!=6 && cls!=7) || !(f&0x0800) || (f&0x0200))
      throw std::runtime_error("Expected complex single or double complex_tensor");
    MatFrame result;
    for (size_t i=0;i<dims.size;i+=4) {
      const auto d=u32(&b[dims.pos+i]);
      if (!d || d>uint32_t(std::numeric_limits<int32_t>::max())) throw std::runtime_error("Invalid MAT dimension");
      result.matlab_dimensions.push_back(d);
    }
    auto& frame=result.frame;
    frame.height=result.matlab_dimensions[0]; frame.width=result.matlab_dimensions[1];
    frame.channels=result.matlab_dimensions.size()==3?result.matlab_dimensions[2]:1;
    frame.scalar_bits=cls==7?32:64;
    const size_t n=checked_samples(frame.width,frame.height,frame.channels), bytes=frame.scalar_bits/8;
    const auto re=element(b,name.next,end), im=element(b,re.next,end);
    const uint32_t expected=frame.scalar_bits==32?7:9;
    if (re.type!=expected || im.type!=expected || re.size!=n*bytes || im.size!=n*bytes || im.next!=end)
      throw std::runtime_error("MAT real/imag payload does not match dimensions");
    frame.real.resize(n); frame.imag.resize(n);
    for (uint32_t c=0;c<frame.channels;++c) for (uint32_t y=0;y<frame.height;++y)
      for (uint32_t x=0;x<frame.width;++x) {
        const size_t src=size_t(y)+size_t(frame.height)*(x+size_t(frame.width)*c);
        const size_t dst=(size_t(c)*frame.height+y)*frame.width+x;
        frame.real[dst]=bytes==4?scalar<float>(&b[re.pos+src*bytes]):scalar<double>(&b[re.pos+src*bytes]);
        frame.imag[dst]=bytes==4?scalar<float>(&b[im.pos+src*bytes]):scalar<double>(&b[im.pos+src*bytes]);
      }
    ensure_frame(frame); result.source_fingerprint=fingerprint(b); return result;
  }
  throw std::runtime_error("MAT variable not found: "+variable);
}

void commit_file(const fs::path& tmp,const fs::path& target) {
  // Only application-owned output files are replaced, never input MAT files.
  if (fs::exists(target)) fs::remove(target);
  fs::rename(tmp,target);
}
void write_holo(const fs::path& path,const Frame& f) {
  if (!little_endian()) throw std::runtime_error("Little-endian host required");
  ensure_frame(f); const fs::path tmp=path.string()+".partial"; auto s=output(tmp);
  write_header(s,"PSHCF01",f.width,f.height,f.channels,f.scalar_bits,f.frame_index,
               f.samples()*2*(f.scalar_bits/8),1);
  const auto n=f.pixels();
  for (uint32_t c=0;c<f.channels;++c) for (const auto* values:{&f.real,&f.imag}) {
    if (f.scalar_bits==64) s.write(reinterpret_cast<const char*>(values->data()+c*n),std::streamsize(n*8));
    else {
      std::vector<float> plane(n);
      for (size_t i=0;i<n;++i) plane[i]=float((*values)[c*n+i]);
      s.write(reinterpret_cast<const char*>(plane.data()),std::streamsize(n*4));
    }
  }
  s.close(); commit_file(tmp,path);
}
Frame read_holo(const fs::path& path) {
  if (!little_endian()) throw std::runtime_error("Little-endian host required");
  const auto b=read_bytes(path); check_header(b,"PSHCF01",1); Frame f;
  f.width=u32(&b[16]); f.height=u32(&b[20]); f.channels=u32(&b[24]); f.scalar_bits=u32(&b[28]);
  f.frame_index=u64(&b[32]); const size_t n=checked_samples(f.width,f.height,f.channels);
  if ((f.scalar_bits!=32 && f.scalar_bits!=64) || n*2*(f.scalar_bits/8)!=b.size()-64)
    throw std::runtime_error("Invalid original payload size or precision");
  f.real.resize(n); f.imag.resize(n); size_t p=64;
  for (uint32_t c=0;c<f.channels;++c) for (auto* values:{&f.real,&f.imag})
    for (size_t i=0;i<f.pixels();++i,p+=f.scalar_bits/8)
      (*values)[c*f.pixels()+i]=f.scalar_bits==32?scalar<float>(&b[p]):scalar<double>(&b[p]);
  ensure_frame(f); return f;
}
bool identical(const Frame& a,const Frame& b) {
  return a.width==b.width && a.height==b.height && a.channels==b.channels && a.scalar_bits==b.scalar_bits &&
    a.frame_index==b.frame_index && a.real.size()==b.real.size() && a.imag.size()==b.imag.size() &&
    std::memcmp(a.real.data(),b.real.data(),a.real.size()*sizeof(double))==0 &&
    std::memcmp(a.imag.data(),b.imag.data(),a.imag.size()*sizeof(double))==0;
}
json frame_statistics(const Frame& f) {
  json a=json::array();
  for (uint32_t c=0;c<f.channels;++c) {
    double rmin=INFINITY,rmax=-INFINITY,imin=INFINITY,imax=-INFINITY,amax=0;
    for (size_t i=c*f.pixels();i<(c+1)*f.pixels();++i) {
      rmin=std::min(rmin,f.real[i]); rmax=std::max(rmax,f.real[i]);
      imin=std::min(imin,f.imag[i]); imax=std::max(imax,f.imag[i]);
      amax=std::max(amax,std::hypot(f.real[i],f.imag[i]));
    }
    a.push_back({{"channel",c},{"real_min",rmin},{"real_max",rmax},{"imag_min",imin},{"imag_max",imax},{"amplitude_max",amax}});
  }
  return a;
}
void write_json(const fs::path& path,const json& j) {
  const fs::path tmp=path.string()+".partial"; auto s=output(tmp); s<<j.dump(2)<<'\n'; s.close(); commit_file(tmp,path);
}
json read_json(const fs::path& path) {
#ifdef _WIN32
  // Study snapshots are published by atomic rename. Share deletion so a
  // publisher can replace the path while we finish reading the old snapshot.
  HANDLE handle=INVALID_HANDLE_VALUE;DWORD error=0;
  for(unsigned attempt=0;attempt<100;++attempt) {
    handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                       nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle!=INVALID_HANDLE_VALUE)break;
    error=GetLastError();
    if(error!=ERROR_SHARING_VIOLATION&&error!=ERROR_ACCESS_DENIED&&error!=ERROR_FILE_NOT_FOUND)break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot read JSON: "+path.u8string()+"; Windows error "+std::to_string(error));
  struct Close {HANDLE value;~Close(){CloseHandle(value);}} close{handle};
  std::string text;char buffer[16384];DWORD count=0;
  do {
    if(!ReadFile(handle,buffer,sizeof(buffer),&count,nullptr))
      throw std::runtime_error("JSON read failed: "+path.u8string()+"; Windows error "+std::to_string(GetLastError()));
    text.append(buffer,count);
  } while(count);
  return json::parse(text);
#else
  std::ifstream s(path);if(!s)throw std::runtime_error("Cannot read JSON: "+path.string());json j;s>>j;return j;
#endif
}
void write_rgb_png(const fs::path& path,uint32_t w,uint32_t h,const std::vector<float>& a,double low,double high) {
  const size_t n=checked_samples(w,h,3)/3;
  if (a.size()!=3*n || !std::isfinite(low) || !std::isfinite(high) || !(high>low)) throw std::runtime_error("Invalid RGB display settings");
  std::vector<uint8_t> rgb(3*n);
  for (size_t i=0;i<n;++i) for (size_t c=0;c<3;++c) {
    const double x=std::clamp((double(a[c*n+i])-low)/(high-low),0.0,1.0);
    rgb[3*i+c]=uint8_t(std::lround(x*255));
  }
  fs::create_directories(path.parent_path()); const fs::path tmp=path.string()+".partial";
  // Filesystem-aware C++ I/O supports Unicode Windows paths.
  auto s=output(tmp);
  auto callback=[](void* ctx,void* data,int size){static_cast<std::ostream*>(ctx)->write(static_cast<const char*>(data),size);};
  if (!stbi_write_png_to_func(callback,&s,int(w),int(h),3,rgb.data(),int(w*3))) throw std::runtime_error("PNG encoding failed");
  s.close(); commit_file(tmp,path);
}
}
