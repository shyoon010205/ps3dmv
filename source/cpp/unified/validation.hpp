#pragma once
#include "engine.hpp"
namespace unified {
struct ValidationStopped:std::runtime_error{ValidationStopped():std::runtime_error("Validation stopped after active codec finished") {}};
inline json validationGroup(const Engine& e,const json& experimentGroup){
 const std::string codec=experimentGroup.at("codec");
 return {{"id",codec},{"codec",codec},{"dataset",e.cfg.at("validation").value("dataset",std::string("dynamic"))}};
}
inline std::vector<json> validationGroups(const Engine& e,const std::vector<json>& groups){
 std::set<std::string> seen;std::vector<json> unique;
 for(const auto& g:groups)if(seen.insert(g.at("codec").get<std::string>()).second)unique.push_back(validationGroup(e,g));
 return unique;
}
inline void validateGroup(Engine& e,const json& experimentGroup){
 const auto group=validationGroup(e,experimentGroup);
 const auto parent=e.out/"validation"/group.at("id").get<std::string>();fs::create_directories(parent);
 const auto run=parent/timestamp();check(fs::create_directory(run),"Validation directory exists");
 const unsigned frames=e.cfg.at("validation").at("frames"),channel=e.cfg.at("validation").at("channel");
 json report={{"status","RUNNING"},{"scope","codec"},{"stage","preparing_input"},{"suite_key",e.identity.at("key")},{"group",group},{"frames",frames},{"physical_channels",1},{"channel",channel},{"validation_config",e.cfg.at("validation")},{"attempt_limit",1},{"cases",json::object()},{"directory",run.u8string()}};
 auto save=[&](){atomicJson(run/"result.json",report);atomicJson(parent/"latest.json",report);};save();
 auto stop=[&](){if(e.stopped())throw ValidationStopped();};
 try{
   stop();
   auto v=e.cfg.at("validation");v["first_frame"]=0;v["input_kind"]="hologram";v["schema_version"]=1;v["source_directory"]=(e.root/"data/holo"/group.at("dataset").get<std::string>()/v.at("sequence").get<std::string>()).u8string();
   const unsigned w=v.at("crop").at("width"),h=v.at("crop").at("height");
   vvcvalidation::Condition condition{int(w),int(h),int(frames),8,v.at("qp"),e.cfg.at("frame_rate")};
   report["input"]=vvcvalidation::prepareInput(e.root,v,run/"input.yuv",condition);save();
   json mapping={{"source_id",holo::fingerprint(report.at("input").dump())},{"inputs",json::array()}};for(unsigned c=0;c<3;++c)mapping["inputs"].push_back(report.at("input").at("input"));
   auto perform=[&](const std::string& name,json method,bool reference,bool optimized){
     stop();report["stage"]=name;report["active_progress"]=(run/name/channels[channel]/"progress.json").u8string();save();std::cout<<group.at("id")<<" validation "<<name<<std::endl;
     json spec={{"suite_key",e.identity.at("key")},{"group",group},{"sequence",v.at("sequence")},{"method",method},{"qp",v.at("qp")},{"width",w},{"height",h},{"frames",frames},{"mapping",mapping},{"directory",(run/name).u8string()},{"key",holo::fingerprint(e.identity.dump()+name+mapping.dump())}};
     // No retry loop is used for validation. A codec or comparison failure stops it.
     e.encodeOne(spec,channel,reference,optimized,true);stop();auto dec=e.decodeOne(spec,channel);report["cases"][name]=dec;save();return dec;
   };
   auto equal=[&](const std::string& a,const std::string& b,bool ps){
     const auto& x=report.at("cases").at(a);const auto& y=report.at("cases").at(b);
     for(const auto* field:{"anchor","encoder_reconstruction","raw"}){
       check(x.at(field).at("sha256")==y.at(field).at("sha256")&&x.at(field).at("bytes")==y.at(field).at("bytes"),"Validation equivalence differs: "+a+" / "+b+" / "+field);
     }
     if(ps)check(x.at("ps").at("sha256")==y.at("ps").at("sha256")&&x.at("mode_area")==y.at("mode_area"),"PS stream or final area differs under optimization");
     report["comparisons"][a+"__"+b]={{"anchor_identical",true},{"encoder_reconstruction_identical",true},{"independent_decoder_identical",true},{"ps_stream_compared",ps}};save();
   };
   // Engine requires byte-for-byte equal native profile objects. The same Off
   // input/profile/binaries need only one comparison for both method profiles.
   auto method=e.methods.at("baseline");
   perform("baseline_reference",method,true,true);perform("baseline",method,false,true);equal("baseline_reference","baseline",false);
   report["baseline_profiles_verified"]={"baseline","ps3dmvp"};save();
   uint64_t selected=0;
   for(const auto& name:{"ps3dmvp","ps3dmvp_theta"}){
     auto r=perform(name,e.methods.at(name),false,true);selected+=r.at("mode_area").at("ps_selected_area_pixels").get<uint64_t>();
     report["coverage"][name]={{"ps_selected_area_pixels",r.at("mode_area").at("ps_selected_area_pixels")},{"ps_exercised",r.at("mode_area").at("ps_selected_area_pixels").get<uint64_t>()>0}};
     if(v.at("verify_optimization").get<bool>()){perform(std::string(name)+"_reference",e.methods.at(name),false,false);equal(name,std::string(name)+"_reference",true);}
   }
   check(!v.at("require_ps_selected").get<bool>()||selected>0,"No PS selected in either ablation: change validation crop/QP to exercise positive PS decode");
   stop();report["status"]="PASS";report["stage"]="complete";report.erase("active_progress");report["optimization_equivalence_passed"]=v.at("verify_optimization");report["finished_local"]=timestamp();save();
 }catch(const ValidationStopped&){report["status"]="STOPPED";report["finished_local"]=timestamp();save();throw;
 }catch(const std::exception& err){report["status"]="FAIL";report["error"]=err.what();save();throw;}
}
inline void requireValidation(Engine& e,const json& group){
 const auto target=validationGroup(e,group);const auto path=e.out/"validation"/target.at("id").get<std::string>()/"latest.json";const auto r=readIf(path);
 check(!r.is_null()&&r.value("status","")=="PASS"&&r.value("scope","")=="codec"&&r.at("group")==target&&r.at("suite_key")==e.identity.at("key")&&r.at("frames")==e.cfg.at("validation").at("frames")&&r.value("validation_config",json())==e.cfg.at("validation"),"Current "+e.cfg.at("validation").at("frames").dump()+"-frame codec validation must pass first: "+group.at("codec").get<std::string>());
 for(const auto& f:r.at("input").at("sources"))check(verified(f),"Validation source changed");
}
}
