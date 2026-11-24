#pragma once
#include "../hm/hm_validation_common.hpp"

#include <set>

namespace vvcarea {
using namespace hmvalidation;
inline json stablePictures(json pictures) {
  for(auto& frame:pictures) {
    auto& direction=frame["views"]["inter_direction"]["shared"];
    const uint64_t l0=direction.at("L0").get<uint64_t>();
    const uint64_t l1=direction.at("L1").get<uint64_t>();
    const uint64_t bi=direction.at("BI").get<uint64_t>();
    const uint64_t notInter=direction.at("NOT_INTER").get<uint64_t>();
    check(l0+l1+bi+notInter==frame.at("area_pixels").get<uint64_t>(),"Invalid inter-direction coverage");
    // VVenC's retained encoder CU can redistribute equivalent merge areas
    // between L0/L1/BI before the anchor syntax is written. VVdeC is the
    // authoritative source for the detailed split. Cross-check the stable
    // invariant (Inter versus Not-Inter) and every other final mode exactly.
    direction={{"INTER",l0+l1+bi},{"NOT_INTER",notInter}};
  }
  return pictures;
}
inline bool stablePicturesEqual(const json& encoder,const json& decoder) {
  return stablePictures(encoder)==stablePictures(decoder);
}
inline uint64_t natural(const std::string& s) {
  check(!s.empty()&&s.find_first_not_of("0123456789")==std::string::npos,"Invalid area count");return std::stoull(s);
}
inline json read(const fs::path& path,unsigned frames,unsigned width,unsigned height,const std::vector<unsigned>& expectedPocs={}) {
  const std::set<unsigned> allowed(expectedPocs.begin(),expectedPocs.end());
  check(expectedPocs.empty()||(allowed.size()==frames&&expectedPocs.size()==frames),"Invalid area POC subset");
  std::ifstream in(path);check(bool(in),"Missing area CSV");std::string line;
  check(bool(std::getline(in,line))&&line=="poc,slice_type,view,component,module,area_pixels,denominator_pixels","Invalid area CSV schema");
  std::map<unsigned,json> pictures;
  while(std::getline(in,line)) {
    if(line.empty())continue;std::istringstream row(line);std::array<std::string,7> cells;
    for(auto& c:cells)check(bool(std::getline(row,c,','))&&!c.empty(),"Short area CSV row");std::string extra;check(!std::getline(row,extra,','),"Extra area CSV field");
    const auto poc=natural(cells[0]),area=natural(cells[5]),denominator=natural(cells[6]);
    check((expectedPocs.empty()?poc<frames:allowed.count(unsigned(poc))!=0)&&denominator==uint64_t(width)*height&&area<=denominator,"Invalid frame/area dimensions");
    check(cells[1]=="I"||cells[1]=="P"||cells[1]=="B","Invalid area slice type");
    auto& frame=pictures[unsigned(poc)];
    if(frame.is_null())frame={{"poc",poc},{"slice_type",cells[1]},{"area_pixels",denominator},{"views",json::object()}};
    check(frame.at("slice_type")==cells[1],"Inconsistent slice type");
    auto& counts=frame["views"][cells[2]][cells[3]];check(!counts.contains(cells[4]),"Duplicate area module row");counts[cells[4]]=area;
  }
  check(in.eof()&&pictures.size()==frames,"Incomplete area frame coverage");
  json scopes={{"all_frames",{{"area_pixels",0},{"views",json::object()}}},{"inter_frames",{{"area_pixels",0},{"views",json::object()}}}};
  json frameList=json::array();
  const std::map<std::string,std::vector<std::string>> expected={
    {"prediction_detail",{"shared"}},{"bdpcm",{"Real","Imag","Zero"}},{"tool_affine",{"shared"}},{"tool_mmvd",{"shared"}},{"tool_geo",{"shared"}},{"tool_ciip",{"shared"}},{"tool_mip",{"shared"}},{"tool_isp",{"shared"}},
    {"prediction",{"shared"}},{"prediction_family",{"shared"}},{"inter_direction",{"shared"}},{"partition_shape",{"shared"}},
    {"intra_direction",{"Real","Imag","Zero"}},{"residual_coding",{"Real","Imag","Zero"}},{"rdpcm",{"Real","Imag","Zero"}},{"cross_component_prediction",{"Imag","Zero"}}};
  for(const auto& entry:pictures) {
    const auto& frame=entry.second;const auto denominator=frame.at("area_pixels").get<uint64_t>();
    check(frame.at("views").size()==expected.size(),"Unexpected area views");
    for(const auto& view:expected) {
      check(frame.at("views").at(view.first).size()==view.second.size(),"Unexpected area components");
      for(const auto& component:view.second) {
      const auto& counts=frame.at("views").at(view.first).at(component);uint64_t sum=0;
      for(const auto& value:counts)sum+=value.get<uint64_t>();check(sum==denominator,"Area modules do not sum to frame area");
      }
    }
    const auto& p=frame.at("views").at("prediction").at("shared");const auto& family=frame.at("views").at("prediction_family").at("shared");
    check(family.at("INTER").get<uint64_t>()==p.at("INTER_AMVP").get<uint64_t>()+p.at("INTER_MERGE").get<uint64_t>()+p.at("INTER_SKIP").get<uint64_t>(),"Native Inter sub-modes do not sum to total Inter area");
    for(const auto* mode:{"PS","INTRA","PCM"})check(family.at(mode)==p.at(mode),"Inconsistent prediction families");
    if(frame.at("slice_type")=="I")check(frame.at("views").at("prediction").at("shared").at("PS")==0,"PS unexpectedly active in I picture");
    for(const auto* scope:{"all_frames","inter_frames"}) {
      if(std::string(scope)=="inter_frames"&&frame.at("slice_type")=="I")continue;
      auto& out=scopes[scope];out["area_pixels"]=out.at("area_pixels").get<uint64_t>()+denominator;
      for(const auto& view:frame.at("views").items())for(const auto& component:view.value().items())for(const auto& module:component.value().items()) {
        auto& target=out["views"][view.key()][component.key()][module.key()];target=(target.is_null()?0:target.get<uint64_t>())+module.value().get<uint64_t>();
      }
    }
    frameList.push_back(frame);
  }
  const auto selected=scopes.at("all_frames").at("views").at("prediction").at("shared").at("PS").get<uint64_t>();
  const auto all=scopes.at("all_frames").at("area_pixels").get<uint64_t>(),eligible=scopes.at("inter_frames").at("area_pixels").get<uint64_t>();
  return {{"schema_version",1},{"scopes",scopes},{"pictures",frameList},{"ps_selected_area_pixels",selected},
    {"ps_eligible_area_pixels",eligible},{"all_frame_area_pixels",all},{"ps_area_ratio",eligible?json(double(selected)/double(eligible)):json()},
    {"ps_area_ratio_all_frames",all?json(double(selected)/double(all)):json()},
    {"primary_denominator","All spatial pixels in P/B pictures, including native Intra CUs; excludes I pictures"},
    {"counting","Final CU rectangles and component TU rectangles clipped to valid picture area, one spatial sample per complex pixel; Real and Imag are not added twice"},
    {"tool_notes",{{"BDPCM","VVC BDPCM is reported separately (disabled by the default cfg)"},{"RDPCM","Not applicable to VVC; BDPCM is a distinct view"},{"NO_CODED_COEFF","CBF=0; cross-component residual prediction is measured separately"},{"Zero_component","Reported separately; excluded from Real/Imag pooling"}}}};
}
inline fs::path verified(const json& info) {
  const auto path=fs::u8path(info.at("path").get<std::string>());
  check(fs::is_regular_file(path)&&fs::file_size(path)==info.at("bytes").get<uint64_t>()&&sha256(path)==info.at("sha256"),"Saved stream has changed: "+path.u8string());return path;
}
}
