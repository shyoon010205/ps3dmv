#include "../../../../cpp/ps3dmvp/SearchProfile.h"
#include "PS3DMVP.h"
#include "PSPredictionMath.h"
#include "PSFullSearch.h"
#include "TComDataCU.h"
#include "TComPic.h"
#include "TComYuv.h"
#include "libmd5/MD5.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <iomanip>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace ps3dmvp {
namespace {
Config cfg;
const Motion noMotion;
constexpr int maxMv = 8191; // The HM motion field uses quarter-sample signed shorts.
constexpr std::int64_t one = 1LL << 20;
std::vector<std::array<int,2>> rotation;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(std::string("PS3DMVP: ") + message); }
std::int64_t roundDiv(std::int64_t a, std::int64_t b) { return a < 0 ? -((-a + b/2)/b) : (a + b/2)/b; }
std::int64_t floorShift(std::int64_t a, int n) {
  const std::int64_t d = 1LL << n;
  return a < 0 ? -((-a + d - 1)/d) : a/d;
}
int clipMv(std::int64_t n) { return int(std::max<std::int64_t>(-maxMv, std::min<std::int64_t>(maxMv,n))); }
bool same(Vector a, Vector b) { return a.x == b.x && a.y == b.y; }
unsigned logBits(unsigned n) { unsigned b=0; for (unsigned v=n-1;v;v>>=1) ++b; return b; }
unsigned ueBits(unsigned n) { unsigned b=1; for (std::uint64_t v=std::uint64_t(n)+1;v>1;v>>=1) b+=2; return b; }
unsigned seCode(int n) { return n<=0 ? unsigned(-std::int64_t(n)*2) : unsigned(2*std::int64_t(n)-1); }
struct Bits {
  std::vector<unsigned char> data;
  std::uint64_t count=0, pos=0;
  void put(std::uint64_t value, unsigned n) {
    require(n<=32,"invalid fixed field width");
    for (unsigned i=n;i;--i) {
      if ((count&7)==0) data.push_back(0);
      data.back() |= static_cast<unsigned char>(((value>>(i-1))&1) << (7-(count&7)));
      ++count;
    }
  }
  void ue(unsigned n) {
    const std::uint64_t v=std::uint64_t(n)+1; unsigned k=0;
    for (auto t=v;t>1;t>>=1) ++k;
    put(0,k); put(v,k+1);
  }
  void se(int n) { ue(seCode(n)); }
  unsigned get(unsigned n) {
    require(n<=32 && pos+n<=count,"truncated payload");
    unsigned v=0;
    for(unsigned i=0;i<n;++i,++pos) v=(v<<1)|((data[size_t(pos/8)]>>(7-(pos&7)))&1);
    return v;
  }
  unsigned readUe() {
    unsigned k=0; while(get(1)==0) { ++k; require(k<31,"Exp-Golomb overflow"); }
    return ((1u<<k)-1) + get(k);
  }
  int readSe() { const unsigned v=readUe(); return (v&1) ? int((v+1)/2) : -int(v/2); }
};
struct StreamReader {
  std::ifstream file;
  unsigned current=0, remaining=0;
  unsigned bit() {
    if(!remaining) { char c; require(bool(file.get(c)),"truncated side stream"); current=static_cast<unsigned char>(c);remaining=8; }
    return (current>>(--remaining))&1;
  }
  unsigned fixed(unsigned n) { unsigned v=0;for(unsigned i=0;i<n;++i)v=(v<<1)|bit();return v; }
  unsigned ue() { unsigned k=0;while(!bit()){++k;require(k<31,"frame field overflow");} return ((1u<<k)-1)+fixed(k); }
  int se() { const auto n=ue(); return n&1 ? int((n+1)/2) : -int(n/2); }
  void align() { while(remaining)require(bit()==0,"nonzero alignment bits"); }
};
std::ofstream output, statistics, rdAudit;
std::uint64_t trialCount=0, beatingNativeCount=0;
double bestDelta=std::numeric_limits<double>::max(), bestDistortionDelta=0, bestBitsDelta=0;
std::ofstream complexOutput;
bool complexHeaderWritten=false;
void openComplexOutput() {
  complexHeaderWritten=false;
  if(!cfg.complexRecon.empty()) {
    complexOutput.open(cfg.complexRecon,std::ios::binary|std::ios::trunc);
    require(bool(complexOutput),"cannot open complex reconstruction output");
  }
}
void closeComplexOutput() {
  if(complexOutput.is_open()){complexOutput.flush();require(bool(complexOutput),"complex output write failed");complexOutput.close();}
}
StreamReader input;
Bits payload;
unsigned order=0, expectedEntries=0, consumedEntries=0;
int activePoc=0;
bool active=false;
std::uint64_t fileBits=0;
void writeBytes(const std::vector<unsigned char>& v) {
  if(!v.empty()) output.write(reinterpret_cast<const char*>(v.data()),std::streamsize(v.size()));
  require(bool(output),"side stream write failed");
  fileBits += v.size()*8;
}
void makeRotation() {
  // Angles are integer turns (2^32 = 2*pi). Q30 CORDIC, rounded to Q20.
  static const std::int64_t atanTurns[] = {
    536870912,316933406,167458907,85004756,42667331,21354465,10679838,5340245,
    2670163,1335087,667544,333772,166886,83443,41722,20861,
    10430,5215,2608,1304,652,326,163,81,41,20,10,5,3,1,1
  };
  rotation.resize(cfg.thetaDivisions);
  for(int q=0;q<cfg.thetaDivisions;++q) {
    if ((std::int64_t(q)*4)%cfg.thetaDivisions==0) {
      const int quadrant=int((std::int64_t(q)*4)/cfg.thetaDivisions);
      rotation[q] = quadrant==0 ? std::array<int,2>{{int(one),0}} :
                    quadrant==1 ? std::array<int,2>{{0,int(one)}} :
                    quadrant==2 ? std::array<int,2>{{-int(one),0}} :
                                  std::array<int,2>{{0,-int(one)}};
      continue;
    }
    std::int64_t z=roundDiv(std::int64_t(q)*(1LL<<32),cfg.thetaDivisions);
    if(z>(1LL<<31)) z-=1LL<<32;
    int sign=1;
    if(z>(1LL<<30)){z-=1LL<<31;sign=-1;}
    if(z<-(1LL<<30)){z+=1LL<<31;sign=-1;}
    std::int64_t x=652032874,y=0;
    for(int k=0;k<31;++k) {
      const int d=z>=0?1:-1;
      const auto nx=x-d*floorShift(y,k);
      y+=d*floorShift(x,k);x=nx;z-=d*atanTurns[k];
    }
    rotation[q]={{int(roundDiv(sign*x,1<<10)),int(roundDiv(sign*y,1<<10))}};
  }
}
struct Block { int x,y,w,h; unsigned ctu,z; };
Block block(TComDataCU* cu,unsigned part,unsigned depth) {
  const unsigned z=cu->getZorderIdxInCtu()+part;
  const auto* sps=cu->getSlice()->getSPS();
  return {int((cu->getCtuRsAddr()%cu->getPic()->getFrameWidthInCtus())*sps->getMaxCUWidth()+g_auiRasterToPelX[g_auiZscanToRaster[z]]),
          int((cu->getCtuRsAddr()/cu->getPic()->getFrameWidthInCtus())*sps->getMaxCUHeight()+g_auiRasterToPelY[g_auiZscanToRaster[z]]),
          int(sps->getMaxCUWidth()>>depth),int(sps->getMaxCUHeight()>>depth),cu->getCtuRsAddr(),z};
}
const Motion* at(TComPic* pic,int x,int y,const Block* availableBefore) {
  const auto& sps=pic->getPicSym()->getSPS();
  if(x<0||y<0||x>=int(sps.getPicWidthInLumaSamples())||y>=int(sps.getPicHeightInLumaSamples()))return nullptr;
  const unsigned addr=unsigned(y)/sps.getMaxCUHeight()*pic->getFrameWidthInCtus()+unsigned(x)/sps.getMaxCUWidth();
  const unsigned raster=(unsigned(y)%sps.getMaxCUHeight())/pic->getMinCUHeight()*pic->getNumPartInCtuWidth()+(unsigned(x)%sps.getMaxCUWidth())/pic->getMinCUWidth();
  const unsigned z=g_auiRasterToZscan[raster];
  if(availableBefore && (addr>availableBefore->ctu||(addr==availableBefore->ctu&&z>=availableBefore->z)))return nullptr;
  const Motion& m=availableBefore ? pic->getCtu(addr)->getPSMotion(z) : pic->getPicSym()->getPSReferenceMotion(addr,z);
  return m.used ? &m : nullptr;
}
bool spatial(const Motion* m,int list,int refPoc,bool longTerm,Vector& mv) {
  if(!m)return false;
  for(int i=0;i<2;++i) {
    const int l=i?1-list:list;
    if((m->direction&(1<<l)) && m->refPoc[l]==refPoc && m->longTerm[l]==longTerm) {mv=m->mv[l];return true;}
  }
  return false; // VVC unscaled spatial/HMVP: matching reference POC.
}
int compressedVvcMv(int integer) {
  const int val=integer*16, sign=val<0?-1:0;
  unsigned magnitude=unsigned(val^sign)|31u;int log=0;
  while(magnitude>>=1)++log;
  const int shift=log-5;
  if(shift<0)return val;
  const int n=int(floorShift(std::int64_t(val)+((1<<shift)>>1),shift));
  const int exponent=shift+((n^sign)>>5);
  const int mantissa=(n&31)|(sign*32);
  return exponent==0?mantissa:(mantissa^32)*(1<<(exponent-1));
}
Vector scaleTemporal(Vector v,int currentPoc,int currentRefPoc,int collocatedPoc,int collocatedRefPoc,bool longTerm) {
  const bool unscaled=longTerm || collocatedPoc-collocatedRefPoc==currentPoc-currentRefPoc;
  int td=std::max(-128,std::min(127,collocatedPoc-collocatedRefPoc));
  int tb=std::max(-128,std::min(127,currentPoc-currentRefPoc));
  if(!unscaled&&td==0)return {};
  int tx=unscaled?0:(16384+std::abs(td/2))/td;
  int scale=std::max(-4096,std::min(4095,int(floorShift(std::int64_t(tb)*tx+32,6))));
  // VVC compressed 1/16-sample temporal MV, distance scaling and IMV rounding.
  auto convert=[&](int n) {
    std::int64_t q=compressedVvcMv(n);
    if(!unscaled){const auto p=std::int64_t(scale)*q;q=floorShift(p+128-(p>=0),8);}
    q=std::max<std::int64_t>(-(1<<17),std::min<std::int64_t>((1<<17)-1,q));
    return clipMv(floorShift(q+8-(q>=0),4));
  };
  return {convert(v.x),convert(v.y)};
}
void resolveMvd(TComDataCU* cu,Motion& m,const std::array<Vector,2>* cached=nullptr) {
  const auto depth=cu->getDepth(0);
  for(int l=0;l<2;++l) if(m.direction&(1<<l)) {
    const auto p=cached ? *cached : predictors(cu,0,depth,l,m.ref[l]);
    unsigned best=std::numeric_limits<unsigned>::max();
    for(int i=0;i<2;++i) {
      Vector d={m.mv[l].x-p[i].x,m.mv[l].y-p[i].y};
      unsigned bits=ueBits(seCode(d.x))+ueBits(seCode(d.y));
      if(bits<best) {best=bits;m.mvp[l]=i;m.mvd[l]=d;}
    }
  }
}
std::array<std::int64_t,2> rotated(TComDataCU* cu,const Motion& m,int l,int x,int y) {
  auto* pic=cu->getSlice()->getRefPic(RefPicList(l),m.ref[l]);
  auto* rec=pic->getPSFinal();
  require(rec!=nullptr,"PS final reference not reconstructed");
  x=std::max(0,std::min(int(rec->getWidth(COMPONENT_Y))-1,x+m.mv[l].x));
  y=std::max(0,std::min(int(rec->getHeight(COMPONENT_Y))-1,y+m.mv[l].y));
  const auto index=size_t(y)*rec->getWidth(COMPONENT_Y)+x;
  const auto bounded=cfg.optimizedSearch
    ? std::array<std::int64_t,2>{{pic->getPSBounded(0).at(index),pic->getPSBounded(1).at(index)}}
    : amplitudeLimited({{pic->getPSComplex(0)[index],pic->getPSComplex(1)[index]}});
  const auto r=bounded[0],i=bounded[1];
  const auto c=rotation[m.theta[l]][0],s=rotation[m.theta[l]][1];
  return amplitudeLimited({{roundDiv(std::int64_t(c)*r-std::int64_t(s)*i,one),roundDiv(std::int64_t(s)*r+std::int64_t(c)*i,one)}});
}
void encodeMotion(Bits& bits,TComDataCU* cu,const Motion& m) {
  bits.put(m.used,1);if(!m.used)return;
  if(cu->getSlice()->isInterB()) bits.put(unsigned(m.direction-1),2);
  for(int l=0;l<2;++l) if(m.direction&(1<<l)) {
    if(cu->getSlice()->getNumRefIdx(RefPicList(l))>1) bits.ue(m.ref[l]);
    bits.put(m.mvp[l],1);bits.se(m.mvd[l].x);bits.se(m.mvd[l].y);
    bits.put(m.theta[l],logBits(cfg.thetaDivisions));
  }
}
void finishPayload() {
  if(active) require(consumedEntries==expectedEntries && payload.pos==payload.count,"frame payload count/length mismatch");
}
void walkPicture(TComDataCU* cu,unsigned part,unsigned depth,Bits& bits,unsigned& entries,unsigned& used) {
  const auto b=block(cu,part,depth);
  const auto* sps=cu->getSlice()->getSPS();
  if(b.x>=int(sps->getPicWidthInLumaSamples())||b.y>=int(sps->getPicHeightInLumaSamples()))return;
  if(depth<cu->getDepth(part)) {
    unsigned step=cu->getPic()->getNumPartitionsInCtu()>>(2*(depth+1));
    for(unsigned i=0;i<4;++i)walkPicture(cu,part+i*step,depth+1,bits,entries,used);
    return;
  }
  if(cu->getSlice()->isIntra())return;
  const auto& m=cu->getPSMotion(part);
  if(m.used) {
    require(cu->isInter(part)&&!cu->isSkipped(part)&&!cu->getMergeFlag(part)&&cu->getPartitionSize(part)==SIZE_2Nx2N,"direct PS mode state changed after RDO");
    const unsigned count=cu->getPic()->getNumPartitionsInCtu()>>(2*depth);
    for(int l=0;l<2;++l)for(unsigned i=part;i<part+count;++i) {
      const bool present=(m.direction&(1<<l))!=0;const auto* field=cu->getCUMvField(RefPicList(l));
      require(field->getRefIdx(i)==(present?m.ref[l]:-1),"final direct PS reference map mismatch");
      require(field->getMv(i).getHor()==(present?4*m.mv[l].x:0)&&field->getMv(i).getVer()==(present?4*m.mv[l].y:0),"final direct PS motion map mismatch");
    }
  }
  // The final tree must use exactly the predictor state used during RDO.
  if(m.used)for(int l=0;l<2;++l)if(m.direction&(1<<l)) {
    const auto p=predictors(cu,part,depth,l,m.ref[l]);
    require(m.mv[l].x==p[m.mvp[l]].x+m.mvd[l].x && m.mv[l].y==p[m.mvp[l]].y+m.mvd[l].y,"RDO/final motion-map mismatch");
  }
  encodeMotion(bits,cu,m);++entries;if(m.used){
    ++used;
  }
}
}
void recordDirectTrial(double psCost,double nativeCost,double distortionDelta,double totalBitsDelta) {
  ++trialCount;if(psCost<nativeCost)++beatingNativeCount;
  const double delta=psCost-nativeCost;
  if(delta<bestDelta){bestDelta=delta;bestDistortionDelta=distortionDelta;bestBitsDelta=totalBitsDelta;}
}
Config& config(){return cfg;}
bool enabled(){return cfg.enabled;}
const Motion& emptyMotion(){return noMotion;}
void validateConfig() {
  if(!enabled())return;
  require(!cfg.bitstream.empty(),"PSBitstream is required when On");
  require(cfg.thetaDivisions>=1&&cfg.thetaDivisions<=1024,"theta divisions must be 1..1024");
  require(cfg.searchRange>=0&&cfg.searchRange<=256,"search range must be 0..256 integer pixels");
  require(cfg.candidates>=1&&cfg.candidates<=16,"RD candidates must be 1..16");
}
void openEncoder() {
  if(!enabled())return;
  validateConfig();makeRotation();order=0;fileBits=0;trialCount=beatingNativeCount=0;bestDelta=std::numeric_limits<double>::max();
  openComplexOutput();
  output.open(cfg.bitstream,std::ios::binary|std::ios::trunc);require(bool(output),"cannot open PSBitstream output");
  const char magic[8]={'P','S','3','D','M','V','P','\0'};
  output.write(magic,8);fileBits=64;
  require(cfg.fullSearch,"PS integer Full Search is required; set PS3DMVPFullSearch=1");
  Bits h;h.put(5,16);h.ue(cfg.thetaDivisions-1);writeBytes(h.data);
  output.flush();require(bool(output),"PS header flush failed");
  if(!cfg.stats.empty()) {
    statistics.open(cfg.stats,std::ios::trunc);require(bool(statistics),"cannot open PSStats");
    rdAudit.open(cfg.stats+".rd.csv",std::ios::trunc);require(bool(rdAudit),"cannot open direct RD audit");
    rdAudit<<std::setprecision(17)<<"poc,tried_ps_candidates,candidates_beating_native,best_rd_delta,best_distortion_delta,best_total_bits_delta\n";
    statistics<<"decode_order,poc,entries,ps_cus,payload_bits,packet_bits,cumulative_file_bits\n";
    statistics.flush();rdAudit.flush();
  }
}
void openDecoder() {
  if(!enabled())return;
  require(!cfg.bitstream.empty(),"PSBitstream input is required when On");
  input.file.open(cfg.bitstream,std::ios::binary);require(bool(input.file),"cannot open PSBitstream input");
  char magic[8]={};input.file.read(magic,8);
  require(input.file.gcount()==8&&std::memcmp(magic,"PS3DMVP",8)==0,"invalid PS magic");
  require(input.fixed(16)==5,"requires direct PS prediction syntax version 5");
  cfg.thetaDivisions=int(input.ue())+1;input.align();validateConfig();makeRotation();
  active=false;order=0;
  openComplexOutput();
}
void closeEncoder() {
  if(!enabled())return;
  closeComplexOutput();
  output.flush();require(bool(output),"side stream flush failed");output.close();
  if(rdAudit.is_open()){rdAudit.flush();require(bool(rdAudit),"RD audit write failed");rdAudit.close();}
  if(statistics.is_open()){statistics.flush();require(bool(statistics),"statistics write failed");statistics.close();}
}
void closeDecoder() {
  if(!enabled())return;
  closeComplexOutput();
  finishPayload();require(input.file.peek()==std::char_traits<char>::eof(),"unconsumed side-stream packets");
  input.file.close();
}
void validatePicture(TComPic* pic) {
  if(!enabled())return;
  const auto& s=pic->getPicSym()->getSPS();const auto& p=pic->getPicSym()->getPPS();
  require(s.getChromaFormatIdc()==CHROMA_444 && s.getBitDepth(CHANNEL_TYPE_LUMA)==8 && s.getBitDepth(CHANNEL_TYPE_CHROMA)==8,"requires planar 444 with 8-bit input/internal/output");
  require(p.getNumTileColumnsMinus1()==0 && p.getNumTileRowsMinus1()==0 && !p.getEntropyCodingSyncEnabledFlag(),"direct v5 requires one tile and WPP off");
  require(!s.getUseSAO() && pic->getSlice(0)->getDeblockingFilterDisable(),"direct v5 requires deblocking and SAO off");
  require(!pic->isField(),"direct v5 requires progressive pictures");
  require(!p.getUseWP()&&!p.getWPBiPred(),"direct v5 uses unweighted uni and equal-weight bi prediction");
}
void writePicture(TComPic* pic) {
  if(!enabled())return;
  psinstrument::profile().flush(cfg.stats,pic->getPOC());
  validatePicture(pic);
  require(pic->getNumAllocatedSlice()==1,"v1 requires one slice per picture");
  Bits b;unsigned entries=0,used=0;
  for(unsigned i=0;i<pic->getNumberOfCtusInFrame();++i)walkPicture(pic->getCtu(i),0,0,b,entries,used);
  require(b.count<std::uint64_t(1)<<30,"side payload too large");
  Bits h;h.ue(order);h.se(pic->getPOC());h.ue(unsigned(b.count));h.ue(entries);
  const auto before=fileBits;writeBytes(h.data);writeBytes(b.data);
  if(statistics.is_open())statistics<<order<<","<<pic->getPOC()<<","<<entries<<","<<used<<","<<b.count<<","<<(fileBits-before)<<","<<fileBits<<"\n";
  if(rdAudit.is_open()) {
    rdAudit<<pic->getPOC()<<","<<trialCount<<","<<beatingNativeCount<<",";
    if(trialCount)rdAudit<<bestDelta<<","<<bestDistortionDelta<<","<<bestBitsDelta;
    else rdAudit<<"0,0,0";
    rdAudit<<"\n";rdAudit.flush();require(bool(rdAudit),"RD audit flush failed");
  }
  output.flush();require(bool(output),"PS picture flush failed");
  if(statistics.is_open()){statistics.flush();require(bool(statistics),"PS statistics flush failed");}
  printf("[PS-DIRECT] POC %d selected %u / %u leaf CUs; payload %llu bits\n",
         pic->getPOC(),used,entries,static_cast<unsigned long long>(b.count));fflush(stdout);
  trialCount=beatingNativeCount=0;bestDelta=std::numeric_limits<double>::max();
  ++order;
}
void beginDecodePicture(TComDataCU* cu) {
  if(!enabled())return;
  // A new picture always starts at CTU 0 in the supported single-slice layout.
  if(cu->getCtuRsAddr()!=0)return;
  finishPayload();validatePicture(cu->getPic());
  const auto decodedOrder=input.ue();const int poc=input.se();const auto count=input.ue();const auto entries=input.ue();input.align();
  require(decodedOrder==order && poc==cu->getSlice()->getPOC(),"anchor/PS picture order or POC mismatch");
  require(count<=64u*1024*1024 && entries<=cu->getPic()->getNumberOfCtusInFrame()*cu->getPic()->getNumPartitionsInCtu(),"invalid frame length/entry count");
  payload=Bits();payload.count=count;payload.data.resize((count+7)/8);
  if(!payload.data.empty())input.file.read(reinterpret_cast<char*>(payload.data.data()),std::streamsize(payload.data.size()));
  require(bool(input.file),"truncated frame payload");
  if(count%8)require((payload.data.back()&((1u<<(8-count%8))-1))==0,"nonzero payload padding");
  expectedEntries=entries;consumedEntries=0;active=true;activePoc=poc;++order;
  if(cu->getSlice()->isIntra())require(count==0&&entries==0,"I picture has PS payload");
}
std::array<Vector,2> predictors(TComDataCU* cu,unsigned part,unsigned depth,int list,int ref) {
  require(list>=0&&list<2&&ref>=0&&ref<cu->getSlice()->getNumRefIdx(RefPicList(list)),"invalid reference for AMVP");
  const auto b=block(cu,part,depth);
  auto* slice=cu->getSlice();auto* pic=cu->getPic();
  const int refPoc=slice->getRefPOC(RefPicList(list),ref);
  const bool lt=slice->getIsUsedAsLongTerm(list,ref);
  std::vector<Vector> result;
  auto add=[&](Vector v){if(result.size()<2&&(result.empty()||!same(result[0],v)))result.push_back(v);};
  Vector v;
  if(spatial(at(pic,b.x-1,b.y+b.h,&b),list,refPoc,lt,v)||spatial(at(pic,b.x-1,b.y+b.h-1,&b),list,refPoc,lt,v))add(v);
  if(spatial(at(pic,b.x+b.w,b.y-1,&b),list,refPoc,lt,v)||spatial(at(pic,b.x+b.w-1,b.y-1,&b),list,refPoc,lt,v)||spatial(at(pic,b.x-1,b.y-1,&b),list,refPoc,lt,v))add(v);
  if(result.size()<2 && slice->getEnableTMVPFlag()) {
    const int colList=slice->isInterB() ? (slice->getColFromL0Flag()?0:1) : 0;
    const int colRef=int(slice->getColRefIdx());
    if(colRef<slice->getNumRefIdx(RefPicList(colList))) {
      auto* colPic=slice->getRefPic(RefPicList(colList),colRef);
      auto temporal=[&](int x,int y) {
        // VVenC AMVP_DECIMATION_FACTOR=2: the collocated grid is 8 samples.
        const Motion* m=at(colPic,(x>>3)<<3,(y>>3)<<3,nullptr);
        if(!m)return false;
        int l=slice->getCheckLDC()?list:(slice->getColFromL0Flag()?1:0);
        if(!(m->direction&(1<<l)))l=1-l;
        if(!(m->direction&(1<<l))||lt!=m->longTerm[l])return false;
        if(!lt && colPic->getPOC()==m->refPoc[l])return false;
        v=scaleTemporal(m->mv[l],slice->getPOC(),refPoc,colPic->getPOC(),m->refPoc[l],lt);
        return true;
      };
      const int ctuHeight=int(slice->getSPS()->getMaxCUHeight());
      const bool bottomRightAvailable=((b.y+b.h)%ctuHeight)!=0;
      if((bottomRightAvailable&&temporal(b.x+b.w,b.y+b.h))||temporal(b.x+b.w/2,b.y+b.h/2))add(v);
    }
  }
  // Derive the last four unique PS history entries from the committed coding
  // prefix, resetting at CTU rows. No mutable history survives rejected RDO.
  std::set<std::tuple<int,int,int,int,int,int,int,int>> seen;
  unsigned lastCtu=b.ctu;
  int z=int(b.z)-1;
  const unsigned rowStart=b.ctu/pic->getFrameWidthInCtus()*pic->getFrameWidthInCtus();
  unsigned visited=0;
  while(result.size()<2 && visited<4) {
    if(z<0) {
      if(lastCtu==rowStart)break;
      --lastCtu;z=int(pic->getNumPartitionsInCtu())-1;
    }
    const auto& m=pic->getCtu(lastCtu)->getPSMotion(unsigned(z));
    if(m.used) {
      const auto key=std::make_tuple(m.direction,m.refPoc[0],m.refPoc[1],m.mv[0].x,m.mv[0].y,m.mv[1].x,m.mv[1].y,int(m.longTerm[0])+2*int(m.longTerm[1]));
      if(seen.insert(key).second) {
        ++visited;
        for(int source=0;source<2;++source) {
          const int l=source?1-list:list;
          if((m.direction&(1<<l))&&m.refPoc[l]==refPoc&&m.longTerm[l]==lt)add(m.mv[l]);
        }
      }
      z=m.origin>=0?std::min(z-1,m.origin-1):z-1;
    } else --z;
  }
  while(result.size()<2)result.push_back({});
  return {{result[0],result[1]}};
}
void applyMotion(TComDataCU* cu,unsigned part,unsigned depth,const Motion& source) {
  Motion m=source;m.origin=int(cu->getZorderIdxInCtu()+part);
  cu->setPSMotionSubParts(m,part,depth);
  if(!m.used)return;
  cu->setPredModeSubParts(MODE_INTER,part,depth);
  cu->setPartSizeSubParts(SIZE_2Nx2N,part,depth);
  cu->setSkipFlagSubParts(false,part,depth);
  cu->setMergeFlagSubParts(false,part,0,depth);
  cu->setMergeIndexSubParts(0,part,0,depth);
  cu->setIPCMFlagSubParts(false,part,depth);
  cu->setInterDirSubParts(m.direction,part,0,depth);
  // Motion-field setters use depth relative to their own allocated buffer.
  // Encoder RDO buffers are already CU-sized; decoder buffers are CTU-sized.
  const unsigned count=cu->getPic()->getNumPartitionsInCtu()>>(2*depth);
  unsigned extent=cu->getTotalNumPart(),relativeDepth=0;
  require(count>0 && part+count<=extent,"invalid PS motion field extent");
  while(extent>count){extent>>=2;++relativeDepth;}
  require(extent==count,"PS motion field depth mismatch");
  for(int l=0;l<2;++l) {
    const bool used=(m.direction&(1<<l))!=0;const auto list=RefPicList(l);
    TComMvField field;field.setMvField(TComMv(used?Short(4*m.mv[l].x):0,used?Short(4*m.mv[l].y):0),used?m.ref[l]:-1);
    cu->getCUMvField(list)->setAllMvField(field,SIZE_2Nx2N,part,relativeDepth);
    // PS MVD belongs only to the side stream; no native MVD syntax is coded.
    cu->getCUMvField(list)->setAllMvd(TComMv(),SIZE_2Nx2N,part,relativeDepth);
    cu->setMVPIdxSubParts(used?m.mvp[l]:-1,list,part,0,depth);
    cu->setMVPNumSubParts(used?2:0,list,part,0,depth);
    for(unsigned i=part;i<part+count;++i) {
      require(cu->getCUMvField(list)->getRefIdx(i)==(used?m.ref[l]:-1),"PS reference field extent mismatch");
      const auto& v=cu->getCUMvField(list)->getMv(i);
      require(v.getHor()==(used?4*m.mv[l].x:0)&&v.getVer()==(used?4*m.mv[l].y:0),"PS motion field extent mismatch");
    }
  }
}
void decodeLeaf(TComDataCU* cu,unsigned part,unsigned depth) {
  if(!enabled()||cu->getSlice()->isIntra())return;
  require(active&&activePoc==cu->getSlice()->getPOC()&&consumedEntries<expectedEntries,"CU side stream is out of sync");
  Motion m;m.used=payload.get(1)!=0;++consumedEntries;
  if(m.used) {
    m.direction=cu->getSlice()->isInterB()?int(payload.get(2))+1:1;
    require(m.direction>=1&&m.direction<=3,"reserved prediction direction");
    for(int l=0;l<2;++l) if(m.direction&(1<<l)) {
      const int refs=cu->getSlice()->getNumRefIdx(RefPicList(l));
      require(refs>0,"empty active reference list");
      m.ref[l]=refs>1?int(payload.readUe()):0;require(m.ref[l]<refs,"reference index out of range");
      m.refPoc[l]=cu->getSlice()->getRefPOC(RefPicList(l),m.ref[l]);
      m.longTerm[l]=cu->getSlice()->getIsUsedAsLongTerm(l,m.ref[l]);
      const auto p=predictors(cu,part,depth,l,m.ref[l]);m.mvp[l]=int(payload.get(1));
      m.mvd[l]={payload.readSe(),payload.readSe()};
      const auto x=std::int64_t(p[m.mvp[l]].x)+m.mvd[l].x,y=std::int64_t(p[m.mvp[l]].y)+m.mvd[l].y;
      require(x>=-maxMv&&x<=maxMv&&y>=-maxMv&&y<=maxMv,"MV out of range");
      m.mv[l]={int(x),int(y)};m.theta[l]=int(payload.get(logBits(cfg.thetaDivisions)));
      require(m.theta[l]<cfg.thetaDivisions,"unused phase index");
    }
  }
  applyMotion(cu,part,depth,m);
}
unsigned syntaxBits(TComDataCU* cu,const Motion& m) {
  if(!enabled()||cu->getSlice()->isIntra())return 0;
  if(cfg.optimizedSearch) {
    unsigned count=1;
    if(!m.used)return count;
    if(cu->getSlice()->isInterB())count+=2;
    for(int l=0;l<2;++l)if(m.direction&(1<<l)) {
      if(cu->getSlice()->getNumRefIdx(RefPicList(l))>1)count+=ueBits(m.ref[l]);
      count+=1+ueBits(seCode(m.mvd[l].x))+ueBits(seCode(m.mvd[l].y))+logBits(cfg.thetaDivisions);
    }
    return count;
  }
  Bits bits;encodeMotion(bits,cu,m);return unsigned(bits.count);
}
namespace {
std::array<std::int64_t,2> prediction(TComDataCU* cu,const Motion& m,int x,int y) {
  std::array<std::int64_t,2> sum={{0,0}};int n=0;
  for(int l=0;l<2;++l) if(m.direction&(1<<l)) {
    const auto p=rotated(cu,m,l,x,y);sum[0]+=p[0];sum[1]+=p[1];++n;
  }
  require(n>0,"empty PS prediction");
  for(auto& v:sum)v=roundDiv(v,n);
  return amplitudeLimited(sum); // Preserve the radial bound after bi-average rounding.
}
void restoreLeaf(TComDataCU* cu,unsigned part,unsigned depth) {
  const auto b=block(cu,part,depth);auto* pic=cu->getPic();
  auto* src=pic->getPicYuvRec();auto* dst=pic->getPSFinal();
  if(b.x>=int(src->getWidth(COMPONENT_Y))||b.y>=int(src->getHeight(COMPONENT_Y)))return;
  if(depth<cu->getDepth(part)) {
    const unsigned step=pic->getNumPartitionsInCtu()>>(2*(depth+1));
    for(unsigned i=0;i<4;++i)restoreLeaf(cu,part+i*step,depth+1);
    return;
  }
  for(int y=b.y;y<std::min(b.y+b.h,int(src->getHeight(COMPONENT_Y)));++y)
    for(int x=b.x;x<std::min(b.x+b.w,int(src->getWidth(COMPONENT_Y)));++x) {
      for(int c=0;c<2;++c) {
        const auto id=ComponentID(c);const int v=src->getAddr(id)[y*src->getStride(id)+x];
        pic->getPSComplex(c)[size_t(y)*src->getWidth(COMPONENT_Y)+x]=(2LL*v-255)*one;
        dst->getAddr(id)[y*dst->getStride(id)+x]=Pel(v);
      }
      dst->getAddr(COMPONENT_Cr)[y*dst->getStride(COMPONENT_Cr)+x]=0;
    }
}
}
void makePrediction(TComDataCU* cu,TComYuv* dest) {
  const auto& m=cu->getPSMotion(0);require(m.used,"direct predictor needs PS motion");
  const auto b=block(cu,0,cu->getDepth(0));
  for(int y=0;y<b.h;++y)for(int x=0;x<b.w;++x) {
    const auto p=prediction(cu,m,b.x+x,b.y+y);
    for(int c=0;c<2;++c) {
      const auto id=ComponentID(c);
      dest->getAddr(id)[y*dest->getStride(id)+x]=Pel(predictionSample(p[c]));
    }
    dest->getAddr(COMPONENT_Cr)[y*dest->getStride(COMPONENT_Cr)+x]=0;
  }
}
void reconstructPicture(TComPic* pic) {
  if(!enabled())return;
  validatePicture(pic);pic->preparePSFinal();
  for(unsigned i=0;i<pic->getNumberOfCtusInFrame();++i)restoreLeaf(pic->getCtu(i),0,0);
  // Refresh on every completed picture, including reused DPB objects. Never
  // replace the unclipped field or modify decoder-visible prediction math.
  if(cfg.optimizedSearch) {
    const auto n=pic->getPSComplex(0).size();
    for(unsigned c=0;c<2;++c)pic->getPSBounded(c).resize(n);
    for(size_t i=0;i<n;++i) {
      const auto bounded=amplitudeLimited({{pic->getPSComplex(0)[i],pic->getPSComplex(1)[i]}});
      for(unsigned c=0;c<2;++c)pic->getPSBounded(c)[i]=std::int32_t(bounded[c]);
    }
  }
  MD5 digest;
  for(int c=0;c<2;++c) {
    auto& plane=pic->getPSComplex(c);
    digest.update(reinterpret_cast<unsigned char*>(plane.data()),unsigned(plane.size()*sizeof(std::int64_t)));
  }
  unsigned char md5[16];digest.finalize(md5);
  printf("\nPSFINAL POC %d Q20_MD5 ",pic->getPOC());for(auto byte:md5)printf("%02x",unsigned(byte));printf("\n");
  if(complexOutput.is_open()) {
    const std::uint32_t dimensions[2]={std::uint32_t(pic->getPicYuvRec()->getWidth(COMPONENT_Y)),std::uint32_t(pic->getPicYuvRec()->getHeight(COMPONENT_Y))};
    if(!complexHeaderWritten) {
      complexOutput.write("PSREC002",8);complexOutput.write(reinterpret_cast<const char*>(dimensions),8);complexHeaderWritten=true;
    }
    require(pic->getPOC()>=0,"negative export POC");
    const auto pixels=size_t(dimensions[0])*dimensions[1];
    complexOutput.seekp(std::streamoff(16+std::uint64_t(pic->getPOC())*pixels*8));
    std::vector<float> plane(pixels);
    for(int c=0;c<2;++c) {
      for(size_t i=0;i<pixels;++i)plane[i]=float(double(pic->getPSComplex(c)[i])/one);
      complexOutput.write(reinterpret_cast<const char*>(plane.data()),std::streamsize(plane.size()*sizeof(float)));
    }
    require(bool(complexOutput),"complex reconstruction write failed");
  }
}
std::uint64_t realImagSse(TComYuv* a,TComYuv* b,unsigned w,unsigned h) {
  std::uint64_t s=0;
  for(int c=0;c<2;++c)for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
    const int d=int(a->getAddr(ComponentID(c))[y*a->getStride(ComponentID(c))+x])-int(b->getAddr(ComponentID(c))[y*b->getStride(ComponentID(c))+x]);
    s+=std::uint64_t(d*d);
  }
  return s;
}

