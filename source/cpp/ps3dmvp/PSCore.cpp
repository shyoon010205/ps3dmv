#include "SearchProfile.h"
#include "PSCore.h"
#include "../../HM-PS3DMVP-Direct/source/Lib/TLibCommon/PSPredictionMath.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
namespace ps3dmvp {
namespace { Config cfg; }
void require(bool v,const char* m){if(!v)throw std::runtime_error(std::string("PS3DMVP: ")+m);}
namespace {
constexpr int maxMv = 8191; // Integer bound fits VVC signed 18-bit 1/16-sample motion fields.
constexpr std::int64_t one = 1LL << 20;
std::vector<std::array<int,2>> rotation;
std::vector<double> rotationReal,rotationImag,rotationNorm;

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
  // These values are invariant for every MV evaluation.  Precomputing them
  // preserves the exact expressions and candidate order while removing the
  // repeated integer-to-double conversion and norm calculation from the hot
  // full-search/theta loop.
  rotationReal.resize(cfg.thetaDivisions);
  rotationImag.resize(cfg.thetaDivisions);
  rotationNorm.resize(cfg.thetaDivisions);
  for(int q=0;q<cfg.thetaDivisions;++q) {
    const double c=double(rotation[q][0])/one;
    const double s=double(rotation[q][1])/one;
    rotationReal[q]=c;
    rotationImag[q]=s;
    rotationNorm[q]=c*c+s*s;
  }
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

struct FrameMap { int width=0,height=0;std::vector<Motion> cells; };
struct Packet { Bits bits;unsigned expected=0,consumed=0,order=0;bool begun=false;int width=0,height=0; };
std::map<int,FrameMap> maps;
std::map<int,Packet> packets;
std::recursive_mutex stateMutex;
std::ofstream output,statistics;
unsigned order=0;uint64_t fileBits=0;
void writeBytes(const std::vector<unsigned char>& v){if(!v.empty())output.write(reinterpret_cast<const char*>(v.data()),std::streamsize(v.size()));require(bool(output),"side stream write failed");fileBits+=v.size()*8;}
struct Reader {
  std::ifstream file;unsigned current=0,remaining=0;
  unsigned bit(){if(!remaining){char c;require(bool(file.get(c)),"truncated side stream");current=(unsigned char)c;remaining=8;}return(current>>(--remaining))&1;}
  unsigned fixed(unsigned n){unsigned v=0;for(unsigned i=0;i<n;++i)v=(v<<1)|bit();return v;}
  unsigned ue(){unsigned k=0;while(!bit()){++k;require(k<31,"packet field overflow");}return((1u<<k)-1)+fixed(k);}
  int se(){unsigned n=ue();return n&1?int((n+1)/2):-int(n/2);}
  void align(){while(remaining)require(bit()==0,"nonzero alignment bits");}
};
bool sameMotion(const Motion& a,const Motion& b){
  if(a.direction!=b.direction)return false;
  for(int l=0;l<2;++l)if((a.direction&(1<<l))&&(a.refPoc[l]!=b.refPoc[l]||a.longTerm[l]!=b.longTerm[l]||!same(a.mv[l],b.mv[l])))return false;
  return true;
}
bool spatial(const Motion& m,int list,int poc,bool lt,Vector& mv){
  if(!m.used)return false;
  for(int source=0;source<2;++source){int l=source?1-list:list;if((m.direction&(1<<l))&&m.refPoc[l]==poc&&m.longTerm[l]==lt){mv=m.mv[l];return true;}}
  return false;
}
void resolveMvd(const Context& ctx,Motion& m,const std::array<Vector,2>* cached=nullptr){
  for(int l=0;l<2;++l)if(m.direction&(1<<l)){
    const auto p=cached?*cached:predictors(ctx,l,m.ref[l]);unsigned best=std::numeric_limits<unsigned>::max();
    for(int i=0;i<2;++i){Vector d={m.mv[l].x-p[i].x,m.mv[l].y-p[i].y};unsigned bits=ueBits(seCode(d.x))+ueBits(seCode(d.y));
      if(bits<best){best=bits;m.mvp[l]=i;m.mvd[l]=d;}}
  }
}
const std::array<int64_t,2>& projectedPair(int real,int imag) {
  require(real>=0&&real<256&&imag>=0&&imag<256,"PS reference sample is not 8-bit");
  static const std::vector<std::array<int64_t,2>> table=[](){
    std::vector<std::array<int64_t,2>> values(65536);
    for(int r=0;r<256;++r)for(int i=0;i<256;++i)
      values[size_t(r)*256+i]=amplitudeLimited({{int64_t(2*r-255)*one,int64_t(2*i-255)*one}});
    return values;
  }();
  psinstrument::profile().lookupBytes=65536*sizeof(std::array<std::int64_t,2>);
  return table[size_t(real)*256+imag];
}
std::array<int64_t,2> rotated(const Context& ctx,const Motion& m,int l,int x,int y){
  const auto& r=ctx.refs[l][m.ref[l]];x=std::max(0,std::min(r.real.width-1,x+m.mv[l].x));y=std::max(0,std::min(r.real.height-1,y+m.mv[l].y));
  const auto v=projectedPair(r.real.at(x,y),r.imag.at(x,y));
  const int c=rotation[m.theta[l]][0],s=rotation[m.theta[l]][1];
  return {{roundDiv(c*v[0]-s*v[1],one),roundDiv(s*v[0]+c*v[1],one)}};
}
}
Config& config(){return cfg;}bool enabled(){return cfg.enabled;}
int Plane::at(int x,int y)const{require(data&&x>=0&&y>=0&&x<width&&y<height,"invalid plane access");return data[y*stride+x];}
void Bits::put(uint64_t value,unsigned n){require(n<=32,"invalid fixed field width");for(unsigned i=n;i;--i){if(!(count&7))data.push_back(0);data.back()|=(unsigned char)(((value>>(i-1))&1)<<(7-(count&7)));++count;}}
void Bits::ue(unsigned n){const uint64_t v=uint64_t(n)+1;unsigned k=0;for(auto t=v;t>1;t>>=1)++k;put(0,k);put(v,k+1);}
void Bits::se(int n){ue(seCode(n));}
unsigned Bits::get(unsigned n){require(n<=32&&pos+n<=count,"truncated CU payload");unsigned v=0;for(unsigned i=0;i<n;++i,++pos)v=(v<<1)|((data[size_t(pos/8)]>>(7-(pos&7)))&1);return v;}
unsigned Bits::readUe(){unsigned k=0;while(!get(1)){++k;require(k<31,"Exp-Golomb overflow");}return((1u<<k)-1)+get(k);}
int Bits::readSe(){const unsigned v=readUe();return v&1?int((v+1)/2):-int(v/2);}
void validateConfig(){if(!enabled())return;require(!cfg.bitstream.empty(),"PSBitstream required when On");require(cfg.thetaDivisions>=1&&cfg.thetaDivisions<=1024,"phase divisions must be 1..1024");require(cfg.searchRange>=0&&cfg.searchRange<=256,"integer search radius must be 0..256");require(cfg.candidates>=1&&cfg.candidates<=16,"RDO candidates must be 1..16");require(cfg.fullSearch,"Only Full Search is supported");}
void resetMap(int poc,int w,int h){std::lock_guard<std::recursive_mutex> lock(stateMutex);auto& m=maps[poc];m.width=w;m.height=h;m.cells.assign(size_t((w+3)/4)*((h+3)/4),Motion());}
void commitMap(int poc,const Block& b,const Motion& motion){std::lock_guard<std::recursive_mutex> lock(stateMutex);auto& m=maps.at(poc);const int stride=(m.width+3)/4;
  for(int y=b.y/4;y<(b.y+b.h+3)/4&&y<(m.height+3)/4;++y)for(int x=b.x/4;x<(b.x+b.w+3)/4&&x<stride;++x)m.cells[size_t(y)*stride+x]=motion;}
Motion temporalAt(int poc,int x,int y){std::lock_guard<std::recursive_mutex> lock(stateMutex);auto f=maps.find(poc);if(f==maps.end())return Motion();const auto& m=f->second;
  if(x<0||y<0||x>=m.width||y>=m.height)return Motion();return m.cells[size_t(y/4)*((m.width+3)/4)+x/4];}
void pushHistory(std::vector<Motion>& history,const Motion& m){if(!m.used)return;history.erase(std::remove_if(history.begin(),history.end(),[&](const Motion& v){return sameMotion(v,m);}),history.end());history.insert(history.begin(),m);if(history.size()>6)history.resize(6);}
std::array<Vector,2> predictors(const Context& ctx,int list,int ref){
  require(list>=0&&list<2&&ref>=0&&ref<int(ctx.refs[list].size()),"AMVP reference out of range");const auto& r=ctx.refs[list][ref];const auto b=ctx.block;std::vector<Vector> result;Vector v;
  auto at=[&](int x,int y){return ctx.spatial?ctx.spatial(x,y):Motion();};
  if(spatial(at(b.x-1,b.y+b.h),list,r.poc,r.longTerm,v)||spatial(at(b.x-1,b.y+b.h-1),list,r.poc,r.longTerm,v))result.push_back(v);
  if(spatial(at(b.x+b.w,b.y-1),list,r.poc,r.longTerm,v)||spatial(at(b.x+b.w-1,b.y-1),list,r.poc,r.longTerm,v)||spatial(at(b.x-1,b.y-1),list,r.poc,r.longTerm,v))result.push_back(v);
  if(result.size()==2&&same(result[0],result[1]))result.pop_back();
  if(result.size()<2&&ctx.tmvp&&b.w+b.h>12){int colList=ctx.bi?(ctx.colFromL0?0:1):0;
    if(ctx.colRef<int(ctx.refs[colList].size())){const int colPoc=ctx.refs[colList][ctx.colRef].poc;
      auto temporal=[&](int x,int y){const Motion m=temporalAt(colPoc,(x>>3)<<3,(y>>3)<<3);if(!m.used)return false;
        int l=ctx.checkLDC?list:(ctx.colFromL0?1:0);if(!(m.direction&(1<<l)))l=1-l;
        if(!(m.direction&(1<<l))||r.longTerm!=m.longTerm[l]||(!r.longTerm&&colPoc==m.refPoc[l]))return false;
        v=scaleTemporal(m.mv[l],ctx.poc,r.poc,colPoc,m.refPoc[l],r.longTerm);return true;};
      const bool c0=b.x+b.w<ctx.width&&b.y+b.h<ctx.height&&(b.y+b.h)%ctx.ctuSize!=0;
      if((c0&&temporal(b.x+b.w,b.y+b.h))||temporal(b.x+b.w/2,b.y+b.h/2))result.push_back(v);
    }
  }
  for(size_t i=0;i<std::min(size_t(4),ctx.history.size())&&result.size()<2;++i){const Motion& m=ctx.history[i];for(int s=0;s<2&&result.size()<2;++s){int l=s?1-list:list;if(m.used&&(m.direction&(1<<l))&&m.refPoc[l]==r.poc&&m.longTerm[l]==r.longTerm)result.push_back(m.mv[l]);}}
  while(result.size()<2)result.push_back(Vector());return{{result[0],result[1]}};
}
void encodeMotion(Bits& bits,const Context& ctx,const Motion& m){
  if(ctx.intra)return;bits.put(m.used,1);if(!m.used)return;if(ctx.bi)bits.put(unsigned(m.direction-1),2);
  for(int l=0;l<2;++l)if(m.direction&(1<<l)){if(ctx.refs[l].size()>1)bits.ue(m.ref[l]);bits.put(m.mvp[l],1);bits.se(m.mvd[l].x);bits.se(m.mvd[l].y);bits.put(m.theta[l],logBits(cfg.thetaDivisions));}
}
unsigned syntaxBits(const Context& c,const Motion& m){
  if(!enabled()||c.intra)return 0;
  if(cfg.optimizedSearch){
    unsigned bits=1;if(!m.used)return bits;if(c.bi)bits+=2;
    for(int l=0;l<2;++l)if(m.direction&(1<<l)){
      if(c.refs[l].size()>1)bits+=ueBits(m.ref[l]);
      bits+=1+ueBits(seCode(m.mvd[l].x))+ueBits(seCode(m.mvd[l].y))+logBits(cfg.thetaDivisions);
    }
    return bits;
  }
  Bits b;encodeMotion(b,c,m);return unsigned(b.count);
}
void openEncoder(){if(!enabled())return;validateConfig();makeRotation();maps.clear();order=0;fileBits=0;output.open(cfg.bitstream,std::ios::binary|std::ios::trunc);require(bool(output),"cannot create PS bitstream");const char magic[8]={'P','S','V','V','C','0','0','1'};output.write(magic,8);fileBits=64;Bits h;h.put(1,16);h.ue(cfg.thetaDivisions-1);writeBytes(h.data);
  if(!cfg.stats.empty()){statistics.open(cfg.stats,std::ios::trunc);require(bool(statistics),"cannot create PS statistics");statistics<<"decode_order,poc,entries,ps_cus,payload_bits,packet_bits,cumulative_file_bits\n";}}
void writePicture(const Context& ctx,const Bits& b,unsigned entries,unsigned used){if(!enabled())return;psinstrument::profile().flush(cfg.stats,ctx.poc);require(b.count<(uint64_t(1)<<30),"payload too large");Bits h;h.ue(order);h.se(ctx.poc);h.ue(ctx.width);h.ue(ctx.height);h.ue(unsigned(b.count));h.ue(entries);const auto before=fileBits;writeBytes(h.data);writeBytes(b.data);if(statistics.is_open())statistics<<order<<','<<ctx.poc<<','<<entries<<','<<used<<','<<b.count<<','<<fileBits-before<<','<<fileBits<<'\n';++order;}
void closeEncoder(){if(!enabled())return;output.flush();require(bool(output),"PS stream flush failed");output.close();if(statistics.is_open()){statistics.flush();require(bool(statistics),"PS stats flush failed");statistics.close();}}
void openDecoder(){if(!enabled())return;validateConfig();Reader in;in.file.open(cfg.bitstream,std::ios::binary);require(bool(in.file),"cannot open PS bitstream");char magic[8];in.file.read(magic,8);require(in.file.gcount()==8&&std::memcmp(magic,"PSVVC001",8)==0,"not a VVC PS side stream");require(in.fixed(16)==1,"unsupported PS syntax version");cfg.thetaDivisions=int(in.ue())+1;in.align();validateConfig();makeRotation();maps.clear();packets.clear();order=0;
  while(in.file.peek()!=std::char_traits<char>::eof()){Packet p;p.order=in.ue();int poc=in.se();p.width=int(in.ue());p.height=int(in.ue());p.bits.count=in.ue();p.expected=in.ue();in.align();require(p.order==order++&&poc>=0&&p.width>0&&p.height>0&&p.width<=16384&&p.height<=16384&&p.bits.count<=64u*1024*1024&&p.expected<=unsigned((p.width+3)/4)*unsigned((p.height+3)/4),"invalid PS packet header");
    require(packets.count(poc)==0,"duplicate PS POC");p.bits.data.resize(size_t((p.bits.count+7)/8));if(!p.bits.data.empty())in.file.read(reinterpret_cast<char*>(p.bits.data.data()),std::streamsize(p.bits.data.size()));require(bool(in.file),"truncated PS packet");if(p.bits.count%8)require((p.bits.data.back()&((1u<<(8-p.bits.count%8))-1))==0,"nonzero payload padding");packets.emplace(poc,std::move(p));}
  require(!packets.empty(),"empty PS stream");}
void beginDecodePicture(const Context& ctx){if(!enabled())return;std::lock_guard<std::recursive_mutex> lock(stateMutex);auto it=packets.find(ctx.poc);require(it!=packets.end(),"missing PS picture");auto& p=it->second;require(!p.begun&&ctx.width==p.width&&ctx.height==p.height,"PS picture dimensions/order mismatch");p.begun=true;resetMap(ctx.poc,ctx.width,ctx.height);if(ctx.intra)require(p.expected==0&&p.bits.count==0,"I picture has PS entries");}
Motion decodeMotion(const Context& ctx){Motion m;if(!enabled()||ctx.intra)return m;std::lock_guard<std::recursive_mutex> lock(stateMutex);auto& p=packets.at(ctx.poc);require(p.begun&&p.consumed<p.expected,"PS CU count mismatch");m.used=p.bits.get(1)!=0;++p.consumed;
  if(m.used){m.direction=ctx.bi?int(p.bits.get(2))+1:1;require(m.direction>=1&&m.direction<=3,"reserved PS direction");for(int l=0;l<2;++l)if(m.direction&(1<<l)){int refs=int(ctx.refs[l].size());require(refs>0,"empty active reference list");m.ref[l]=refs>1?int(p.bits.readUe()):0;require(m.ref[l]>=0&&m.ref[l]<refs,"PS reference out of range");m.refPoc[l]=ctx.refs[l][m.ref[l]].poc;m.longTerm[l]=ctx.refs[l][m.ref[l]].longTerm;auto v=predictors(ctx,l,m.ref[l]);m.mvp[l]=int(p.bits.get(1));m.mvd[l]={p.bits.readSe(),p.bits.readSe()};auto x=int64_t(v[m.mvp[l]].x)+m.mvd[l].x,y=int64_t(v[m.mvp[l]].y)+m.mvd[l].y;require(x>=-maxMv&&x<=maxMv&&y>=-maxMv&&y<=maxMv,"PS motion out of range");m.mv[l]={int(x),int(y)};m.theta[l]=int(p.bits.get(logBits(cfg.thetaDivisions)));require(m.theta[l]<cfg.thetaDivisions,"unused phase index");}}
  return m;}
void closeDecoder(){if(!enabled())return;std::lock_guard<std::recursive_mutex> lock(stateMutex);for(const auto& item:packets){const auto& p=item.second;require(p.begun&&p.consumed==p.expected&&p.bits.pos==p.bits.count,"unconsumed PS packets/CUs/bits");}}
void predict(const Context& ctx,const Motion& m,const std::array<int16_t*,3>& dst,const std::array<ptrdiff_t,3>& stride){require(m.used,"PS predictor on native CU");const auto b=ctx.block;
  for(int y=0;y<b.h;++y)for(int x=0;x<b.w;++x){std::array<int64_t,2> sum={{0,0}};int n=0;for(int l=0;l<2;++l)if(m.direction&(1<<l)){auto p=rotated(ctx,m,l,b.x+x,b.y+y);sum[0]+=p[0];sum[1]+=p[1];++n;}require(n>0,"empty PS prediction");for(int c=0;c<2;++c){const auto mapped=roundDiv(sum[c]+255*one*n,2*one*n);dst[c][y*stride[c]+x]=int16_t(std::max<int64_t>(0,std::min<int64_t>(255,mapped)));}dst[2][y*stride[2]+x]=0;}}
uint64_t realImagSse(const std::array<Plane,2>& a,const std::array<Plane,2>& b){uint64_t s=0;for(int c=0;c<2;++c)for(int y=0;y<a[c].height;++y)for(int x=0;x<a[c].width;++x){int d=a[c].at(x,y)-b[c].at(x,y);s+=uint64_t(d*d);}return s;}
// All 8-bit reference pairs are projected with the existing Q20 routine.
// A 1 MiB lookup avoids repeating the identical square root at every MV.
// Non-8-bit inputs use the unchanged scalar routine. Initialization is included
// in encoder wall time and guarded by C++ static initialization.
std::array<std::int64_t,2> cachedProjectedPair(int real,int imag) {
  if(real<0||real>255||imag<0||imag>255)return projectedPair(real,imag);
  static const auto table=[](){
    std::vector<std::array<std::int64_t,2>> values(65536);
    for(int r=0;r<256;++r)for(int i=0;i<256;++i)values[size_t(r)*256+i]=projectedPair(r,i);
    return values;
  }();
  return table[size_t(real)*256+imag];
}

std::vector<Candidate> search(const Context& ctx,const std::array<Plane,2>& org,double lambda) {
  const auto b=ctx.block;
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
    // Equal-cost candidates originally append, then the last is removed.
    if(cfg.optimizedSearch&&int(out.size())==keep&&c.cost>=out.back().cost)return;
    for(const auto& old:out)if(equivalent(old.motion,c.motion))return;
    auto it=std::upper_bound(out.begin(),out.end(),c.cost,[](double value,const Candidate& item){return value<item.cost;});
    out.insert(it,c);if(int(out.size())>keep)out.pop_back();
  };
  // Exact centered original samples, prepared once per CU.
  std::vector<std::array<int,2>> centered(size_t(b.w)*b.h);
  std::int64_t eOrg=0;
  for(int y=0;y<b.h;++y)for(int x=0;x<b.w;++x)for(int c=0;c<2;++c) {
    const int v=2*org[c].at(x,y)-255;eOrg+=v*v;centered[size_t(y)*b.w+x][c]=v;
  }
  for(int l=0;l<(ctx.bi?2:1);++l)for(int ref=0;ref<int(ctx.refs[l].size());++ref) {
    Motion m;m.used=true;m.direction=1<<l;m.ref[l]=ref;
    m.refPoc[l]=ctx.refs[l][ref].poc;
    m.longTerm[l]=ctx.refs[l][ref].longTerm;
    const auto& rec=ctx.refs[l][ref];
    const auto pv=predictors(ctx,l,ref);
    std::map<std::pair<int,int>,double> cache;
    std::unordered_set<uint32_t> visited;
    const bool fast=cfg.optimizedSearch&&cfg.fullSearch;
    if(fast)visited.reserve(size_t(2*cfg.searchRange+1)*(2*cfg.searchRange+1)*3);
    auto evaluate=[&](Vector mv) {
      const auto key=std::make_pair(mv.x,mv.y);
      if(fast){const auto packed=(uint32_t(mv.y+maxMv)<<14)|uint32_t(mv.x+maxMv);if(!visited.insert(packed).second)return 0.0;}
      else{auto found=cache.find(key);if(found!=cache.end())return found->second;}
      ++psinstrument::profile().mvs;psinstrument::profile().thetas+=cfg.thetaDivisions;psinstrument::profile().samples+=uint64_t(b.w)*b.h;
      long double eRef=0,dot=0,cross=0;
      const int x0=b.x+mv.x,y0=b.y+mv.y;
      const bool interior=cfg.optimizedSearch&&x0>=0&&y0>=0&&x0+b.w<=rec.real.width&&y0+b.h<=rec.real.height;
      for(int y=0;y<b.h;++y)for(int x=0;x<b.w;++x) {
        const int rx=interior?x0+x:std::max(0,std::min(rec.real.width-1,b.x+x+mv.x));
        const int ry=interior?y0+y:std::max(0,std::min(rec.real.height-1,b.y+y+mv.y));
        const auto projected=cfg.optimizedSearch?cachedProjectedPair(rec.real.at(rx,ry),rec.imag.at(rx,ry)):projectedPair(rec.real.at(rx,ry),rec.imag.at(rx,ry));
        const double rr=double(projected[0])/one;
        const double ri=double(projected[1])/one;
        const int ar=cfg.optimizedSearch?centered[size_t(y)*b.w+x][0]:2*org[0].at(x,y)-255;
        const int ai=cfg.optimizedSearch?centered[size_t(y)*b.w+x][1]:2*org[1].at(x,y)-255;
        eRef+=rr*rr+ri*ri;dot+=ar*rr+ai*ri;cross+=ai*rr-ar*ri;
      }
      m.mv[l]=mv;resolveMvd(ctx,m,cfg.optimizedSearch?&pv:nullptr);double best=std::numeric_limits<double>::max();
      const unsigned invariantBits=cfg.optimizedSearch?syntaxBits(ctx,m):0; // All theta indices have the same length.
      for(int q=0;q<cfg.thetaDivisions;++q) {
        m.theta[l]=q;const double c=rotationReal[q],s=rotationImag[q];
        // Complex SSE in the inverse-mapped domain; common amplitude^2
        // cancels with lambda. No SAD, subsampling, or fractional MV search.
        const double distortion=double(std::max(0.0L,(eOrg+rotationNorm[q]*eRef-2*(c*dot+s*cross))/4.0L));
        Candidate candidate{m,distortion+lambda*(cfg.optimizedSearch?invariantBits:syntaxBits(ctx,m))};
        retain(uni[l],candidate);best=std::min(best,candidate.cost);
      }
      if(!fast)cache.emplace(key,best);return best;
    };
    const std::array<Vector,3> seeds={{pv[0],pv[1],Vector{}}};
    for(auto seed:seeds) {
      seed={clipMv(seed.x),clipMv(seed.y)};
      const int minX=clipMv(seed.x-cfg.searchRange),maxX=clipMv(seed.x+cfg.searchRange);
      const int minY=clipMv(seed.y-cfg.searchRange),maxY=clipMv(seed.y+cfg.searchRange);
      if(cfg.fullSearch) {
        for(int y=minY;y<=maxY;++y)for(int x=minX;x<=maxX;++x)evaluate({x,y});
      } else {
        Vector current=seed;double currentCost=evaluate(current);
        int step=1;while(step<=cfg.searchRange/2)step*=2;
        for(;step;step/=2) {
          bool moved;
          do {
            moved=false;Vector winner=current;double cost=currentCost;
            const std::array<Vector,4> trial={{{current.x-step,current.y},{current.x+step,current.y},{current.x,current.y-step},{current.x,current.y+step}}};
            for(const auto& v:trial)if(v.x>=minX&&v.x<=maxX&&v.y>=minY&&v.y<=maxY) {
              const double value=evaluate(v);if(value<cost){cost=value;winner=v;moved=true;}
            }
            current=winner;currentCost=cost;
          } while(moved);
        }
      }
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
  if(cfg.optimizedSearch&&!uni[0].empty()&&!uni[1].empty()&&b.w+b.h>12) {
    for(int l=0;l<2;++l)for(const auto& candidate:uni[l]) {
      RotatedBlock samples(size_t(b.w)*b.h);
      for(int y=0;y<b.h;++y)for(int x=0;x<b.w;++x)
        samples[size_t(y)*b.w+x]=rotated(ctx,candidate.motion,l,b.x+x,b.y+y);
      biCache[l].push_back(std::move(samples));
    }
  }
  if(b.w+b.h>12)for(const auto& a:uni[0])for(const auto& d:uni[1]) {
    ++psinstrument::profile().pairs;
    Motion m=a.motion;m.direction=3;
    m.ref[1]=d.motion.ref[1];m.refPoc[1]=d.motion.refPoc[1];m.longTerm[1]=d.motion.longTerm[1];
    m.mv[1]=d.motion.mv[1];m.mvd[1]=d.motion.mvd[1];m.mvp[1]=d.motion.mvp[1];m.theta[1]=d.motion.theta[1];
    long double error=0;
    for(int y=0;y<b.h;++y)for(int x=0;x<b.w;++x) {
      const size_t sample=size_t(y)*b.w+x;
      const auto p0=cfg.optimizedSearch?biCache[0][size_t(&a-uni[0].data())][sample]:rotated(ctx,m,0,b.x+x,b.y+y);
      const auto p1=cfg.optimizedSearch?biCache[1][size_t(&d-uni[1].data())][sample]:rotated(ctx,m,1,b.x+x,b.y+y);
      for(int c=0;c<2;++c) {
        const auto original=2*org[c].at(x,y)-255;
        const long double delta=static_cast<long double>(original)-(static_cast<long double>(p0[c])+p1[c])/(2*one);
        error+=delta*delta/4;
      }
    }
    retain(all,{m,double(error)+lambda*syntaxBits(ctx,m)});
  }
  return all;
}

} // namespace ps3dmvp
