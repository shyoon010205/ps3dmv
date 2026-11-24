#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace psvalidation {
inline void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
inline std::map<unsigned,std::string> complexHashes(const std::filesystem::path& path,unsigned frames) {
  std::ifstream in(path);require(bool(in),"Missing PS complex hash log");std::string line;
  std::map<unsigned,std::string> hashes;
  while(std::getline(in,line))if(line.rfind("PSFINAL ",0)==0) {
    std::istringstream row(line);std::string tag,pocLabel,hashLabel,hash;unsigned poc;
    require(bool(row>>tag>>pocLabel>>poc>>hashLabel>>hash)&&pocLabel=="POC"&&hashLabel=="Q20_MD5"&&poc<frames&&hash.size()==32&&hash.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid complex hash log");
    require(hashes.emplace(poc,hash).second,"Duplicate complex picture hash");
  }
  require(in.eof()&&hashes.size()==frames,"Missing complex picture hashes");return hashes;
}
// PSREC002: 8-byte magic, little-endian u32 width/height, display-order frames,
// each containing two planar IEEE float32 arrays. Physical value = value*A/255.
// This is temporary decoder output, not a coded bitstream and not a depth .amp.
class ComplexReader {
  std::ifstream in;
  std::size_t pixels;
  unsigned frames;
public:
  ComplexReader(const std::filesystem::path& path,unsigned w,unsigned h,unsigned f)
    :in(path,std::ios::binary),pixels(std::size_t(w)*h),frames(f) {
    require(bool(in),"Missing precise complex decoder output");
    require(std::filesystem::file_size(path)==16+std::uint64_t(pixels)*8*f,"Complex output frame count/size differs");
    std::array<char,8> magic{};std::array<std::uint32_t,2> dims{};
    in.read(magic.data(),8);in.read(reinterpret_cast<char*>(dims.data()),8);
    require(bool(in)&&std::string(magic.data(),8)=="PSREC002"&&dims[0]==w&&dims[1]==h,"Invalid complex output header");
  }
  std::vector<float> read(unsigned frame) {
    require(frame<frames,"Complex output frame index out of range");
    std::vector<float> result(pixels*2);
    in.seekg(std::streamoff(16+std::uint64_t(frame)*pixels*8));
    in.read(reinterpret_cast<char*>(result.data()),std::streamsize(result.size()*sizeof(float)));
    require(bool(in),"Short complex decoder output");
    for(float v:result)require(std::isfinite(v),"Non-finite complex output");
    return result;
  }
};
}
