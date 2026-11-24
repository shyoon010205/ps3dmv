#pragma once
#include "vvc_validation_common.hpp"
#include "vvc_area_report.hpp"
namespace vvcstudy {
using namespace hmvalidation;
inline json settings(const std::string& text) {
  json out=json::object();std::istringstream in(text);std::string line;
  auto trim=[](std::string v){auto a=v.find_first_not_of(" \t\r\n"),b=v.find_last_not_of(" \t\r\n");return a==std::string::npos?std::string():v.substr(a,b-a+1);};
  while(std::getline(in,line)){line=line.substr(0,line.find('#'));auto p=line.find(':');if(p!=std::string::npos)out[trim(line.substr(0,p))]=trim(line.substr(p+1));}return out;
}
inline void validateOverlay(const json& values) {
  auto get=[&](const char* k){check(values.contains(k),"Missing codec setting: "+std::string(k));const auto& v=values.at(k);return v.is_string()?v.get<std::string>():v.dump();};
  check(get("Tiles")=="1x1","PS3DMVP requires one tile");
  check(get("DualITree")=="0","PS3DMVP requires DualITree=0 in Baseline and Ours cfg");
  for(auto k:{"WaveFrontSynchro","IFP","MTProfile","SAO","ALF","CCALF","MCTF","BIM","PerceptQPA","EncDbOpt","IBC","JointCbCr"})check(get(k)=="0","Expected disabled codec setting: "+std::string(k));
  for(auto k:{"Threads","LoopFilterDisable","TransformSkip","ChromaTS","SEIDecodedPictureHash"})check(get(k)=="1","Expected codec setting 1: "+std::string(k));
  check(get("InputChromaFormat")=="444"&&get("ChromaFormatIDC")=="444"&&get("InputBitDepth")=="8"&&get("InputBitDepthC")=="8","Expected Real/Imag/zero 8-bit 444");
  check(get("GOPSize")=="16"&&get("IntraPeriod")=="32","Expected proven VVenC RA GOP16 / IntraPeriod32 config");
  // With CTU32, sps_max_luma_transform_size_64_flag is absent and VVdeC
  // infers log2MaxTbSize=5. Leaving VVenC's default of 6 skips the required
  // 4:4:4 sps_act_enabled_flag and misaligns the rest of the SPS.
  if(values.contains("CTUSize")&&get("CTUSize")=="32")
    check(values.contains("Log2MaxTbSize")&&get("Log2MaxTbSize")=="5",
          "CTUSize=32 requires explicit Log2MaxTbSize=5; the default 6 misaligns the 4:4:4 SPS header");
}
inline void validatePs(const json& ps) {
  check(ps.at("full_search").get<bool>(),"PS only supports Full Search in this experiment");
  check(ps.at("search_range").get<int>()>=0&&ps.at("search_range").get<int>()<=256,"PS search range must be 0..256");
  check(ps.at("theta_divisions").get<int>()>=1&&ps.at("theta_divisions").get<int>()<=1024,"PS theta divisions must be 1..1024");
  check(ps.at("rd_candidates").get<int>()>=1&&ps.at("rd_candidates").get<int>()<=16,"PS RD candidates must be 1..16");
}
inline void psArgs(std::vector<std::string>& args,bool enabled,const fs::path& dir,const json& ps,bool encode) {
  args.push_back(std::string("--PS3DMVP=")+(enabled?"1":"0"));
  if(!enabled)return;args.push_back("--PSBitstream="+(dir/"prediction.ps").u8string());
  if(encode){args.push_back("--PSStats="+(dir/"ps_stats.csv").u8string());
    args.push_back("--PS3DMVPThetaDivisions="+ps.at("theta_divisions").dump());
    args.push_back("--PS3DMVPSearchRange="+ps.at("search_range").dump());
    args.push_back("--PS3DMVPCandidates="+ps.at("rd_candidates").dump());args.push_back("--PS3DMVPFullSearch=1");
    if(ps.contains("optimized_search"))args.push_back(std::string("--PS3DMVPOptimizedSearch=")+(ps.at("optimized_search").get<bool>()?"1":"0"));}
}
inline void validateFastGeometry(const json& areas,const json& overrides) {
  if(!overrides.contains("MaxCodingCUSize"))return;
  const auto integer=[&](const char* key){const auto& value=overrides.at(key);return value.is_string()?std::stoi(value.get<std::string>()):value.get<int>();};
  const int maximum=integer("MaxCodingCUSize"),minimum=1<<integer("Log2MinCodingBlockSize");
  if(!maximum)return;
  for(const auto& shape:areas.at("scopes").at("all_frames").at("views").at("partition_shape").at("shared").items()) {
    if(shape.value().get<uint64_t>()==0)continue;
    const auto split=shape.key().find('x');check(split!=std::string::npos,"Invalid CU shape");
    const int width=std::stoi(shape.key().substr(0,split)),height=std::stoi(shape.key().substr(split+1));
    check(width>=minimum&&height>=minimum&&width<=maximum&&height<=maximum,"Final CU violates configured minimum/maximum dimensions: "+shape.key());
  }
}
inline std::vector<std::string> decoderArgs(const fs::path& bitstream,const fs::path& output) {
  return {"-b",bitstream.u8string(),"-o",output.u8string(),"--threads","1","--simd","0","--errHandling","0","-dph","1","-v","6"};
}
inline json pictureLog(const fs::path& path,bool decoder,unsigned frames) {
  std::ifstream in(path);check(bool(in),"Missing VVC picture log");std::string line;json rows=json::array();std::set<int> pocs;int current=-1;
  while(std::getline(in,line)) {
    const auto p=line.find("POC ");
    if(p!=std::string::npos){std::istringstream row(line.substr(p+4));int poc=-1;check(bool(row>>poc)&&poc>=0&&poc<int(frames)&&pocs.insert(poc).second,"Duplicate/out-of-range VVC POC");
      json item={{"poc",poc},{"md5_ok",false}};const auto q=line.find(" QP ");if(q!=std::string::npos)item["actual_qp"]=std::stoi(line.substr(q+4));rows.push_back(item);current=int(rows.size())-1;}
    if(decoder&&line.find("[MD5:")!=std::string::npos){check(current>=0&&!rows[current].at("md5_ok").get<bool>(),"Missing/duplicate VVC hash POC");check(line.find("(OK)")!=std::string::npos,"VVC decoded picture MD5 mismatch");rows[current]["md5_ok"]=true;}
  }
  check(in.eof()&&rows.size()==frames,"VVC picture count differs from requested frames");
  if(decoder)for(const auto& row:rows)check(row.at("md5_ok").get<bool>(),"VVC picture MD5 missing");return rows;
}
inline json unpack(const fs::path& y4m,const fs::path& raw,unsigned width,unsigned height,unsigned frames) {
  vvcvalidation::Condition c{int(width),int(height),int(frames),8,0,30};vvcvalidation::Frames input(y4m,c,true);
  std::ofstream output(raw,std::ios::binary);check(bool(output),"Cannot create decoded raw file");
  for(unsigned f=0;f<frames;++f){auto bytes=input.read();output.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));}
  input.finish();output.close();check(bool(output),"Decoded raw write failed");return input.header;
}
inline json psStatistics(const fs::path& path,unsigned frames,uint64_t bytes) {
  std::ifstream in(path);check(bool(in),"Missing PS statistics");std::string line;std::getline(in,line);
  check(line=="decode_order,poc,entries,ps_cus,payload_bits,packet_bits,cumulative_file_bits","Unexpected PS CSV schema");
  json rows=json::array();uint64_t entries=0,used=0,payload=0,last=0,packets=0;std::set<unsigned> pocs;
  while(std::getline(in,line)){if(line.empty())continue;std::istringstream s(line);std::array<uint64_t,7>a{};std::string t;for(auto& n:a){check(bool(std::getline(s,t,','))&&!t.empty()&&t.find_first_not_of("0123456789")==std::string::npos,"Bad PS statistics");n=std::stoull(t);}check(a[0]==rows.size()&&a[1]<frames&&pocs.insert(unsigned(a[1])).second&&a[3]<=a[2]&&a[4]>=a[2]&&a[5]%8==0&&a[5]>=(a[4]+7)/8*8,"Inconsistent PS statistics");if(!rows.empty())check(a[6]==last+a[5],"PS cumulative size differs");last=a[6];entries+=a[2];used+=a[3];payload+=a[4];packets+=a[5];rows.push_back({{"poc",a[1]},{"eligible_leaf_cus",a[2]},{"ps_cus",a[3]},{"payload_bits",a[4]},{"packet_bits",a[5]}});}
  check(rows.size()==frames&&last==bytes*8&&packets<=last,"Incomplete PS statistics");return {{"pictures",rows},{"eligible_leaf_cus",entries},{"ps_cus",used},{"payload_bits",payload},{"framing_bits",bytes*8-payload},{"total_bits",bytes*8}};
}
struct ValidationInputs{json trial,evidence;fs::path trialPath;};
inline ValidationInputs validationInputs(const fs::path& root,const json& cfg,const fs::path& encoder,const fs::path& decoder) {
  const auto path=study::resolve(root,cfg.at("validation").at("result"));
  check(fs::is_regular_file(path),"17-frame VVC validation pending. Run the validation configured for this study first; no full experiment started.");
  const auto r=holo::read_json(path);
  check(r.at("implementation")=="vvc_ps_direct_validation_v1"&&r.at("status")=="PASS"&&r.at("full_17_picture_roundtrip_passed").get<bool>(),"17-frame VVC validation must PASS before the full experiment");
  check(r.at("config").at("frames")==17&&r.at("config").at("gop_size")==16&&r.at("config").at("ps")==cfg.at("ps"),"Passing validation used different frames/PS settings");
  check(fileInfo(encoder).at("sha256")==r.at("executables").at("encoder").at("sha256")&&fileInfo(decoder).at("sha256")==r.at("executables").at("decoder").at("sha256"),"VVC executable changed after validation; rerun validation");
  check(sha256(study::resolve(root,cfg.at("anchor_cfg")))==r.at("base_cfg").at("sha256"),"RA anchor cfg changed after validation");
  auto overlay=r.at("overlay_settings");for(const auto& item:cfg.at("codec_overrides").items()){const auto v=item.value().is_string()?item.value().get<std::string>():item.value().dump();check(overlay.contains(item.key())&&overlay.at(item.key())==v,"Codec override changed after validation: "+item.key());}
  check(overlay.size()==cfg.at("codec_overrides").size(),"Codec overlay keys differ from validation");
  for(const auto* method:{"off","on"}) {
    check(r.at(method).at("status")=="PASS","Off/On roundtrip must PASS");
    validateFastGeometry(r.at(method).at("area"),cfg.at("codec_overrides"));
  }
  if(cfg.at("validation").value("require_optimization_equivalence",false)) {
    check(r.at("optimization_equivalence_passed").get<bool>()&&r.at("reference_on").at("status")=="PASS","Optimized/reference PS search validation must PASS");
    for(const auto& pair:r.at("optimization_comparisons"))check(pair.at("identical").get<bool>(),"Optimized/reference PS output differs");
    validateFastGeometry(r.at("reference_on").at("area"),cfg.at("codec_overrides"));
  }
  return {r,{{"result",fileInfo(path)},{"frames",17},{"off_equivalence",true},{"on_roundtrip",true},{"ps_exercised",true},{"dual_tree",false}},path};
}
}