std::vector<Candidate> search(TComDataCU* cu,TComYuv* org,double lambda) {
  const auto b=block(cu,0,cu->getDepth(0));
  psinstrument::SearchMeasure measure;
  const int keep=cfg.candidates;
  std::vector<Candidate> uni[2], all;
  auto equivalent=[](const Motion& a,const Motion& d) {
    if(a.direction!=d.direction)return false;
    for(int l=0;l<2;++l)if(a.direction&(1<<l))
      if(a.ref[l]!=d.ref[l]||!same(a.mv[l],d.mv[l])||a.theta[l]!=d.theta[l])return false;
    return true;
  };
  auto retain=[&](std::vector<Candidate>& out,const Candidate& c) {
    // upper_bound appends equal costs: preserve that exact tie policy.
    if(cfg.optimizedSearch && int(out.size())==keep && c.cost>=out.back().cost)return;
    for(const auto& old:out)if(equivalent(old.motion,c.motion))return;
    auto it=std::upper_bound(out.begin(),out.end(),c.cost,[](double value,const Candidate& item){return value<item.cost;});
    out.insert(it,c);if(int(out.size())>keep)out.pop_back();
  };
  // Exact centered original samples, prepared once per CU.
  std::vector<std::array<int,2>> centered(size_t(b.w)*b.h);
  std::int64_t eOrg=0;
  for(int y=0;y<b.h;++y)for(int x=0;x<b.w;++x)for(int c=0;c<2;++c) {
    const int v=2*int(org->getAddr(ComponentID(c))[y*org->getStride(ComponentID(c))+x])-255;eOrg+=v*v;centered[size_t(y)*b.w+x][c]=v;
  }
  for(int l=0;l<(cu->getSlice()->isInterB()?2:1);++l)for(int ref=0;ref<cu->getSlice()->getNumRefIdx(RefPicList(l));++ref) {
    Motion m;m.used=true;m.direction=1<<l;m.ref[l]=ref;
    m.refPoc[l]=cu->getSlice()->getRefPOC(RefPicList(l),ref);
    m.longTerm[l]=cu->getSlice()->getIsUsedAsLongTerm(l,ref);
    auto* rec=cu->getSlice()->getRefPic(RefPicList(l),ref)->getPSFinal();
    require(rec!=nullptr,"PS search reference not reconstructed");
    const auto pv=predictors(cu,0,cu->getDepth(0),l,ref);
    std::map<std::pair<int,int>,double> cache;
    std::unordered_set<std::uint32_t> visited;
    auto* refPic=cu->getSlice()->getRefPic(RefPicList(l),ref);
    const int refWidth=rec->getWidth(COMPONENT_Y),refHeight=rec->getHeight(COMPONENT_Y);
    if(cfg.optimizedSearch) {
      visited.reserve(size_t(2*cfg.searchRange+1)*(2*cfg.searchRange+1)*3);
      require(refPic->getPSBounded(0).size()==size_t(refWidth)*refHeight,"PS reference cache not finalized");
    }
    auto evaluate=[&](Vector mv) {
      const auto key=std::make_pair(mv.x,mv.y);
      if(cfg.optimizedSearch) {
        const auto packed=(std::uint32_t(mv.y+maxMv)<<14)|std::uint32_t(mv.x+maxMv);
        if(!visited.insert(packed).second)return 0.0; // Scan ignores returned score.
      } else {auto found=cache.find(key);if(found!=cache.end())return found->second;}
      ++psinstrument::profile().mvs;psinstrument::profile().thetas+=cfg.thetaDivisions;psinstrument::profile().samples+=uint64_t(b.w)*b.h;
      double eRef=0,dot=0,cross=0;
      const int x0=b.x+mv.x,y0=b.y+mv.y;
      const bool interior=cfg.optimizedSearch&&x0>=0&&y0>=0&&x0+b.w<=refWidth&&y0+b.h<=refHeight;
      for(int y=0;y<b.h;++y)for(int x=0;x<b.w;++x) {
        const int rx=interior?x0+x:std::max(0,std::min(refWidth-1,b.x+x+mv.x));
        const int ry=interior?y0+y:std::max(0,std::min(refHeight-1,b.y+y+mv.y));
        const auto index=size_t(ry)*refWidth+rx;
        const auto bounded=cfg.optimizedSearch
          ? std::array<std::int64_t,2>{{refPic->getPSBounded(0)[index],refPic->getPSBounded(1)[index]}}
          : amplitudeLimited({{refPic->getPSComplex(0)[index],refPic->getPSComplex(1)[index]}});
        const double rr=double(bounded[0])/one;
        const double ri=double(bounded[1])/one;
        const int ar=cfg.optimizedSearch?centered[size_t(y)*b.w+x][0]:2*int(org->getAddr(COMPONENT_Y)[y*org->getStride(COMPONENT_Y)+x])-255;
        const int ai=cfg.optimizedSearch?centered[size_t(y)*b.w+x][1]:2*int(org->getAddr(COMPONENT_Cb)[y*org->getStride(COMPONENT_Cb)+x])-255;
        eRef+=rr*rr+ri*ri;dot+=ar*rr+ai*ri;cross+=ai*rr-ar*ri;
      }
      m.mv[l]=mv;resolveMvd(cu,m,cfg.optimizedSearch?&pv:nullptr);double best=std::numeric_limits<double>::max();
      const unsigned invariantBits=cfg.optimizedSearch?syntaxBits(cu,m):0; // theta is fixed-length.
      for(int q=0;q<cfg.thetaDivisions;++q) {
        m.theta[l]=q;const double c=double(rotation[q][0])/one,s=double(rotation[q][1])/one;
        // Complex SSE in the inverse-mapped domain; common amplitude^2
        // cancels with lambda. No SAD, subsampling, or fractional MV search.
        const double distortion=std::max(0.0,(double(eOrg)+(c*c+s*s)*double(eRef)-2*(c*dot+s*cross))/4.0);
        Candidate candidate{m,distortion+lambda*(cfg.optimizedSearch?invariantBits:syntaxBits(cu,m))};
        retain(uni[l],candidate);best=std::min(best,candidate.cost);
      }
      if(!cfg.optimizedSearch)cache.emplace(key,best);return best;
    };
    const std::array<Vector,3> seeds={{pv[0],pv[1],Vector{}}};
    for(auto seed:seeds) {
      seed={clipMv(seed.x),clipMv(seed.y)};
      const int minX=clipMv(seed.x-cfg.searchRange),maxX=clipMv(seed.x+cfg.searchRange);
      const int minY=clipMv(seed.y-cfg.searchRange),maxY=clipMv(seed.y+cfg.searchRange);
      fullSearch(minX,maxX,minY,maxY,[&](int x,int y){evaluate({x,y});});
    }
  }
  for(int l=0;l<2;++l)for(const auto& c:uni[l])retain(all,c);
  // Evaluate Cartesian pairs of the shortlisted L0/L1 integer-motion/phase
  // candidates. Each list keeps its own reference, MVP, MVD and phase.
  measure.beginBi();
  psinstrument::profile().peakScratch=std::max(psinstrument::profile().peakScratch,uint64_t(b.w)*b.h*(sizeof(std::array<int,2>)+(cfg.optimizedSearch?(uni[0].size()+uni[1].size())*sizeof(std::array<std::int64_t,2>):0)));
  // Cache the exact pre-average Q20 vectors. No 8-bit quantization or
  // floating reassociation is introduced; pairs retain their original order.
  using RotatedBlock=std::vector<std::array<std::int64_t,2>>;
  std::vector<RotatedBlock> biCache[2];
  if(cfg.optimizedSearch&&!uni[0].empty()&&!uni[1].empty()) {
    for(int l=0;l<2;++l)for(const auto& candidate:uni[l]) {
      RotatedBlock samples(size_t(b.w)*b.h);
      for(int y=0;y<b.h;++y)for(int x=0;x<b.w;++x)
        samples[size_t(y)*b.w+x]=rotated(cu,candidate.motion,l,b.x+x,b.y+y);
      biCache[l].push_back(std::move(samples));
    }
  }
  for(const auto& a:uni[0])for(const auto& d:uni[1]) {
    ++psinstrument::profile().pairs;
    Motion m=a.motion;m.direction=3;
    m.ref[1]=d.motion.ref[1];m.refPoc[1]=d.motion.refPoc[1];m.longTerm[1]=d.motion.longTerm[1];
    m.mv[1]=d.motion.mv[1];m.mvd[1]=d.motion.mvd[1];m.mvp[1]=d.motion.mvp[1];m.theta[1]=d.motion.theta[1];
    long double error=0;
    for(int y=0;y<b.h;++y)for(int x=0;x<b.w;++x) {
      const size_t sample=size_t(y)*b.w+x;
      const auto p0=cfg.optimizedSearch?biCache[0][size_t(&a-uni[0].data())][sample]:rotated(cu,m,0,b.x+x,b.y+y);
      const auto p1=cfg.optimizedSearch?biCache[1][size_t(&d-uni[1].data())][sample]:rotated(cu,m,1,b.x+x,b.y+y);
      for(int c=0;c<2;++c) {
        const auto original=2*int(org->getAddr(ComponentID(c))[y*org->getStride(ComponentID(c))+x])-255;
        const long double delta=static_cast<long double>(original)-(static_cast<long double>(p0[c])+p1[c])/(2*one);
        error+=delta*delta/4;
      }
    }
    retain(all,{m,double(error)+lambda*syntaxBits(cu,m)});
  }
  return all;
}
} // namespace ps3dmvp


