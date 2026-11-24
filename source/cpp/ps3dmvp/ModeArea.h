#pragma once
#include "PSOptions.h"
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
namespace ps3dmvp {
struct ModeArea {
  using Key=std::tuple<std::string,std::string,std::string>;
  std::map<Key,uint64_t> counts;
  ModeArea(){
    for(const auto* m:{"PS","INTRA","INTER_AMVP","INTER_MERGE","INTER_SKIP","PCM"})counts[{"prediction","shared",m}]=0;
    for(const auto* m:{"PS","INTRA","INTER","PCM"})counts[{"prediction_family","shared",m}]=0;
    for(const auto* m:{"PS","INTRA_PLANAR","INTRA_DC","INTRA_ANGULAR","INTRA_MIP","INTRA_ISP","INTRA_BDPCM","INTER_AMVP","INTER_AFFINE_AMVP","INTER_MERGE","INTER_AFFINE_MERGE","INTER_MMVD","INTER_GEO","INTER_CIIP","INTER_SKIP"})counts[{"prediction_detail","shared",m}]=0;
    for(const auto* m:{"L0","L1","BI","NOT_INTER"})counts[{"inter_direction","shared",m}]=0;
    for(unsigned w=4;w<=128;w*=2)for(unsigned h=4;h<=128;h*=2)counts[{"partition_shape","shared",std::to_string(w)+"x"+std::to_string(h)}]=0;
    for(const auto* tool:{"affine","mmvd","geo","ciip","mip","isp"})for(const auto* m:{"ON","OFF"})counts[{std::string("tool_")+tool,"shared",m}]=0;
    for(const auto* c:{"Real","Imag","Zero"}) {
      counts[{"intra_direction",c,"NOT_INTRA"}]=0;counts[{"intra_direction",c,"CHROMA_DERIVED_OR_LM"}]=0;for(int d=0;d<=66;++d)counts[{"intra_direction",c,std::to_string(d)}]=0;
      for(const auto* m:{"OFF","HORIZONTAL","VERTICAL"})counts[{"bdpcm",c,m}]=0;
      for(const auto* m:{"NO_CODED_COEFF","TRANSFORM","TRANSFORM_SKIP"})counts[{"residual_coding",c,m}]=0;
      counts[{"rdpcm",c,"NOT_APPLICABLE_VVC"}]=0;
    }
    for(const auto* c:{"Imag","Zero"})for(const auto* m:{"OFF","JOINT_CBCR"})counts[{"cross_component_prediction",c,m}]=0;
  }
  void add(const char* view,const char* component,const std::string& module,uint64_t pixels){counts[{view,component,module}]+=pixels;}
  void prediction(const std::string& mode,const std::string& details,int direction,int width,int height,const std::array<bool,6>& tools,uint64_t pixels){
    add("prediction","shared",mode,pixels);add("prediction_family","shared",mode.rfind("INTER_",0)==0?"INTER":mode,pixels);
    add("prediction_detail","shared",details,pixels);add("inter_direction","shared",direction==1?"L0":direction==2?"L1":direction==3?"BI":"NOT_INTER",pixels);
    add("partition_shape","shared",std::to_string(width)+"x"+std::to_string(height),pixels);
    const char* names[]={"tool_affine","tool_mmvd","tool_geo","tool_ciip","tool_mip","tool_isp"};for(unsigned i=0;i<6;++i)add(names[i],"shared",tools[i]?"ON":"OFF",pixels);
  }
  void component(unsigned c,bool intra,int direction,int bdpcm,bool coded,bool skip,bool joint,uint64_t pixels){
    const char* names[]={"Real","Imag","Zero"};const auto name=names[c];
    add("intra_direction",name,intra?(direction<=66?std::to_string(direction):"CHROMA_DERIVED_OR_LM"):"NOT_INTRA",pixels);
    add("bdpcm",name,bdpcm==1?"HORIZONTAL":bdpcm==2?"VERTICAL":"OFF",pixels);
    add("residual_coding",name,!coded?"NO_CODED_COEFF":skip?"TRANSFORM_SKIP":"TRANSFORM",pixels);
    add("rdpcm",name,"NOT_APPLICABLE_VVC",pixels);
    if(c)add("cross_component_prediction",name,joint?"JOINT_CBCR":"OFF",pixels);
  }
  void write(int poc,const char* slice,uint64_t denominator)const{
    if(modeStatsPath().empty())return;
    static std::mutex mutex;std::lock_guard<std::mutex> lock(mutex);static std::set<int> written;static std::ofstream output;
    require(written.insert(poc).second,"duplicate mode area picture");
    if(!output.is_open()){output.open(modeStatsPath(),std::ios::trunc);require(bool(output),"cannot create mode area CSV");output<<"poc,slice_type,view,component,module,area_pixels,denominator_pixels\n";}
    std::map<std::pair<std::string,std::string>,uint64_t> sums;
    for(const auto& row:counts)sums[{std::get<0>(row.first),std::get<1>(row.first)}]+=row.second;
    for(const auto& sum:sums)require(sum.second==denominator,"mode area view does not cover all valid pixels");
    for(const auto& row:counts)output<<poc<<','<<slice<<','<<std::get<0>(row.first)<<','<<std::get<1>(row.first)<<','<<std::get<2>(row.first)<<','<<row.second<<','<<denominator<<'\n';
    output.flush();require(bool(output),"mode area CSV flush failed");
  }
};
}
