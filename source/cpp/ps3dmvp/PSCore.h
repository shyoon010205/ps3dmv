#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// Shared research syntax and fixed-point predictor; no codec-native motion map.
namespace ps3dmvp {
struct Vector { int x=0,y=0; };
struct Motion {
  bool used=false;
  int direction=0;
  int ref[2]={-1,-1},refPoc[2]={0,0};
  bool longTerm[2]={false,false};
  Vector mv[2],mvd[2];
  int mvp[2]={0,0},theta[2]={0,0};
};
struct Config {
  bool enabled=false;
  std::string bitstream,stats;
  int thetaDivisions=16,searchRange=32,candidates=4;
  bool fullSearch=true;
  bool optimizedSearch=true; // Encoder-only, keeps a reference path for byte-equivalence checks.
};
struct Plane {
  const int16_t* data=nullptr;
  ptrdiff_t stride=0;
  int width=0,height=0;
  Plane()=default;
  Plane(const int16_t* p,ptrdiff_t s,int w,int h):data(p),stride(s),width(w),height(h){}
  int at(int x,int y)const;
};
struct Ref { int poc=0;bool longTerm=false;Plane real,imag; };
struct Block { int x=0,y=0,w=0,h=0; };
struct Context {
  Block block;
  int poc=0,width=0,height=0,ctuSize=128;
  bool intra=false,bi=false,tmvp=false,checkLDC=false,colFromL0=true;
  int colRef=0;
  std::array<std::vector<Ref>,2> refs;
  std::vector<Motion> history; // latest first, VVC HMVP insertion/duplicate policy
  std::function<Motion(int,int)> spatial;
};
struct Candidate { Motion motion;double cost; };
struct Bits {
  std::vector<unsigned char> data;uint64_t count=0,pos=0;
  void put(uint64_t value,unsigned n);void ue(unsigned n);void se(int n);
  unsigned get(unsigned n);unsigned readUe();int readSe();
};
Config& config();bool enabled();
void require(bool condition,const char* message);
void validateConfig();void openEncoder();void closeEncoder();void openDecoder();void closeDecoder();
std::array<Vector,2> predictors(const Context& context,int list,int ref);
void pushHistory(std::vector<Motion>& history,const Motion& motion);
unsigned syntaxBits(const Context& context,const Motion& motion);
void encodeMotion(Bits& bits,const Context& context,const Motion& motion);
Motion decodeMotion(const Context& context);
void beginDecodePicture(const Context& context);
void writePicture(const Context& context,const Bits& bits,unsigned entries,unsigned used);
void resetMap(int poc,int width,int height);
void commitMap(int poc,const Block& block,const Motion& motion);
Motion temporalAt(int poc,int x,int y);
void predict(const Context& context,const Motion& motion,const std::array<int16_t*,3>& dst,const std::array<ptrdiff_t,3>& stride);
std::vector<Candidate> search(const Context& context,const std::array<Plane,2>& original,double lambda);
uint64_t realImagSse(const std::array<Plane,2>& original,const std::array<Plane,2>& reconstructed);
}
