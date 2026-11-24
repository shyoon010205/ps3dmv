#include "PS3DMVP.h"
#include "../../../../cpp/ps3dmvp/ModeArea.h"
#include "CodingStructure.h"
#include "Picture.h"
#include "UnitTools.h"
#include "../../../../cpp/ps3dmvp/PSCore.cpp"
namespace vvenc { namespace ps {
namespace {
ps3dmvp::Plane plane(const CPelBuf& b){static_assert(sizeof(Pel)==sizeof(int16_t),"PS requires 16-bit Pel storage");return{b.buf,b.stride,int(b.width),int(b.height)};}
}
uint64_t sideBits(const CodingStructure& cs){uint64_t bits=0;for(const auto* cu:cs.cus)if(cu->chType==CH_L)bits+=ps3dmvp::syntaxBits(context(*cu),cu->psMotion);return bits;}
uint64_t distortion(const CodingStructure& cs) {
  uint64_t result=0;
  for(int c=0;c<2;++c)if(cs.area.blocks[c].valid()) {
    const auto a=cs.getOrgBuf(ComponentID(c)),b=cs.getRecoBuf(ComponentID(c));
    const int width=std::max(0,std::min(int(a.width),int(cs.pcv->lumaWidth)-cs.area.blocks[c].x));
    const int height=std::max(0,std::min(int(a.height),int(cs.pcv->lumaHeight)-cs.area.blocks[c].y));
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){const int d=a.buf[y*a.stride+x]-b.buf[y*b.stride+x];result+=uint64_t(d*d);}
  }
  return result;
}
ps3dmvp::Context context(const CodingUnit& cu){
  ps3dmvp::Context c;const auto& s=*cu.slice;const auto& cs=*cu.cs;
  c.block={cu.lx(),cu.ly(),int(cu.lwidth()),int(cu.lheight())};c.poc=s.poc;c.width=cs.pcv->lumaWidth;c.height=cs.pcv->lumaHeight;c.ctuSize=cs.pcv->maxCUSize;
  c.intra=s.isIntra();c.bi=s.isInterB();c.tmvp=cs.picHeader->enableTMVP;c.checkLDC=s.checkLDC;c.colFromL0=s.colFromL0Flag;c.colRef=int(s.colRefIdx);c.history=cs.motionLut.ps;
  for(int l=0;l<2;++l)for(int i=0;i<s.numRefIdx[l];++i){const auto* p=s.getRefPic(RefPicList(l),i);ps3dmvp::Ref ref;ref.poc=p->poc;ref.longTerm=s.isUsedAsLongTerm[l][i];ref.real=plane(p->getRecoBuf(COMP_Y));ref.imag=plane(p->getRecoBuf(COMP_Cb));c.refs[l].push_back(ref);}
  c.spatial=[&cu](int x,int y){const auto* n=cu.cs->getCURestricted(Position(x,y),cu,CH_L);return n&&n!=&cu?n->psMotion:ps3dmvp::Motion();};return c;
}
void predict(const CodingUnit& cu,UnitBuf<short>& out){ps3dmvp::predict(context(cu),cu.psMotion,{{out.Y().buf,out.Cb().buf,out.Cr().buf}},{{out.Y().stride,out.Cb().stride,out.Cr().stride}});}
void apply(CodingUnit& cu,const ps3dmvp::Motion& m){cu.psMotion=m;if(!m.used)return;cu.predMode=MODE_INTER;cu.skip=false;cu.mergeFlag=false;cu.mmvdMergeFlag=false;cu.mmvdSkip=false;cu.affine=false;cu.geo=false;cu.ciip=false;cu.imv=IMV_FPEL;cu.BcwIdx=BCW_DEFAULT;cu.smvdMode=0;cu.interDir=m.direction;
  for(int l=0;l<2;++l){const bool use=(m.direction&(1<<l))!=0;cu.refIdx[l]=use?m.ref[l]:-1;cu.mv[l][0]=Mv(use?16*m.mv[l].x:0,use?16*m.mv[l].y:0);cu.mvd[l][0]=Mv(0,0);cu.mvpIdx[l]=0;cu.mvpNum[l]=2;}
  CU::spanMotionInfo(cu);
}
void validate(const CodingStructure& cs){using ps3dmvp::require;require(cs.pcv->chrFormat==VVENC_CHROMA_444&&cs.sps->bitDepths[CH_L]==8&&cs.sps->bitDepths[CH_C]==8,"VVC PS requires 8-bit 444");require(cs.pps->getNumTiles()==1&&cs.pps->numSlicesInPic==1,"VVC PS requires one tile and one slice");require(!cs.sps->dualITree,"PS3DMVP=1 requires DualITree=0; Dual tree cannot share complex Real/Imag prediction");require(!cs.sps->entropyCodingSyncEnabled,"VVC PS requires WPP off");require(!cs.sps->saoEnabled&&!cs.sps->alfEnabled&&!cs.sps->IBC,"VVC PS validation requires SAO/ALF/IBC off");}
void beginCtu(CodingStructure& cs,int x,int y){if(!ps3dmvp::enabled())return;validate(cs);if(x==0&&y==0)ps3dmvp::resetMap(cs.slice->poc,cs.pcv->lumaWidth,cs.pcv->lumaHeight);if(x==0)cs.motionLutBuf[cs.pps->getTileLineId(x,y)].ps.clear();}
void commitCtu(CodingStructure& cs,const UnitArea& area){if(!ps3dmvp::enabled())return;for(const auto* cu:cs.cus)if(cu->chType==CH_L&&area.Y().contains(cu->Y()))ps3dmvp::commitMap(cs.slice->poc,{cu->lx(),cu->ly(),int(cu->lwidth()),int(cu->lheight())},cu->psMotion);}
void writePicture(Picture& pic){if(!ps3dmvp::enabled())return;ps3dmvp::require(pic.slices.size()==1,"VVC PS requires one slice");ps3dmvp::Bits bits;unsigned entries=0,used=0;int row=-1;std::vector<ps3dmvp::Motion> history;ps3dmvp::Context frame;
  // Picture CUs are appended in CTU task completion order, which can follow
  // a diagonal even with one worker and WPP disabled. The decoder consumes
  // CTUs in raster order. Preserve the selected leaf order within each CTU.
  std::vector<const CodingUnit*> leaves;
  for(const auto* cu:pic.cs->cus)if(cu->chType==CH_L)leaves.push_back(cu);
  const auto ctuAddress=[&](const CodingUnit* cu){return (cu->ly()/pic.cs->pcv->maxCUSize)*pic.cs->pcv->widthInCtus+cu->lx()/pic.cs->pcv->maxCUSize;};
  std::stable_sort(leaves.begin(),leaves.end(),[&](const CodingUnit* a,const CodingUnit* b){return ctuAddress(a)<ctuAddress(b);});
  for(const auto* cu:leaves){auto c=context(*cu);frame=c;const int nextRow=c.block.y/c.ctuSize;if(nextRow!=row){history.clear();row=nextRow;}c.history=history;
    if(!c.intra){const auto& m=cu->psMotion;if(m.used)for(int l=0;l<2;++l)if(m.direction&(1<<l)){
      auto v=ps3dmvp::predictors(c,l,m.ref[l]);
      const bool matches=v[m.mvp[l]].x+m.mvd[l].x==m.mv[l].x&&v[m.mvp[l]].y+m.mvd[l].y==m.mv[l].y;
      ps3dmvp::require(matches,"RDO/final PS predictor differs");
    }ps3dmvp::encodeMotion(bits,c,m);++entries;if(m.used)++used;ps3dmvp::pushHistory(history,m);}
  }ps3dmvp::writePicture(frame,bits,entries,used);
}

void writeAreas(const Picture& pic) {
  if(ps3dmvp::modeStatsPath().empty())return;
  const auto& cs=*pic.cs;ps3dmvp::ModeArea counts;
  const int width=cs.pcv->lumaWidth,height=cs.pcv->lumaHeight;
  std::set<const CodingUnit*> visited;
  for(int y=0;y<height;y+=4)for(int x=0;x<width;x+=4) {
    const Position pos(x,y);const auto* cu=cs.getCU(pos,CH_L,TREE_D);
    ps3dmvp::require(cu!=nullptr,"missing final CU for area statistics");
    if(!visited.insert(cu).second)continue;
    const uint64_t area=uint64_t(std::min(int(cu->lwidth()),width-cu->lx()))*std::min(int(cu->lheight()),height-cu->ly());
    const bool intra=CU::isIntra(*cu),ps=cu->psMotion.used;
    ps3dmvp::require(!CU::isIBC(*cu),"area schema requires IBC off");
    const bool skip=cu->skip,merge=cu->mergeFlag,affine=cu->affine,mmvd=cu->mmvdMergeFlag,geo=cu->geo,ciip=cu->ciip,mip=cu->mipFlag,isp=cu->ispMode!=0;
    const int bdpcm=cu->bdpcmM[CH_L];
    const std::string mode=ps?"PS":intra?"INTRA":skip?"INTER_SKIP":merge?"INTER_MERGE":"INTER_AMVP";
    const std::string detail=ps?"PS":intra?(bdpcm?"INTRA_BDPCM":mip?"INTRA_MIP":isp?"INTRA_ISP":cu->intraDir[0]==0?"INTRA_PLANAR":cu->intraDir[0]==1?"INTRA_DC":"INTRA_ANGULAR"):
      skip?"INTER_SKIP":geo?"INTER_GEO":ciip?"INTER_CIIP":mmvd?"INTER_MMVD":affine?(merge?"INTER_AFFINE_MERGE":"INTER_AFFINE_AMVP"):merge?"INTER_MERGE":"INTER_AMVP";
    counts.prediction(mode,detail,intra?0:cu->interDir,int(cu->lwidth()),int(cu->lheight()),{{affine,mmvd,geo,ciip,mip,isp}},area);
    for(const TransformUnit* tu=cu->firstTU;tu;tu=tu==cu->lastTU?nullptr:tu->next) {
      for(unsigned c=0;c<3;++c) {
        const auto& block=tu->blocks[c];if(!block.valid())continue;
        const int w=std::max(0,std::min(int(block.width),width-int(block.x))),h=std::max(0,std::min(int(block.height),height-int(block.y)));
        if(!w||!h)continue;
        counts.component(c,intra,cu->intraDir[c==0?0:1],cu->bdpcmM[c==0?CH_L:CH_C],TU::getCbf(*tu,ComponentID(c)),tu->mtsIdx[c]==MTS_SKIP,tu->jointCbCr!=0,uint64_t(w)*h);
      }
    }
  }
  const auto* slice=pic.slices[0];counts.write(slice->poc,slice->isIntra()?"I":slice->isInterB()?"B":"P",uint64_t(width)*height);
}

} }
