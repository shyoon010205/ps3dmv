#pragma once
#include "holo.hpp"
#include <array>
#include <iomanip>
#include <sstream>

namespace holo {
inline std::string reference_depth_name(size_t n) {
  std::ostringstream s;s<<"z_"<<std::setw(2)<<std::setfill('0')<<n;return s.str();
}
// Check dimensions, the entire chunk structure and every CRC before reusing an image.
inline bool intact_rgb_png(const fs::path& path,uint32_t w,uint32_t h) {
  try {
    const auto b=read_bytes(path);
    const std::array<uint8_t,8> signature{137,80,78,71,13,10,26,10};
    if (b.size()<45 || !std::equal(signature.begin(),signature.end(),b.begin())) return false;
    const auto word=[&](size_t p) {return (uint32_t(b[p])<<24)|(uint32_t(b[p+1])<<16)|(uint32_t(b[p+2])<<8)|b[p+3];};
    static const auto table=[] {std::array<uint32_t,256> t{};for(uint32_t i=0;i<256;++i){auto c=i;for(int k=0;k<8;++k)c=(c>>1)^((c&1)?0xedb88320u:0u);t[i]=c;}return t;}();
    bool header=false,data=false;
    for(size_t p=8;p+12<=b.size();) {
      const size_t n=word(p);if(n>b.size()-p-12)return false;
      uint32_t crc=0xffffffffu;
      for(size_t i=p+4;i<p+8+n;++i)crc=table[(crc^b[i])&255]^(crc>>8);
      if((crc^0xffffffffu)!=word(p+8+n))return false;
      const std::string type(reinterpret_cast<const char*>(b.data()+p+4),4);
      if(!header) {
        if(type!="IHDR" || n!=13 || word(p+8)!=w || word(p+12)!=h || b[p+16]!=8 || b[p+17]!=2 || b[p+18]!=0 || b[p+19]!=0 || b[p+20]!=0)return false;
        header=true;
      } else if(type=="IHDR")return false;
      if(type=="IDAT")data=true;
      if(type=="IEND")return data && n==0 && p+12==b.size();
      p+=n+12;
    }
  } catch(const std::exception&) {}
  return false;
}
inline bool same_reference(const json& old,const json& identity,const std::string& source,
                           const std::string& id,uint32_t w,uint32_t h) {
  return old.value("source_fingerprint_fnv1a64","")==source && old.value("reference_id","")==id &&
    old.value("width",0u)==w && old.value("height",0u)==h && !old.value("numeric_arrays_saved",true) &&
    old.value("identity",json{})==identity;
}
inline std::vector<json> reusable_depths(const fs::path& folder,const json& identity,const std::string& source,
                                       const std::string& id,uint32_t w,uint32_t h,size_t count) {
  std::vector<json> result(count);
  if(fs::is_regular_file(folder/"frame.json"))try {
    const auto old=read_json(folder/"frame.json");
    if(old.value("complete",false) && same_reference(old,identity,source,id,w,h)) {
      for(const auto& d:old.at("depths")) {
        const size_t i=d.at("index");
        if(i<count && d.at("image_file")==reference_depth_name(i)+".png")result[i]=d;
      }
    }
  } catch(const std::exception&) {}
  for(size_t i=0;i<count;++i) {
    const auto prefix=reference_depth_name(i);
    if(result[i].is_null() && fs::is_regular_file(folder/(prefix+".json"))) {
      try {
        const auto checkpoint=read_json(folder/(prefix+".json"));
        if(checkpoint.value("complete",false) && same_reference(checkpoint,identity,source,id,w,h) &&
           checkpoint.at("depth").at("index")==i && checkpoint.at("depth").at("image_file")==prefix+".png")result[i]=checkpoint.at("depth");
      } catch(const std::exception&) {}
    }
    if(!result[i].is_null() && !intact_rgb_png(folder/(prefix+".png"),w,h))result[i]=nullptr;
  }
  return result;
}
}
