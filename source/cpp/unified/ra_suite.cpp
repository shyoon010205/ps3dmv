#include "validation.hpp"
#include "reports.hpp"
#include "dashboard.hpp"
#include <deque>
#ifdef _OPENMP
#include <omp.h>
#endif
using namespace unified;
namespace {
void status(Engine& e,json s){
 s["pid"]=GetCurrentProcessId();s["updated_local"]=timestamp();s["stop_requested"]=e.stopped();
 advisoryProgress([&]{atomicJson(e.out/"status.json",s);refreshDashboard(e.out,e.cfg);});
}
struct Task{std::string label;fs::path progress;std::function<void()> action;};
// One global queue, no RGB/QP/sequence barrier inside an encoding round.
bool queue(Engine& e,std::vector<Task> tasks,unsigned slots,const std::string& stage,const std::function<void()>& onComplete={},bool validation=false){
 struct Running{size_t index;std::future<void> future;};std::vector<Running> active;size_t next=0,done=0;json errors=json::array();
 std::atomic<bool> validationFailed{false};
 while(next<tasks.size()||!active.empty()){
   while(!e.stopped()&&!validationFailed&&active.size()<slots&&next<tasks.size()){
     const auto i=next++;active.push_back({i,std::async(std::launch::async,[&,i]{
       const unsigned attempts=validation?1u:e.cfg.at("retry").at("maximum_attempts_per_invocation").get<unsigned>();
       for(unsigned attempt=1;attempt<=attempts;++attempt){try{tasks[i].action();return;}catch(const ValidationStopped&){throw;}catch(const std::exception& err){
         if(validation){validationFailed=true;throw;}
         auto path=tasks[i].progress.parent_path();atomicJson(path/("retry_"+timestamp()+"_"+std::to_string(attempt)+".json"),{{"label",tasks[i].label},{"attempt",attempt},{"maximum_attempts",attempts},{"error",err.what()},{"validation",false}});
         if(attempt==attempts||e.stopped())throw;
         const unsigned delay=e.cfg.at("retry").value("backoff_seconds",30u);
         for(unsigned second=0;second<delay&&!e.stopped();++second)std::this_thread::sleep_for(std::chrono::seconds(1));
         if(e.stopped())throw;
       }}
     })});
   }
   for(size_t i=0;i<active.size();){
     if(active[i].future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready){++i;continue;}
     const auto label=tasks[active[i].index].label;
     try{active[i].future.get();std::cout<<stage<<" COMPLETE "<<label<<std::endl;}catch(const ValidationStopped&){std::cout<<stage<<" STOPPED "<<label<<std::endl;}catch(const std::exception& err){errors.push_back({{"task",label},{"error",err.what()}});std::cerr<<stage<<" ERROR "<<label<<": "<<err.what()<<std::endl;}
     ++done;active.erase(active.begin()+i);if(onComplete)advisoryProgress(onComplete);
   }
   json jobs=json::array();for(const auto& a:active){auto p=readIf(tasks[a.index].progress);
     if(validation&&!p.is_null()){json compact={{"state",p.value("status","")},{"stage",p.value("stage","")},{"frames",p.value("frames",0u)},{"completed_cases",p.value("cases",json::object()).size()}};if(p.contains("active_progress"))compact["process"]=readIf(jpath(p.at("active_progress")));p=std::move(compact);}
     jobs.push_back({{"task",tasks[a.index].label},{"progress",p}});}
   status(e,{{"state",e.stopped()||validationFailed?"DRAINING":validation?"VALIDATING":"RUNNING"},{"stage",stage},{"slots",slots},{"completed",done},{"total",tasks.size()},{"pending",tasks.size()-next},{"active",jobs},{"errors",errors}});
   if((e.stopped()||validationFailed)&&active.empty())break;
   if(!active.empty())std::this_thread::sleep_for(std::chrono::milliseconds(1000));
 }
 if(!errors.empty()){atomicJson(e.out/"last_errors.json",errors);throw std::runtime_error(validation?"Validation failed; no experiment started. See last_errors.json and per-group validation logs.":"Experiment retries exhausted. Completed channels are retained; see last_errors.json.");}
 return !e.stopped();
}
// The barrier runs before any full experiment, including RunOnly. A late
// group's missing/failed gate can never be discovered after an earlier run.
bool validationThenExperiments(bool validate,bool run,const std::function<bool()>& validateAll,const std::function<bool()>& requireAll,const std::function<bool()>& runAll){
 if(validate&&!validateAll())return false;
 if(run){if(!requireAll())return false;return runAll();}
 return true;
}
std::vector<json> selectedGroups(const Engine& e,const std::string& groupOnly){
 std::vector<json> groups;for(const auto& g:e.cfg.at("groups"))if(groupOnly.empty()||g.at("id")==groupOnly)groups.push_back(g);
 check(!groups.empty(),"Unknown group filter");return groups;
}
bool validateAll(Engine& e,const std::vector<json>& groups,bool reuse){
 std::vector<Task> tasks;
 for(const auto& g:validationGroups(e,groups)){bool ready=false;if(reuse){try{requireValidation(e,g);ready=true;}catch(...){}}
   if(!ready)tasks.push_back({g.at("id"),e.out/"validation"/g.at("id").get<std::string>()/"latest.json",[&,g]{validateGroup(e,g);}});
 }
 return queue(e,std::move(tasks),e.cfg.at("resources").at("validation_slots"),"codec validation (shared by datasets)",{},true);
}
void preflight(Engine& e){
 for(const auto& g:e.cfg.at("groups")){
   check(safeName(g.at("id"))&&e.cfg.at("codecs").contains(g.at("codec")),"Invalid group");
   const std::string phase=g.at("dataset");check(safeName(phase)&&fs::is_regular_file(e.root/"config"/(phase+".json")),"Invalid dataset");
   for(const auto& name:e.cfg.at("sequences"))for(unsigned f=0;f<16;++f)check(fs::is_regular_file(original(e.root/"data/holo"/phase/name.get<std::string>(),f)),"Missing original frame: "+name.get<std::string>()+" / "+std::to_string(f));
 }
 check(e.cfg.at("metrics").at("save_reconstruction_png").get<bool>(),"This protocol requires all 160 PNGs per trial");
 std::cout<<"READY: full experiment frames0..15; validation="<<e.cfg.at("validation").at("frames")<<" frames, "<<e.cfg.at("validation").at("crop").at("width")<<'x'<<e.cfg.at("validation").at("crop").at("height")<<", one physical channel, slots="<<e.cfg.at("resources").at("validation_slots")<<"; encoding slots="<<e.cfg.at("resources").at("encoding_slots")<<", evaluation slots="<<e.cfg.at("resources").at("evaluation_slots")<<". No codec/reconstruction executed.\n";
}
bool runGroup(Engine& e,const json& group){
 requireValidation(e,group);const auto dir=e.groupDir(group);fs::create_directories(dir);
 auto c=e.cfg;c["adaptive_qp"]["qp_min"]=e.cfg.at("codecs").at(group.at("codec").get<std::string>()).at("qp_min");c["adaptive_qp"]["qp_max"]=e.cfg.at("codecs").at(group.at("codec").get<std::string>()).at("qp_max");
 auto plan=readIf(dir/"plan.json");if(plan.is_null())plan={{"suite_key",e.identity.at("key")},{"qps",json::object()},{"adaptive_history",json::array()}};
 check(plan.at("suite_key")==e.identity.at("key"),"Scientific settings/build changed. Use a new suite output_root; historical results will not be mixed.");
 for(const auto& seq:e.cfg.at("sequences"))for(const auto& m:e.cfg.at("qp_lists").items()){
   auto& q=plan["qps"][seq.get<std::string>()][m.key()];if(q.is_null())q=json::array();for(const auto& n:m.value())if(std::find(q.begin(),q.end(),n)==q.end())q.push_back(n);
 }
 atomicJson(dir/"plan.json",plan);
 std::map<std::string,json> mappings;
 for(const auto& seq:e.cfg.at("sequences")){
   if(e.stopped())return false;const std::string name=seq;
   auto previous=readIf(dir/"prepared"/name/"prepared.json");bool allDone=!previous.is_null();
   if(allDone){for(const auto& f:previous.at("sources"))if(!verified(f)){allDone=false;break;}}
   if(allDone)for(const auto& m:e.methods)for(const auto& qp:plan.at("qps").at(name).at(m.first))if(!e.complete(e.makeSpec(group,name,m.first,qp,previous)))allDone=false;
   if(allDone){mappings[name]=previous;continue;}
   Evaluation ev(e.root,dir,e.cfg,group.at("dataset"));ev.progress=[&](json j){j["state"]="RUNNING";j["group"]=group.at("id");status(e,j);};mappings[name]=ev.prepare(name);
 }
 bool bdLimited=false;
 while(true){
   std::vector<json> pending;
   for(const auto& seq:e.cfg.at("sequences"))for(const auto& method:e.methods)for(const auto& qp:plan.at("qps").at(seq.get<std::string>()).at(method.first)){
     auto spec=e.makeSpec(group,seq,method.first,qp,mappings.at(seq));if(e.complete(spec))continue;
     const auto trial=jpath(spec.at("directory"));fs::create_directories(trial);atomicJson(trial/"spec.json",spec);pending.push_back(spec);
   }
   // Completed groups may have deleted prepared raw video. Recreate it only when
   // a new or unfinished trial actually needs mapping/metrics input.
   std::set<std::string> need;for(const auto& spec:pending)need.insert(spec.at("sequence"));
   for(const auto& seq:need){bool present=true;for(const auto& f:mappings.at(seq).at("inputs"))present=present&&verified(f);if(!present){Evaluation ev(e.root,dir,e.cfg,group.at("dataset"));ev.progress=[&](json j){status(e,j);};mappings[seq]=ev.prepare(seq);}}
   std::vector<Task> encTasks;
   for(const auto& spec:pending)for(unsigned channel=0;channel<3;++channel){const auto trial=jpath(spec.at("directory"));const auto label=spec.at("sequence").get<std::string>()+"/"+spec.at("method").at("id").get<std::string>()+"/QP"+spec.at("qp").dump()+"/"+channels[channel];
     encTasks.push_back({label,trial/channels[channel]/"progress.json",[&,spec,channel]{e.encodeOne(spec,channel,false,e.cfg.at("ps3dmvp").at("optimized_search"));}});
   }
   if(!queue(e,std::move(encTasks),e.cfg.at("resources").at("encoding_slots"),group.at("id").get<std::string>()+" encoding"))return false;
   std::vector<Task> evalTasks;
   for(const auto& spec:pending){const auto trial=jpath(spec.at("directory"));const auto label=spec.at("sequence").get<std::string>()+"/"+spec.at("method").at("id").get<std::string>()+"/QP"+spec.at("qp").dump();
     evalTasks.push_back({label,trial/"evaluation_progress.json",[&,trial]{
       const auto log=trial/("evaluation_"+timestamp()+".log");auto r=runChild(executablePath(),{"--root",e.root.u8string(),"--config",(e.out/"config_snapshot.json").u8string(),"--evaluate",(trial/"spec.json").u8string()},log,trial,[&](json j){j["state"]="EVALUATING";j["worker_log"]=log.u8string();e.progress(trial/"worker.json",j);});
       check(r.at("exit_code")==0,"Evaluation worker failed: "+log.u8string());
     }});
   }
   if(!queue(e,std::move(evalTasks),e.cfg.at("resources").at("evaluation_slots"),group.at("id").get<std::string>()+" reconstruction/evaluation",[&]{reports(e,group,plan,c);} ))return false;
   auto r=reports(e,group,plan,c);bool added=false;bdLimited=false;
   if(c.at("adaptive_qp").at("enabled").get<bool>())for(const auto& seq:c.at("sequences")){
     json assessment={{"requests",json::array()}};bool sufficient=true;
     for(const auto& pair:pairs()){const auto& cov=r.at("sequences").at(seq.get<std::string>()).at(pair.first+"__"+pair.second).at("coverage");sufficient=sufficient&&cov.at("sufficient").get<bool>();for(const auto& q:cov.at("requests"))assessment["requests"].push_back(q);}
     if(sufficient)continue;auto q=nextQp(c,plan,seq,assessment);
     if(q.is_null()){bdLimited=true;plan["unresolved_coverage"][seq.get<std::string>()]={{"reason","No requested legal QP remains; no extrapolation performed"},{"assessment",assessment}};continue;}
     const std::string method=q.at("method");plan["qps"][seq.get<std::string>()][method].push_back(q.at("qp"));q["sequence"]=seq;q["added_local"]=timestamp();plan["adaptive_history"].push_back(q);added=true;
   }
   atomicJson(dir/"plan.json",plan);if(!added)break;
 }
 auto final=reports(e,group,plan,c);
 for(const auto& seq:final.at("sequences"))for(const auto& comparison:seq)for(const auto* metric:{"reconstruction_psnr","reconstruction_ssim"})if(comparison.at("metrics").at(metric).at("status")!="VALID")bdLimited=true;
 final["status"]=bdLimited?"COMPLETE_BD_UNAVAILABLE":"COMPLETE";final["frames"]=16;atomicJson(dir/"result.json",final);
 if(!e.cfg.at("storage").at("keep_prepared_inputs").get<bool>())for(const auto& seq:mappings)for(const auto& f:seq.second.at("inputs"))removeOwned(jpath(f.at("path")),dir);
 return true;
}
}
#ifndef RA_SUITE_EMBEDDED
int main(int argc,char** argv){
 fs::path root=fs::current_path(),config="config/suite.json",worker;std::string action="--check",groupOnly;json failure;
 try{
   for(int i=1;i<argc;++i){const std::string a=argv[i];if((a=="--root"||a=="--config"||a=="--evaluate"||a=="--group")&&i+1<argc){const auto v=argv[++i];if(a=="--root")root=fs::u8path(v);else if(a=="--config")config=fs::u8path(v);else if(a=="--group")groupOnly=v;else{worker=fs::u8path(v);action=a;}}else if(a=="--check"||a=="--validate"||a=="--run"||a=="--all"||a=="--dashboard")action=a;else throw std::runtime_error("Invalid argument: "+a);}
   const auto configPath=resolve(root,config.u8string());auto cfg=holo::read_json(configPath);
   if(action=="--dashboard"){const auto out=resolve(root,cfg.at("output_root"));check(within(out,root/"results")&&out!=root/"results","Invalid dashboard output");refreshDashboard(out,cfg);return 0;}
   Engine e(root,cfg);
   const auto threads=e.cfg.at("resources").at("reconstruction_threads").get<unsigned>();SetEnvironmentVariableW(L"OMP_NUM_THREADS",std::to_wstring(threads).c_str());
#ifdef _OPENMP
   omp_set_num_threads(int(threads));
#endif
   if(action=="--evaluate"){e.evaluateTrial(holo::read_json(worker));return 0;}
   preflight(e);if(action=="--check")return 0;
   SetConsoleCtrlHandler(interruptHandler,TRUE);fs::create_directories(e.out);
   Handle lock{CreateFileW((e.out/"run.lock").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr)};check(lock.value!=INVALID_HANDLE_VALUE,"This suite is already running");
   auto old=readIf(e.out/"identity.json");check(old.is_null()||old.at("key")==e.identity.at("key"),"Different scientific setup/build already exists here. Set a new output_root.");
   atomicJson(e.out/"identity.json",e.identity);atomicJson(e.out/"config_snapshot.json",cfg);removeOwned(e.out/"stop.request",e.out);
   const auto groups=selectedGroups(e,groupOnly);
   validationThenExperiments(action=="--validate"||action=="--all",action=="--run"||action=="--all",
     [&]{return validateAll(e,groups,action=="--all");},
     [&]{if(e.stopped())return false;for(const auto& g:validationGroups(e,groups))requireValidation(e,g);return true;},
     [&]{for(const auto& g:groups){if(e.stopped()||!runGroup(e,g))return false;}return true;});
   bool bdLimited=false;
   if(action=="--run"||action=="--all")for(const auto& g:e.cfg.at("groups")){
     if(!groupOnly.empty()&&g.at("id")!=groupOnly)continue;
     auto result=readIf(e.groupDir(g)/"result.json");if(!result.is_null()&&result.value("status","")=="COMPLETE_BD_UNAVAILABLE")bdLimited=true;
   }
   const std::string finalState=e.stopped()?"STOPPED":bdLimited?"COMPLETE_BD_UNAVAILABLE":"COMPLETE";
   status(e,{{"state",finalState},{"stage",action=="--validate"?"validation":"suite"},{"scope",groupOnly.empty()?"all groups":groupOnly}});
   std::cout<<(e.stopped()?"STOPPED: running jobs drained; resume with the same command.":finalState)<<'\n';return e.stopped()?2:0;
 }catch(const std::exception& err){std::cerr<<"ERROR: "<<err.what()<<std::endl;
   try{if(action!="--dashboard"){const auto cfg=holo::read_json(resolve(root,config.u8string()));const auto out=resolve(root,cfg.at("output_root"));if(within(out,root/"results")){failure={{"state","ERROR"},{"error",err.what()},{"updated_local",timestamp()},{"pid",GetCurrentProcessId()}};atomicJson(worker.empty()?out/"status.json":worker.parent_path()/"worker_error.json",failure);if(worker.empty())refreshDashboard(out,cfg);}}}catch(...){}
   return 1;
 }
}

#endif
