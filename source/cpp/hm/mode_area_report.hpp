#pragma once
#include "hm_validation_common.hpp"
#include "ps_complex_io.hpp"
#include <set>

namespace hmarea {
using namespace hmvalidation;
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
    {"counting","Final minimum coding-partition grid, weighted by valid pixel area, one spatial sample per complex pixel; Real and Imag are not added twice"},
    {"tool_notes",{{"BDPCM","Not supported by this HM build; do not equate HM RDPCM with VVC BDPCM"},{"RDPCM","Reported separately from prediction and transform-skip; counts only coded residual regions"},{"NO_CODED_COEFF","CBF=0; cross-component residual prediction is measured separately"},{"Zero_component","Reported separately; excluded from Real/Imag pooling"}}}};
}
inline fs::path verified(const json& info) {
  const auto path=fs::u8path(info.at("path").get<std::string>());
  check(fs::is_regular_file(path)&&fs::file_size(path)==info.at("bytes").get<uint64_t>()&&sha256(path)==info.at("sha256"),"Saved stream has changed: "+path.u8string());return path;
}
inline void hashes(const fs::path& path,unsigned frames) {
  std::ifstream in(path);check(bool(in),"Missing audit decode log");std::string line;std::set<unsigned> pocs;
  while(std::getline(in,line))if(line.find("[MD5:")!=std::string::npos) {
    std::istringstream row(line);std::string tag;unsigned poc;check(bool(row>>tag>>poc)&&tag=="POC"&&poc<frames&&pocs.insert(poc).second&&line.find("(OK)")!=std::string::npos,"Audit decoder picture hash mismatch");
  }
  check(in.eof()&&pocs.size()==frames,"Audit decoder picture count mismatch");
}
inline json collect(const json& trial,const fs::path& trialDirectory,const fs::path& decoder,const fs::path& out) {
  const auto& condition=trial.at("condition");const unsigned frames=condition.at("frames"),width=condition.at("crop").at("width"),height=condition.at("crop").at("height");
  check(trial.at("implementation")=="hm_ps_direct_v5"&&trial.value("roundtrip_passed",false),"Area collection needs a verified direct-v5 roundtrip");
  check(!fs::exists(out),"Area output directory already exists");fs::create_directories(out);
  json result={{"status","RUNNING"},{"qp",condition.at("qp")},{"decoder",fileInfo(decoder)},{"output_directory",out.u8string()}};
  try {
    for(const auto* method:{"off","on"}) {
      const bool ps=std::string(method)=="on";
      const auto& stream=ps?trial.at("anchor_stream"):trial.at("same_qp_baseline").at("anchor");const auto anchor=verified(stream);
      const auto csv=out/(std::string(method)+"_mode_area.csv"),log=out/(std::string(method)+"_decode.log");
      std::vector<std::string> args={"--BitstreamFile="+anchor.u8string(),"--ModeStats="+csv.u8string(),"--SEIDecodedPictureHash=1","--OutputBitDepth=8","--OutputBitDepthC=8",std::string("--PS3DMVP=")+(ps?"1":"0")};
      if(ps)args.push_back("--PSBitstream="+verified(trial.at("ps_stream")).u8string());
      const auto process=runProcess(decoder,args,log,out);result[method]["process"]=process;
      check(process.at("exit_code")==0,"Area decode failed: "+std::string(method));hashes(log,frames);
      if(ps)check(psvalidation::complexHashes(log,frames)==psvalidation::complexHashes(trialDirectory/"on_encode.log",frames),"Audit decoder changed final complex reconstruction");
      result[method]["areas"]=read(csv,frames,width,height);result[method]["csv"]=fileInfo(csv);result[method]["input_stream"]=stream;result[method]["picture_hashes_passed"]=true;
      check(sha256(anchor)==stream.at("sha256"),"Input stream changed during area decode");
    }
    check(result.at("off").at("areas").at("ps_selected_area_pixels")==0,"PS Off audit contains PS area");
    const bool selected=trial.at("ps_statistics").at("ps_cus").get<uint64_t>()>0;
    check((result.at("on").at("areas").at("ps_selected_area_pixels").get<uint64_t>()>0)==selected,"PS area and selected-CU presence disagree");
    result["status"]="PASS";result["no_reencoding"]=true;result["reconstruction_files_written"]=false;
    holo::write_json(out/"result.json",result);return result;
  }catch(const std::exception& e){result["status"]="ERROR";result["error"]=e.what();holo::write_json(out/"result.json",result);throw;}
}
inline std::string number(const json& v){return v.is_null()?"":v.dump();}
inline void comparisons(const fs::path& out,const json& trials) {
  std::ostringstream csv,summary,html;
  csv<<"qp,scope,view,component,module,off_area_pixels,on_area_pixels,off_denominator_pixels,on_denominator_pixels,off_percent,on_percent,delta_percentage_points\n";
  summary<<"qp,ps_selected_area_pixels,ps_eligible_area_pixels,ps_area_ratio,ps_area_percent,ps_area_ratio_all_frames,off_bytes,on_anchor_bytes,on_ps_bytes,on_total_bytes\n";
  html<<"<!doctype html><meta charset=utf-8><title>HM mode area comparison</title><style>body{font:16px system-ui;margin:30px;max-width:1200px}table{border-collapse:collapse;margin:20px 0}td,th{padding:7px 14px;border-bottom:1px solid #ddd;text-align:right}.bar{display:flex;width:900px;height:25px;background:#eee;margin:6px 0}.bar span{display:block;overflow:hidden;font-size:11px}label{display:inline-block;margin:8px}</style><h1>PS Off / On: pixel-area shares</h1><p>Each prediction bar sums to 100%. Residual tools are separate views. HM RDPCM is reported; VVC BDPCM is not supported. Area is pooled across frames, not averaged across CUs.</p>";
  const char* modes[]={"PS","INTRA","INTER_AMVP","INTER_MERGE","INTER_SKIP","PCM"};
  const char* colors[]={"#52b788","#f4c95d","#5680c2","#7ac4cc","#aaa","#ae81ba"};
  for(unsigned i=0;i<6;++i)html<<"<label style='border-bottom:6px solid "<<colors[i]<<"'>"<<modes[i]<<"</label>";
  for(const auto& t:trials) {
    const auto& area=t.at("mode_area");const auto& on=area.at("on").at("areas");
    summary<<t.at("qp")<<','<<on.at("ps_selected_area_pixels")<<','<<on.at("ps_eligible_area_pixels")<<','<<number(on.at("ps_area_ratio"))<<',';
    if(on.at("ps_area_ratio").is_number())summary<<100*on.at("ps_area_ratio").get<double>();summary<<','<<number(on.at("ps_area_ratio_all_frames"));
    const auto& rate=t.at("rate");for(const auto* key:{"off_anchor_bytes","on_anchor_bytes","on_ps_bytes","on_combined_bytes"})summary<<','<<rate.at(key);summary<<'\n';
    html<<"<h2>QP "<<t.at("qp")<<"</h2>";
    for(const auto* scope:{"all_frames","inter_frames"}) {
      const auto& a=area.at("off").at("areas").at("scopes").at(scope);const auto& b=on.at("scopes").at(scope);
      const auto da=a.at("area_pixels").get<uint64_t>(),db=b.at("area_pixels").get<uint64_t>();check(da==db,"Off/On area scope differs");
      html<<"<h3>"<<scope<<"</h3>";
      if(!da){html<<"<p>No eligible area.</p>";continue;}
      for(const auto& view:a.at("views").items())for(const auto& component:view.value().items())for(const auto& mode:component.value().items()) {
        const auto av=mode.value().get<uint64_t>(),bv=b.at("views").at(view.key()).at(component.key()).at(mode.key()).get<uint64_t>();
        const double ap=100.0*double(av)/double(da),bp=100.0*double(bv)/double(db);
        csv<<t.at("qp")<<','<<scope<<','<<view.key()<<','<<component.key()<<','<<mode.key()<<','<<av<<','<<bv<<','<<da<<','<<db<<','<<std::setprecision(12)<<ap<<','<<bp<<','<<bp-ap<<'\n';
      }
      for(const auto* method:{"off","on"}) {
        const auto& shared=area.at(method).at("areas").at("scopes").at(scope).at("views").at("prediction").at("shared");
        html<<"<div>"<<method<<"</div><div class=bar>";
        for(unsigned i=0;i<6;++i){const double p=100.0*shared.at(modes[i]).get<double>()/double(da);html<<"<span title='"<<modes[i]<<" "<<p<<"%' style='width:"<<p<<"%;background:"<<colors[i]<<"'>"<<modes[i]<<"</span>";}html<<"</div>";
      }
      html<<"<table><tr><th>Prediction</th><th>Off %</th><th>On %</th><th>Change (pp)</th></tr>";
      for(const auto* mode:modes){double av=100.0*a.at("views").at("prediction").at("shared").at(mode).get<double>()/double(da),bv=100.0*b.at("views").at("prediction").at("shared").at(mode).get<double>()/double(db);html<<"<tr><td>"<<mode<<"</td><td>"<<std::fixed<<std::setprecision(4)<<av<<"</td><td>"<<bv<<"</td><td>"<<bv-av<<"</td></tr>";}
      const double ai=100.0*a.at("views").at("prediction_family").at("shared").at("INTER").get<double>()/double(da),bi=100.0*b.at("views").at("prediction_family").at("shared").at("INTER").get<double>()/double(db);
      html<<"<tr><th>Native Inter subtotal</th><td>"<<ai<<"</td><td>"<<bi<<"</td><td>"<<bi-ai<<"</td></tr></table>";
    }
  }
  html<<"<p>Complete direction, partition, transform-skip, RDPCM and cross-component prediction results: <a href='mode_area_comparison.csv'>mode_area_comparison.csv</a>. Zero is the constant third codec plane and is reported separately. Primary PS selection = PS area / P-B frame area; all_frames also includes I pictures. Percent changes are percentage points, not relative percent changes.</p>";
  writeText(out/"summary_area.csv",summary.str());writeText(out/"mode_area_comparison.csv",csv.str());writeText(out/"mode_area_comparison.html",html.str());
}
}
