#pragma once
#include "evaluation.hpp"
#include "../hm/direct_study_area.hpp"
#include "../vvc444/vvc_experiment.hpp"
#include <future>
#include <set>
#include <mutex>
#include <numeric>
namespace unified {
inline fs::path jpath(const json& j){return fs::u8path(j.get<std::string>());}

inline fs::path executablePath(){wchar_t p[32768];auto n=GetModuleFileNameW(nullptr,p,32768);check(n&&n<32768,"Cannot find runner");return fs::path(p);}
inline bool safeName(const std::string& s){return !s.empty()&&s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")==std::string::npos;}
inline bool verified(const json& f){try{return fs::is_regular_file(jpath(f.at("path")))&&fs::file_size(jpath(f.at("path")))==f.at("bytes")&&sha256(jpath(f.at("path")))==f.at("sha256");}catch(...){return false;}}
inline json readIf(const fs::path& p){try{if(!fs::is_regular_file(p))return json();return holo::read_json(p);}catch(...){return json();}}
inline json quality(const json& r,const std::string& metric){
 const auto& m=r.at("metrics");
 if(metric=="reconstruction_psnr")return m.at("reconstruction").at("psnr_db");
 if(metric=="reconstruction_ssim")return m.at("reconstruction").at("ssim");
 if(metric=="reconstruction_ssim_db"){const auto s=m.at("reconstruction").at("ssim");if(s.is_number()&&s.get<double>()>=0&&s.get<double>()<1)return -10*std::log10(1-s.get<double>());return nullptr;}
 if(metric=="complex_snr")return m.at("complex").at("complex_snr_db");
 if(metric=="mapped_psnr")return m.at("mapped").at("psnr_db");
 if(metric=="log_rate")return std::log(r.at("rate").at("bits_per_rgb_hologram_pixel").get<double>());
 throw std::runtime_error("Unknown metric");
}
struct Engine {
 fs::path root,out;json cfg,identity;std::map<std::string,json> profiles,methods;
 Engine(fs::path r,json c):root(fs::weakly_canonical(r)),cfg(std::move(c)){
   check(cfg.at("schema_version")==1&&cfg.at("frames")==16&&cfg.at("first_frame")==0&&cfg.at("gop_size")==16,"This suite requires frames 0..15, GOP16");
   out=resolve(root,cfg.at("output_root"));check(within(out,root/"results")&&out!=root/"results","Output must be a named directory in results");
   check(cfg.at("width").get<unsigned>()%8==0&&cfg.at("height").get<unsigned>()%8==0,"Dimensions must be divisible by 8");
   check(cfg.at("frame_rate").get<double>()>0,"Invalid frame rate");
   for(auto key:{"encoding_slots","evaluation_slots","validation_slots"})check(cfg.at("resources").at(key).get<unsigned>()>=1&&cfg.at("resources").at(key).get<unsigned>()<=64,"Invalid parallel slots");
   const auto& validation=cfg.at("validation");
   const auto validationDataset=validation.value("dataset",std::string("dynamic"));
   check(safeName(validationDataset)&&fs::is_regular_file(root/"config"/(validationDataset+".json")),"Invalid validation dataset");
   check(validation.at("frames").get<int>()>=3&&validation.at("frames").get<int>()<=16,"Validation frames must be 3..16; full experiments remain 16 frames");
   check(validation.at("channel").get<int>()>=0&&validation.at("channel").get<int>()<3,"Validation requires one physical RGB channel");
   for(auto key:{"width","height"}){const int n=validation.at("crop").at(key);check(n>=64&&n%8==0,"Validation crop must be at least 64 and divisible by 8");}
   check(cfg.at("retry").at("maximum_attempts_per_invocation").get<unsigned>()>=1&&cfg.at("retry").at("validation_attempts")==1,"Invalid retry settings");
   check(cfg.at("ps3dmvp").at("search_range")==32&&cfg.at("ps3dmvp").at("full_search").get<bool>(),"PS range32 Full Search required");
   for(const auto& method:cfg.at("methods")){const std::string name=method.at("id");check(safeName(name)&&!methods.count(name),"Invalid method");methods[name]=method;}
   check(methods.size()==3&&methods.count("baseline")&&methods.count("ps3dmvp")&&methods.count("ps3dmvp_theta"),"Three ablation methods required");
   check(!methods.at("baseline").at("ps").get<bool>()&&methods.at("ps3dmvp").at("ps").get<bool>()&&methods.at("ps3dmvp_theta").at("ps").get<bool>(),"Invalid theta ablation");
   for(const auto& item:methods){const int divisions=item.second.at("theta_divisions");check(divisions>=1&&divisions<=1024,"theta_divisions must be 1..1024");}
   for(const auto& seq:cfg.at("sequences"))check(safeName(seq.get<std::string>()),"Invalid sequence name");
   json scientific=cfg;for(const auto* k:{"qp_lists","resources","retry","output_root","suite_id"})scientific.erase(k);
   identity={{"implementation","ps3dmvp_ra_v1"},{"runner",fileInfo(executablePath())},{"scientific_config",scientific},{"codecs",json::object()}};
   for(const auto& entry:cfg.at("codecs").items()){
     const auto& cdc=entry.value();profiles[entry.key()]=holo::read_json(resolve(root,cdc.at("profiles")));
     const auto& p=profiles.at(entry.key());
     check(p.at("ps3dmvp").at("SearchRange")==32&&p.at("ps3dmvp").at("IntegerMvOnly")==1,"Native search profiles differ from protocol");
     check(p.at("baseline")==p.at("ps3dmvp"),"Baseline and PS3DMVP must have identical native codec settings; only PS enable/theta may differ");
     if(entry.key()=="vvenc"){vvcstudy::validateOverlay(p.at("baseline"));vvcstudy::validateOverlay(p.at("ps3dmvp"));}
     auto& evidence=identity["codecs"][entry.key()];evidence["profiles"]=p;evidence["anchor"]=fileInfo(resolve(root,cdc.at("anchor_cfg")));
     for(auto k:{"encoder","decoder"})evidence[k]=fileInfo(resolve(root,cdc.at(k)));
     if(validation.at("verify_optimization").get<bool>())evidence["reference_encoder"]=fileInfo(resolve(root,cdc.at("reference_encoder")));
     for(const auto& q:cfg.at("qp_lists").items()){check(!q.value().empty(),"Empty QP list");std::set<int> seen;for(const auto& v:q.value()){int n=v;check(n>=cdc.at("qp_min")&&n<=cdc.at("qp_max")&&seen.insert(n).second,"Duplicate or illegal QP");}}
   }
   std::set<std::string> datasets;
   datasets.insert(validationDataset);
   for(const auto& group:cfg.at("groups"))datasets.insert(group.at("dataset").get<std::string>());
   for(const auto& dataset:datasets)identity["optics"][dataset]=holo::read_json(root/"config"/(dataset+".json"));
   identity["key"]=holo::fingerprint(identity.dump());
 }
 bool stopped()const{return stopRequested||fs::exists(out/"stop.request");}
 fs::path groupDir(const json& group)const{check(safeName(group.at("id")),"Unsafe group name");return out/group.at("id").get<std::string>();}
 fs::path trialDir(const json& group,const std::string& seq,const std::string& method,int qp)const{return groupDir(group)/seq/(method+"_qp"+std::to_string(qp));}
 json psFor(const json& method,bool optimized=true)const{auto p=cfg.at("ps3dmvp");p["theta_divisions"]=method.at("theta_divisions");p["optimized_search"]=optimized;return p;}
 json makeSpec(const json& group,const std::string& seq,const std::string& method,int qp,const json& mapping)const{
   const auto dir=trialDir(group,seq,method,qp);json s={{"group",group},{"sequence",seq},{"method",methods.at(method)},{"qp",qp},{"mapping",mapping},{"directory",dir.u8string()},{"width",cfg.at("width")},{"height",cfg.at("height")},{"frames",16},{"suite_key",identity.at("key")}};
   s["key"]=holo::fingerprint(identity.at("key").get<std::string>()+mapping.at("source_id").get<std::string>()+group.dump()+method+std::to_string(qp));return s;
 }
 bool complete(const json& spec)const{
   auto r=readIf(jpath(spec.at("directory"))/"result.json");
   if(r.is_null()||r.value("status","")!="COMPLETE"||r.value("key","")!=spec.at("key")||!verified(r.at("metrics_file")))return false;
   for(const auto& ch:r.at("channels")){if(!verified(ch.at("anchor")))return false;if(ch.contains("ps")&&!verified(ch.at("ps")))return false;}
   const auto p=jpath(spec.at("directory"))/"reconstruction_png/index.json";auto png=readIf(p);
   if(cfg.at("metrics").value("save_reconstruction_png",false)){
     if(png.is_null()||!png.value("complete",false)||png.at("count")!=160)return false;
     for(const auto& f:png.at("files")){auto path=p.parent_path()/f.at("file").get<std::string>();if(!fs::is_regular_file(path)||fs::file_size(path)!=f.at("bytes"))return false;}
   }return true;
 }
 void progress(const fs::path& path,json j)const{advisoryProgress([&]{j["updated_local"]=timestamp();atomicJson(path,j);});}
 std::string overlay(const json& spec)const{
   const std::string codec=spec.at("group").at("codec");const auto& profile=profiles.at(codec).at(spec.at("method").at("profile").get<std::string>());
   std::ostringstream s;for(const auto& item:profile.items())s<<item.key()<<" : "<<value(item.value())<<'\n';
   s<<"SourceWidth : "<<spec.at("width")<<"\nSourceHeight : "<<spec.at("height")<<"\nFramesToBeEncoded : "<<spec.at("frames")<<"\nFrameRate : "<<cfg.at("frame_rate")<<'\n';return s.str();
 }
 json encodeOne(const json& spec,unsigned channel,bool reference=false,bool optimized=true,bool keepRaw=false){
   const auto dir=jpath(spec.at("directory"))/channels[channel];fs::create_directories(dir);
   auto prior=readIf(dir/"encoded.json");
   if(!prior.is_null()&&prior.value("status","")=="COMPLETE"&&prior.value("key","")==spec.at("key")&&prior.value("reference",false)==reference&&prior.value("optimized",true)==optimized&&verified(prior.at("anchor"))&&(!prior.contains("ps")||verified(prior.at("ps"))))return prior;
   const auto attempt=dir/("encode_"+timestamp());check(fs::create_directory(attempt),"Encoder attempt exists");
   json r={{"status","RUNNING"},{"key",spec.at("key")},{"reference",reference},{"optimized",optimized},{"directory",attempt.u8string()}};
   const auto report=attempt/"encoded.json";auto save=[&](){atomicJson(report,r);atomicJson(dir/"encoded.json",r);};
   const unsigned frames=spec.at("frames");check(frames>=1&&frames<=16,"Invalid encode frame count");
   save();try{
     const auto codec=spec.at("group").at("codec").get<std::string>();const auto cdc=cfg.at("codecs").at(codec);const auto& method=spec.at("method");const bool ps=method.at("ps");
     const auto enc=resolve(root,cdc.at(reference?"reference_encoder":"encoder"));const auto input=jpath(spec.at("mapping").at("inputs").at(channel).at("path"));
     atomicText(attempt/"overlay.cfg",overlay(spec));fs::copy_file(resolve(root,cdc.at("anchor_cfg")),attempt/"anchor.cfg");
     std::vector<std::string> args={"-c",(attempt/"anchor.cfg").u8string(),"-c",(attempt/"overlay.cfg").u8string(),"--QP="+spec.at("qp").dump(),"--InputFile="+input.u8string(),"--BitstreamFile="+(attempt/"anchor.bin").u8string(),"--ReconFile="+(attempt/"encoder.yuv").u8string()};
     if(codec=="vvenc"){args.push_back("--Verbosity=6");args.push_back("--InternalBitDepth=8");args.push_back("--OutputBitDepth=8");args.push_back("--OutputBitDepthC=8");}
     if(!reference){vvcstudy::psArgs(args,ps,attempt,psFor(method,optimized),true);if(codec=="vvenc")args.push_back("--ModeStats="+(attempt/"encoder_area.csv").u8string());}
     const auto log=attempt/"encode.log";
     r["process"]=runChild(enc,args,log,attempt,[&](json p){p["state"]="ENCODING";p["log"]=log.u8string();progress(dir/"progress.json",p);});
     check(r.at("process").at("exit_code")==0,"Encoder failed: "+log.u8string());
     r["pictures"]=codec=="vvenc"?vvcstudy::pictureLog(log,false,frames):study::pictureLog(log,false);check(r.at("pictures").size()==frames,"Encoder picture count differs from requested frames");
     const auto expected=spec.at("width").get<uint64_t>()*spec.at("height").get<uint64_t>()*frames*3;
     r["anchor"]=fileInfo(attempt/"anchor.bin");check(r.at("anchor").at("bytes").get<uint64_t>()>0,"Empty anchor stream");
     r["encoder_reconstruction"]=fileInfo(attempt/"encoder.yuv");check(r.at("encoder_reconstruction").at("bytes")==expected,"Incomplete encoder reconstruction");
     if(ps){r["ps"]=fileInfo(attempt/"prediction.ps");r["ps_statistics"]=vvcstudy::psStatistics(attempt/"ps_stats.csv",frames,fs::file_size(attempt/"prediction.ps"));
       if(fs::is_regular_file(attempt/"ps_stats.csv.search.csv"))r["search_profile"]=fileInfo(attempt/"ps_stats.csv.search.csv");}
     r["status"]="COMPLETE";save();if(!keepRaw)removeOwned(attempt/"encoder.yuv",out);progress(dir/"progress.json",{{"state","ENCODED"}});return r;
   }catch(const std::exception& e){r["status"]="ERROR";r["error"]=e.what();save();if(!keepRaw)removeOwned(attempt/"encoder.yuv",out);progress(dir/"progress.json",r);throw;}
 }
 json decodeOne(const json& spec,unsigned channel){
   const unsigned frames=spec.at("frames");check(frames>=1&&frames<=16,"Invalid decode frame count");
   const auto dir=jpath(spec.at("directory"))/channels[channel];auto enc=holo::read_json(dir/"encoded.json");
   check(enc.at("status")=="COMPLETE"&&enc.at("key")==spec.at("key")&&verified(enc.at("anchor")),"Invalid encoded checkpoint");
   const bool ps=spec.at("method").at("ps");if(ps)check(verified(enc.at("ps")),"PS stream changed");
   auto prior=readIf(dir/"decoded.json");if(!prior.is_null()&&prior.value("status","")=="COMPLETE"&&prior.value("key","")==spec.at("key")&&verified(prior.at("raw"))&&verified(prior.at("mode_area_file"))&&prior.at("anchor")==enc.at("anchor"))return prior;
   const auto attempt=dir/("decode_"+timestamp());check(fs::create_directory(attempt),"Decoder attempt exists");
   const auto codec=spec.at("group").at("codec").get<std::string>();const auto dec=resolve(root,cfg.at("codecs").at(codec).at("decoder"));
   const auto raw=dir/"decoder.yuv",bitstream=jpath(enc.at("anchor").at("path"));
   auto args=codec=="vvenc"?vvcstudy::decoderArgs(bitstream,attempt/"decoder.y4m"):std::vector<std::string>{"--BitstreamFile="+bitstream.u8string(),"--ReconFile="+raw.u8string(),"--OutputBitDepth=8","--OutputBitDepthC=8","--SEIDecodedPictureHash=1"};
   vvcstudy::psArgs(args,ps,jpath(enc.at("directory")),psFor(spec.at("method")),false);args.push_back("--ModeStats="+(attempt/"mode_area.csv").u8string());
   auto proc=runChild(dec,args,attempt/"decode.log",attempt,[&](json p){p["state"]="DECODING";p["log"]=(attempt/"decode.log").u8string();progress(dir/"progress.json",p);});
   check(proc.at("exit_code")==0,"Decoder failed: "+(attempt/"decode.log").u8string());
   auto pics=codec=="vvenc"?vvcstudy::pictureLog(attempt/"decode.log",true,frames):study::pictureLog(attempt/"decode.log",true);
   check(pics.size()==frames,"Decoder picture count differs from requested frames");std::set<unsigned> pocs;for(const auto& p:pics)check(p.at("md5_ok").get<bool>()&&p.at("poc").get<unsigned>()<frames&&pocs.insert(p.at("poc").get<unsigned>()).second,"Decoder POC/hash mismatch");
   const unsigned w=spec.at("width"),h=spec.at("height");if(codec=="vvenc")vvcstudy::unpack(attempt/"decoder.y4m",raw,w,h,frames);
   auto rawInfo=fileInfo(raw);check(rawInfo.at("sha256")==enc.at("encoder_reconstruction").at("sha256")&&rawInfo.at("bytes")==enc.at("encoder_reconstruction").at("bytes"),"Independent decoder samples differ from encoder");
   if(ps&&codec=="hm")check(psvalidation::complexHashes(jpath(enc.at("directory"))/"encode.log",frames)==psvalidation::complexHashes(attempt/"decode.log",frames),"HM Q20 reconstruction hash mismatch");
   auto areas=codec=="hm"?hmarea::read(attempt/"mode_area.csv",frames,w,h):vvcarea::read(attempt/"mode_area.csv",frames,w,h);
   if(codec=="vvenc"&&!enc.at("reference").get<bool>()){auto a=vvcarea::read(jpath(enc.at("directory"))/"encoder_area.csv",frames,w,h);check(vvcarea::stablePicturesEqual(a.at("pictures"),areas.at("pictures")),"Encoder/decoder stable mode areas differ");vvcstudy::validateFastGeometry(areas,profiles.at(codec).at(spec.at("method").at("profile").get<std::string>()));}
   if(!ps)check(areas.at("ps_selected_area_pixels")==0,"PS Off contains PS area");
   atomicJson(dir/"mode_area.json",areas);
   json r=enc;r["decode_process"]=proc;r["decoder_pictures"]=pics;r["raw"]=rawInfo;r["mode_area_file"]=fileInfo(dir/"mode_area.json");r["mode_area"]=areas;r["mode_area"].erase("pictures");r["status"]="COMPLETE";
   atomicJson(dir/"decoded.json",r);if(codec=="vvenc")removeOwned(attempt/"decoder.y4m",out);progress(dir/"progress.json",{{"state","DECODED"}});return r;
 }
 json evaluateTrial(const json& spec){
   check(spec.at("suite_key")==identity.at("key"),"Config/build changed since task was created");const auto dir=jpath(spec.at("directory"));check(within(dir,out),"Trial outside results");
   if(complete(spec))return holo::read_json(dir/"result.json");
   json r={{"key",spec.at("key")},{"status","RUNNING"},{"sequence",spec.at("sequence")},{"method",spec.at("method").at("id")},{"qp",spec.at("qp")},{"channels",json::object()}};
   auto save=[&](){atomicJson(dir/"result.json",r);};save();const auto start=std::chrono::steady_clock::now();
   try{uint64_t anchorBytes=0,psBytes=0;double encTime=0,decTime=0;json areas=json::object();
     for(unsigned c=0;c<3;++c){auto ch=decodeOne(spec,c);anchorBytes+=ch.at("anchor").at("bytes").get<uint64_t>();if(ch.contains("ps"))psBytes+=ch.at("ps").at("bytes").get<uint64_t>();encTime+=ch.at("process").at("seconds").get<double>();decTime+=ch.at("decode_process").at("seconds").get<double>();areas[channels[c]]=ch.at("mode_area");r["channels"][channels[c]]=ch;save();}
     r["mode_area"]=directstudy::poolAreas(areas);
     const auto mapping=spec.at("mapping");fs::copy_file(jpath(mapping.at("required_metadata").at("path")),dir/"mapping.bin",fs::copy_options::overwrite_existing);
     const uint64_t total=anchorBytes+psBytes+56;const double samples=cfg.at("width").get<double>()*cfg.at("height").get<double>()*16,duration=16/cfg.at("frame_rate").get<double>(),rawBytes=samples*3*2*mapping.at("source_scalar_bits").get<double>()/8;
     r["rate"]={{"anchor_bytes",anchorBytes},{"ps_bytes",psBytes},{"mapping_bytes",56},{"total_bytes",total},{"kbps",double(total)*8/duration/1000},{"bits_per_rgb_hologram_pixel",double(total)*8/samples},{"bits_per_complex_sample",double(total)*8/(samples*3)},{"compression_ratio_original_payload",rawBytes/double(total)},{"original_complex_payload_bytes",rawBytes},{"ps_fraction",double(psBytes)/double(total)}};
     Evaluation ev(root,groupDir(spec.at("group")),cfg,spec.at("group").at("dataset"));ev.progress=[&](const json& j){progress(dir/"evaluation_progress.json",j);};
     const auto before=std::chrono::steady_clock::now();auto m=ev.evaluate(spec.at("sequence"),mapping,dir);
     r["metrics"]=json::object();for(auto k:{"mapped","mapped_real","mapped_imag","complex","reconstruction","unclipped_reconstruction"})r["metrics"][k]=m.at(k);
     r["metrics_file"]=fileInfo(dir/"metrics.json");r["reconstruction_png"]=m.at("reconstruction_png");r["reconstruction_png"].erase("files");
     r["timing"]={{"encode_seconds",encTime},{"decode_seconds",decTime},{"evaluation_seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-before).count()},{"worker_seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()},{"note","encode/decode are sums of channel wall times; concurrent jobs overlap"}};
     r["status"]="COMPLETE";save();for(const auto& c:channels)removeOwned(dir/c/"decoder.yuv",out);return r;
   }catch(const std::exception& e){r["status"]="ERROR";r["error"]=e.what();save();throw;}
 }
};
}
