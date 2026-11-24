#include "PS3DMVP.h"
#include "../../../../../cpp/ps3dmvp/ModeArea.h"
#include "CodingStructure.h"
#include "Picture.h"
#include "UnitTools.h"
#include "../../../../../cpp/ps3dmvp/PSCore.cpp"
namespace vvdec { namespace ps {
namespace { ps3dmvp::Plane plane(const CPelBuf& b){static_assert(sizeof(Pel)==sizeof(int16_t),"PS requires 16-bit Pel storage");return{b.buf,b.stride,int(b.width),int(b.height)};} }

ps3dmvp::Context context(const CodingUnit& cu){ps3dmvp::Context c;const auto& s=*cu.slice;const auto& cs=*cu.cs;
  c.block={cu.lx(),cu.ly(),int(cu.lwidth()),int(cu.lheight())};c.poc=s.getPOC();c.width=cs.pcv->lumaWidth;c.height=cs.pcv->lumaHeight;c.ctuSize=cs.pcv->maxCUWidth;
  c.intra=s.isIntra();c.bi=s.isInterB();c.tmvp=cs.picHeader->getEnableTMVPFlag();c.checkLDC=s.getCheckLDC();c.colFromL0=s.getColFromL0Flag();c.colRef=int(s.getColRefIdx());
  for(int l=0;l<2;++l)for(int i=0;i<s.getNumRefIdx(RefPicList(l));++i){const auto* p=s.getRefPic(RefPicList(l),i);ps3dmvp::Ref r;r.poc=p->getPOC();r.longTerm=s.getIsUsedAsLongTerm(l,i);r.real=plane(p->getRecoBuf(COMPONENT_Y));r.imag=plane(p->getRecoBuf(COMPONENT_Cb));c.refs[l].push_back(r);}
  c.spatial=[&cu](int x,int y){const auto* n=cu.cs->getCURestricted(Position(x,y),cu,CH_L);return n&&n!=&cu?n->psMotion:ps3dmvp::Motion();};return c;
}
void beginCtu(CodingStructure& cs,const Slice& slice,unsigned addr){if(!ps3dmvp::enabled())return;
  ps3dmvp::require(cs.pps->getNumTiles()==1&&cs.pps->getNumSlicesInPic()==1,"VVC PS requires one tile and one slice");
  ps3dmvp::require(!cs.sps->getUseDualITree(),"PS3DMVP=1 requires DualITree=0; the bitstream enables Dual tree");
  ps3dmvp::require(cs.pcv->chrFormat==CHROMA_444&&cs.sps->getBitDepth()==8&&!cs.sps->getEntropyCodingSyncEnabledFlag(),"PS requires 8bit 444 and WPP off");
  if(addr==0){ps3dmvp::Context c;c.poc=slice.getPOC();c.width=cs.pcv->lumaWidth;c.height=cs.pcv->lumaHeight;c.intra=slice.isIntra();ps3dmvp::beginDecodePicture(c);}
  if(addr%cs.pcv->widthInCtus==0)cs.psHistory.clear();
}
void apply(CodingUnit& cu,const ps3dmvp::Motion& m){cu.psMotion=m;if(!m.used)return;
  cu.setPredMode(MODE_INTER);cu.setSkip(false);cu.setMergeFlag(false);cu.setMmvdFlag(false);cu.setAffineFlag(false);cu.setGeoFlag(false);cu.setCiipFlag(false);cu.setImv(IMV_FPEL);cu.setBcwIdx(BCW_DEFAULT);cu.setSmvdMode(0);cu.setInterDir(uint8_t(m.direction));cu.setDmvrCondition(false);
  for(int l=0;l<2;++l){const bool use=(m.direction&(1<<l))!=0;cu.refIdx[l]=use?m.ref[l]:-1;cu.mv[l][0]=Mv(use?16*m.mv[l].x:0,use?16*m.mv[l].y:0);cu.mvpIdx[l]=0;}
}
void decodeLeaf(CodingUnit& cu){if(!ps3dmvp::enabled()||cu.chType()!=CH_L)return;auto c=context(cu);c.history=cu.cs->psHistory;auto m=ps3dmvp::decodeMotion(c);apply(cu,m);ps3dmvp::commitMap(c.poc,c.block,m);ps3dmvp::pushHistory(cu.cs->psHistory,m);}
void predict(const CodingUnit& cu,UnitBuf<short>& out){ps3dmvp::predict(context(cu),cu.psMotion,{{out.Y().buf,out.Cb().buf,out.Cr().buf}},{{out.Y().stride,out.Cb().stride,out.Cr().stride}});}

void writeAreas(const Picture& pic) {
  if(ps3dmvp::modeStatsPath().empty())return;
  const auto& cs=*pic.cs;ps3dmvp::ModeArea counts;
  const int width=cs.pcv->lumaWidth,height=cs.pcv->lumaHeight;
  std::set<const CodingUnit*> visited;
  for(int y=0;y<height;y+=4)for(int x=0;x<width;x+=4) {
    const Position pos(x,y);const auto* cu=cs.getCU(pos,CH_L);
    ps3dmvp::require(cu!=nullptr,"missing final CU for area statistics");
    if(!visited.insert(cu).second)continue;
    const uint64_t area=uint64_t(std::min(int(cu->lwidth()),width-cu->lx()))*std::min(int(cu->lheight()),height-cu->ly());
    const bool intra=CU::isIntra(*cu),ps=cu->psMotion.used;
    ps3dmvp::require(!CU::isIBC(*cu),"area schema requires IBC off");
    const bool skip=cu->skip(),merge=cu->mergeFlag(),affine=cu->affineFlag(),mmvd=cu->mmvdFlag(),geo=cu->geoFlag(),ciip=cu->ciipFlag(),mip=cu->mipFlag(),isp=cu->ispMode()!=0;
    const int bdpcm=cu->bdpcmMode();
    const std::string mode=ps?"PS":intra?"INTRA":skip?"INTER_SKIP":merge?"INTER_MERGE":"INTER_AMVP";
    const std::string detail=ps?"PS":intra?(bdpcm?"INTRA_BDPCM":mip?"INTRA_MIP":isp?"INTRA_ISP":cu->intraDir[0]==0?"INTRA_PLANAR":cu->intraDir[0]==1?"INTRA_DC":"INTRA_ANGULAR"):
      skip?"INTER_SKIP":geo?"INTER_GEO":ciip?"INTER_CIIP":mmvd?"INTER_MMVD":affine?(merge?"INTER_AFFINE_MERGE":"INTER_AFFINE_AMVP"):merge?"INTER_MERGE":"INTER_AMVP";
    counts.prediction(mode,detail,intra?0:cu->interDir(),int(cu->lwidth()),int(cu->lheight()),{{affine,mmvd,geo,ciip,mip,isp}},area);
    for(const TransformUnit* tu=&cu->firstTU;tu;tu=tu==cu->lastTU?nullptr:tu->next) {
      for(unsigned c=0;c<3;++c) {
        const auto& block=tu->blocks[c];if(!block.valid())continue;
        const int w=std::max(0,std::min(int(block.width),width-int(block.x))),h=std::max(0,std::min(int(block.height),height-int(block.y)));
        if(!w||!h)continue;
        counts.component(c,intra,cu->intraDir[c==0?0:1],(c==0?cu->bdpcmMode():cu->bdpcmModeChroma()),TU::getCbf(*tu,ComponentID(c)),tu->mtsIdx(c)==MTS_SKIP,tu->jointCbCr!=0,uint64_t(w)*h);
      }
    }
  }
  const auto* slice=pic.slices[0];counts.write(slice->getPOC(),slice->isIntra()?"I":slice->isInterB()?"B":"P",uint64_t(width)*height);
}

} }
