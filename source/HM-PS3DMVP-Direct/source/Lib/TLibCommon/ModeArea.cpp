#include "ModeArea.h"
#include "PS3DMVP.h"
#include "TComDataCU.h"
#include "TComPic.h"
#include "TComChromaFormat.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

namespace modearea {
namespace {
std::string destination;
std::ofstream stream;
using Counts=std::map<std::string,std::uint64_t>;
using Groups=std::map<std::string,std::map<std::string,Counts>>;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(std::string("ModeArea: ")+message);}
std::string direction(unsigned dir) {
  require(dir>=1&&dir<=3,"invalid inter direction");
  return dir==1?"L0":dir==2?"L1":"BI";
}
unsigned intraMode(TComDataCU* cu,unsigned part,ComponentID component) {
  const auto channel=toChannelType(component);unsigned mode=cu->getIntraDir(channel,part);
  if(isChroma(component)&&mode==DM_CHROMA_IDX) {
    const auto* sps=cu->getSlice()->getSPS();
    const unsigned partsPerMinCU=1u<<(2*(sps->getMaxTotalCUDepth()-sps->getLog2DiffMaxMinCodingBlockSize()));
    mode=cu->getIntraDir(CHANNEL_TYPE_LUMA,getChromasCorrespondingPULumaIdx(part,CHROMA_444,partsPerMinCU));
  }
  require(mode<=34,"unsupported resolved intra mode");return mode;
}
std::string intraName(unsigned mode){return mode==PLANAR_IDX?"PLANAR":mode==DC_IDX?"DC":"ANGULAR_"+std::to_string(mode);}
const char* shape(PartSize value) {
  switch(value) {
    case SIZE_2Nx2N:return "2Nx2N";case SIZE_2NxN:return "2NxN";
    case SIZE_Nx2N:return "Nx2N";case SIZE_NxN:return "NxN";
    case SIZE_2NxnU:return "2NxnU";case SIZE_2NxnD:return "2NxnD";
    case SIZE_nLx2N:return "nLx2N";case SIZE_nRx2N:return "nRx2N";
    default:throw std::runtime_error("ModeArea: invalid prediction partition shape");
  }
}
}
std::string& outputPath(){return destination;}
void writePicture(TComPic* picture) {
  if(destination.empty())return;
  const auto* sps=picture->getSlice(0)->getSPS();
  require(sps->getChromaFormatIdc()==CHROMA_444,"current Real/Imag audit requires 444");
  if(!stream.is_open()) {
    stream.open(destination,std::ios::trunc);require(bool(stream),"cannot open output CSV");
    stream<<"poc,slice_type,view,component,module,area_pixels,denominator_pixels\n";
  }
  const unsigned width=sps->getPicWidthInLumaSamples(),height=sps->getPicHeightInLumaSamples();
  const auto total=std::uint64_t(width)*height;
  Groups groups;
  for(const auto* module:{"PS","INTRA","INTER_AMVP","INTER_MERGE","INTER_SKIP","PCM"})groups["prediction"]["shared"][module]=0;
  for(const auto* module:{"PS","INTRA","INTER","PCM"})groups["prediction_family"]["shared"][module]=0;
  for(const auto* prefix:{"PS","AMVP","MERGE","SKIP"})for(const auto* suffix:{"L0","L1","BI"})groups["inter_direction"]["shared"][std::string(prefix)+"_"+suffix]=0;
  groups["inter_direction"]["shared"]["NOT_INTER"]=0;
  for(const auto* part:{"2Nx2N","2NxN","Nx2N","NxN","2NxnU","2NxnD","nLx2N","nRx2N"})groups["partition_shape"]["shared"][part]=0;
  const char* components[3]={"Real","Imag","Zero"};
  for(const auto* component:{"Imag","Zero"})for(const auto* module:{"PCM","OFF","ACTIVE"})groups["cross_component_prediction"][component][module]=0;
  for(const auto* component:components) {
    for(const auto* module:{"PCM","NO_CODED_COEFF","TRANSFORM","TRANSFORM_SKIP","TRANSQUANT_BYPASS"})groups["residual_coding"][component][module]=0;
    for(const auto* module:{"PCM","NO_CODED_COEFF","DISABLED","OFF","HORIZONTAL","VERTICAL"})groups["rdpcm"][component][module]=0;
    groups["intra_direction"][component]["NOT_INTRA"]=0;
    for(unsigned mode=0;mode<=34;++mode)groups["intra_direction"][component][intraName(mode)]=0;
  }
  std::uint64_t covered=0;
  const unsigned unitWidth=picture->getMinCUWidth(),unitHeight=picture->getMinCUHeight();
  require(unitWidth&&unitHeight,"zero partition grid size");
  // Final CU/PU/TU fields are replicated over minimum coding partitions.
  // Integrating this spatial grid handles mixed-PU CUs, NxN intra and clipped
  // picture boundaries without counting either channels or overlapping tools twice.
  for(unsigned address=0;address<picture->getNumberOfCtusInFrame();++address) {
    auto* cu=picture->getCtu(address);
    for(unsigned part=0;part<picture->getNumPartitionsInCtu();++part) {
      const unsigned x=(address%picture->getFrameWidthInCtus())*sps->getMaxCUWidth()+g_auiRasterToPelX[g_auiZscanToRaster[part]];
      const unsigned y=(address/picture->getFrameWidthInCtus())*sps->getMaxCUHeight()+g_auiRasterToPelY[g_auiZscanToRaster[part]];
      if(x>=width||y>=height)continue;
      const std::uint64_t area=std::uint64_t(std::min(unitWidth,width-x))*std::min(unitHeight,height-y);covered+=area;
      const bool ps=ps3dmvp::enabled()&&cu->getPSMotion(part).used;
      const bool pcm=cu->getIPCMFlag(part),intra=cu->isIntra(part);
      std::string predictor,inter="NOT_INTER";
      if(ps){require(cu->isInter(part)&&!pcm,"invalid direct PS type");predictor="PS";inter="PS_"+direction(cu->getPSMotion(part).direction);}
      else if(pcm)predictor="PCM";
      else if(intra)predictor="INTRA";
      else {
        require(cu->isInter(part),"unknown prediction mode");
        if(cu->isSkipped(part)){predictor="INTER_SKIP";inter="SKIP_";}
        else if(cu->getMergeFlag(part)){predictor="INTER_MERGE";inter="MERGE_";}
        else {predictor="INTER_AMVP";inter="AMVP_";}
        inter+=direction(cu->getInterDir(part));
      }
      groups["prediction"]["shared"][predictor]+=area;
      groups["prediction_family"]["shared"][predictor.compare(0,6,"INTER_")==0?"INTER":predictor]+=area;
      groups["inter_direction"]["shared"][inter]+=area;
      groups["partition_shape"]["shared"][shape(cu->getPartitionSize(part))]+=area;
      for(unsigned c=0;c<3;++c) {
        const auto component=ComponentID(c);const auto* name=components[c];
        groups["intra_direction"][name][intra&&!pcm?intraName(intraMode(cu,part,component)):"NOT_INTRA"]+=area;
        const bool coded=cu->getCbf(part,component,cu->getTransformIdx(part))!=0;
        const bool bypass=cu->getCUTransquantBypass(part),skip=cu->getTransformSkip(part,component)!=0;
        if(isChroma(component))groups["cross_component_prediction"][name][pcm?"PCM":cu->getCrossComponentPredictionAlpha(part,component)!=0?"ACTIVE":"OFF"]+=area;
        const char* residual=pcm?"PCM":!coded?"NO_CODED_COEFF":bypass?"TRANSQUANT_BYPASS":skip?"TRANSFORM_SKIP":"TRANSFORM";
        groups["residual_coding"][name][residual]+=area;
        const char* rdpcm="OFF";
        if(pcm)rdpcm="PCM";
        else if(!coded)rdpcm="NO_CODED_COEFF";
        else if(!cu->isRDPCMEnabled(part))rdpcm="DISABLED";
        else if(skip||bypass) {
          unsigned mode=RDPCM_OFF;
          if(intra){const auto d=intraMode(cu,part,component);mode=d==HOR_IDX?RDPCM_HOR:d==VER_IDX?RDPCM_VER:RDPCM_OFF;}
          else mode=cu->getExplicitRdpcmMode(component,part);
          require(mode<NUMBER_OF_RDPCM_MODES,"invalid RDPCM mode");
          rdpcm=mode==RDPCM_HOR?"HORIZONTAL":mode==RDPCM_VER?"VERTICAL":"OFF";
        }
        groups["rdpcm"][name][rdpcm]+=area;
      }
    }
  }
  require(covered==total,"picture grid area does not cover the coded image exactly once");
  const auto* slice=picture->getSlice(0);const char type=slice->isIntra()?'I':slice->isInterP()?'P':'B';
  for(const auto& view:groups)for(const auto& component:view.second) {
    std::uint64_t sum=0;for(const auto& mode:component.second)sum+=mode.second;
    require(sum==total,"module area partition does not sum to picture area");
    for(const auto& mode:component.second)stream<<picture->getPOC()<<','<<type<<','<<view.first<<','<<component.first<<','<<mode.first<<','<<mode.second<<','<<total<<'\n';
  }
  stream.flush();require(bool(stream),"area CSV write failed");
}
}
