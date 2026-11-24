#pragma once
#include "engine.hpp"
namespace unified {
inline bool runnerAlive(const json& s){
 if(!s.contains("pid")||!s.at("pid").is_number_integer()||s.at("pid").get<int64_t>()<=0)return false;
 Handle process{OpenProcess(SYNCHRONIZE,FALSE,s.at("pid").get<DWORD>())};
 return process.value&&process.value!=INVALID_HANDLE_VALUE&&WaitForSingleObject(process.value,0)==WAIT_TIMEOUT;
}
inline std::string dashboardLink(const fs::path& path,const fs::path& out,const std::string& label){
 if(!within(path,out)||!fs::is_regular_file(path))return "<span class=muted>"+escape(label)+" pending</span>";
 return "<a href=\""+escape(fs::relative(path,out).generic_u8string())+"\">"+escape(label)+"</a>";
}
inline std::string dashboardHtml(const fs::path& out,const json& cfg,json s,bool alive){
 const auto text=[](const json& j,const char* k,const std::string& fallback="—"){auto p=j.find(k);return p!=j.end()&&p->is_string()?p->get<std::string>():fallback;};
 const auto number=[](const json& j,const char* k,unsigned fallback=0u){auto p=j.find(k);return p!=j.end()&&p->is_number_integer()&&p->get<int64_t>()>=0?p->get<unsigned>():fallback;};
 const std::string rawState=text(s,"state","NOT_STARTED");
 const bool wasRunning=rawState=="RUNNING"||rawState=="VALIDATING"||rawState=="DRAINING";
 const std::string state=wasRunning&&!alive?"INTERRUPTED":rawState;
 std::ostringstream h;h<<R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta http-equiv="refresh" content="5"><title>PS3DMVP experiment status</title><style>
:root{color-scheme:light}*{box-sizing:border-box}body{margin:0;background:#f3f5f8;color:#172236;font:15px/1.6 'Segoe UI',sans-serif}main{max-width:1280px;margin:auto;padding:28px}h1{font-size:28px;margin:0}h2{font-size:20px;margin:0 0 16px}.muted{color:#68758a}.top{display:flex;justify-content:space-between;align-items:center;gap:16px;margin-bottom:22px}.badge{border-radius:20px;padding:6px 14px;background:#e4edfc;color:#15468c;font-weight:700}.error{background:#fff0ef;color:#992b28;border-left:4px solid #c4463e;padding:14px;overflow-wrap:anywhere}.cards{display:grid;grid-template-columns:repeat(4,1fr);gap:14px;margin:18px 0}.card,section{background:white;border:1px solid #dce2eb;border-radius:12px;padding:20px}.card strong{display:block;font-size:27px}.card span{color:#68758a;font-size:13px}section{margin:18px 0}.scroll{overflow:auto}table{width:100%;border-collapse:collapse}th,td{text-align:left;padding:12px;border-bottom:1px solid #e5e9f0;vertical-align:top}th{font-size:13px;color:#526076}td{overflow-wrap:anywhere}a{color:#235da8;text-decoration:none;margin-right:12px}a:hover{text-decoration:underline}progress{display:block;width:120px;height:10px;margin-top:6px}.log{font:12px/1.6 Consolas,monospace;max-width:420px;overflow-wrap:anywhere;white-space:pre-wrap}details pre{white-space:pre-wrap;overflow-wrap:anywhere;max-height:360px;overflow:auto;font-size:12px}footer{font-size:13px;color:#68758a}@media(max-width:800px){main{padding:14px}.cards{grid-template-columns:repeat(2,1fr)}.top{align-items:flex-start}.card,section{padding:14px}th,td{padding:8px}}
</style></head><body><main><header class="top"><div><h1>PS3DMVP experiment status</h1><div class="muted">RA · Fixed / Random · HM / VVenC</div></div><span class="badge">)HTML"<<escape(state)<<"</span></header>";
 h<<"<div class=muted>Last update "<<escape(text(s,"updated_local"))<<" · "<<escape(text(s,"stage","waiting to start"))<<" · refreshes every 5 seconds</div>";
 if(state=="INTERRUPTED")h<<"<p class=error>The runner stopped. This page shows its last saved state; rerun the command to resume.</p>";
 if(s.contains("error"))h<<"<p class=error>"<<escape(text(s,"error"))<<"</p>";
 if(s.contains("errors")&&s.at("errors").is_array())for(const auto& err:s.at("errors"))h<<"<p class=error>"<<escape(text(err,"task"))<<": "<<escape(text(err,"error"))<<"</p>";
 std::set<std::string> codecs;for(const auto& g:cfg.at("groups"))codecs.insert(g.at("codec").get<std::string>());
 unsigned passed=0;std::map<std::string,json> validations;const auto identity=readIf(out/"identity.json");
 for(const auto& c:codecs){auto r=readIf(out/"validation"/c/"latest.json");if(!r.is_null()&&r.value("status","")=="PASS"){if(r.value("scope","")!="codec"||identity.is_null()||r.value("suite_key",json())!=identity.at("key"))r["status"]="STALE";else ++passed;}validations[c]=r;}
 auto jobs=s.value("active",json::array());if(!jobs.is_array())jobs=json::array();
 h<<"<div class=cards><div class=card><span>Codec validation PASS</span><strong>"<<passed<<" / "<<codecs.size()<<"</strong></div><div class=card><span>Active jobs / slots</span><strong>"<<jobs.size()<<" / "<<number(s,"slots")<<"</strong></div><div class=card><span>Current stage progress</span><strong>"<<number(s,"completed")<<" / "<<number(s,"total")<<"</strong></div><div class=card><span>Pending jobs</span><strong>"<<number(s,"pending")<<"</strong></div></div>";
 h<<"<section><h2>Codec validation</h2><p class=muted>"<<escape(cfg.at("validation").value("dataset",std::string("dynamic")))<<" · "<<cfg.at("validation").at("crop").at("width")<<" × "<<cfg.at("validation").at("crop").at("height")<<" · "<<channels[cfg.at("validation").at("channel").get<unsigned>()]<<" channel · "<<cfg.at("validation").at("frames")<<" frames · shared across all datasets for the codec</p><div class=scroll><table><thead><tr><th>Codec</th><th>Status</th><th>Completed cases</th><th>Current stage</th><th>Record</th></tr></thead><tbody>";
 const unsigned cases=cfg.at("validation").value("verify_optimization",true)?6:4;
 for(const auto& c:codecs){const auto& r=validations.at(c);const auto count=r.is_null()?0:r.value("cases",json::object()).size();h<<"<tr><td>"<<escape(c)<<"</td><td>"<<escape(text(r,"status","pending"))<<"</td><td>"<<count<<" / "<<cases<<"<progress max="<<cases<<" value="<<count<<"></progress></td><td>"<<escape(text(r,"stage"))<<"</td><td>"<<dashboardLink(out/"validation"/c/"latest.json",out,"Validation result")<<"</td></tr>";}
 h<<"</tbody></table></div></section><section><h2>Active jobs</h2>";
 if(jobs.empty())h<<"<p class=muted>No active job is available.</p>";
 else{h<<"<div class=scroll><table><thead><tr><th>Job</th><th>Status / stage</th><th>Latest frame</th><th>Time / log</th></tr></thead><tbody>";
 for(const auto& j:jobs){const auto p=j.value("progress",json::object());const auto process=p.is_object()?p.value("process",p):json::object();h<<"<tr><td>"<<escape(text(j,"task"))<<"</td><td>"<<escape(text(p,"state"))<<"<br>"<<escape(text(p,"stage",text(process,"state")))<<"</td><td class=log>"<<escape(text(process,"last_picture_log","waiting for frame log"))<<"</td><td>";if(process.contains("child_seconds")&&process.at("child_seconds").is_number())h<<std::fixed<<std::setprecision(1)<<process.at("child_seconds").get<double>()<<" s<br>";if(process.contains("log")&&process.at("log").is_string())h<<dashboardLink(jpath(process.at("log")),out,"Current log");h<<"</td></tr>";}h<<"</tbody></table></div>";}
 h<<"</section><section><h2>Full experiments</h2><p class=muted>Starts after codec validation; links appear when results are available.</p><div class=scroll><table><thead><tr><th>Group</th><th>Codec / data</th><th>Completed evaluations</th><th>Result</th></tr></thead><tbody>";
 for(const auto& g:cfg.at("groups")){const auto dir=out/g.at("id").get<std::string>();const auto r=readIf(dir/"bd_results.json");h<<"<tr><td>"<<escape(g.at("id").get<std::string>())<<"</td><td>"<<escape(g.at("codec").get<std::string>())<<" / "<<escape(g.at("dataset").get<std::string>())<<"</td><td>";if(r.is_null())h<<"pending";else h<<r.value("completed_trials",0u)<<" / "<<r.value("planned_trials",0u);h<<"</td><td>"<<dashboardLink(dir/"results.html",out,"Results / PNG / BD")<<"</td></tr>";}
 h<<"</tbody></table></div></section><details><summary>Detailed status JSON</summary><pre>"<<escape(s.dump(2))<<"</pre></details><footer>This is a status page. Opening or refreshing it does not start codec experiments.</footer></main></body></html>";return h.str();
}
inline void refreshDashboard(const fs::path& out,const json& cfg){auto s=readIf(out/"status.json");if(s.is_null())s={{"state","NOT_STARTED"},{"stage","waiting to start"}};atomicText(out/"status.html",dashboardHtml(out,cfg,s,runnerAlive(s)));}
}
