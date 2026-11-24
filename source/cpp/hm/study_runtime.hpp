#pragma once
#include "hm_validation_common.hpp"
#include <functional>
#include <thread>
#include <atomic>
namespace study {
using namespace hmvalidation;
inline std::atomic<bool> stopRequested{false};
inline BOOL WINAPI interruptHandler(DWORD event){if(event==CTRL_C_EVENT||event==CTRL_BREAK_EVENT){stopRequested=true;return TRUE;}return FALSE;}
inline void atomicText(const fs::path& path,const std::string& text) {
  static std::atomic<uint64_t> serial{0};
  fs::create_directories(path.parent_path());auto tmp=path;
  tmp+=L".partial."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++serial);
  writeText(tmp,text);
  DWORD error=ERROR_SUCCESS;
  // A status reader can briefly hold a Windows handle without delete sharing.
  for(unsigned retry=0;retry<100;++retry){if(MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return;
    error=GetLastError();if(error!=ERROR_SHARING_VIOLATION&&error!=ERROR_ACCESS_DENIED)break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
  std::error_code ignored;fs::remove(tmp,ignored);
  throw std::runtime_error("Cannot commit "+path.u8string()+"; Windows error "+std::to_string(error));
}
inline void atomicJson(const fs::path& path,const json& j){atomicText(path,j.dump(2)+"\n");}
inline std::atomic<uint64_t> telemetryErrors{0};
template<class Function> inline void advisoryProgress(Function action) noexcept {
  try { action(); } catch(const std::exception&) { ++telemetryErrors; }
}
inline json workerStatus(const fs::path& path,const std::string& sequence,const std::string& method,int qp) {
  try{return holo::read_json(path);}
  catch(const std::exception& error) {
    // Telemetry is advisory. The worker future and final result determine
    // success/failure; a transient dashboard read must not cancel the study.
    return {{"sequence",sequence},{"method",method},{"qp",qp},{"state","STATUS_UNAVAILABLE"},
            {"stage","progress snapshot unavailable; worker outcome still pending"},{"status_read_error",error.what()}};
  }
}
inline std::string readText(const fs::path& path){std::ifstream in(path,std::ios::binary);check(bool(in),"Cannot read "+path.u8string());std::ostringstream s;s<<in.rdbuf();check(!in.bad(),"Read failed");return s.str();}
inline fs::path resolve(const fs::path& root,const std::string& p){return fs::weakly_canonical(root/fs::u8path(p));}
inline bool within(const fs::path& path,const fs::path& root){auto a=fs::weakly_canonical(path).wstring(),b=fs::weakly_canonical(root).wstring();std::transform(a.begin(),a.end(),a.begin(),towlower);std::transform(b.begin(),b.end(),b.begin(),towlower);return a==b||(a.size()>b.size()&&a.compare(0,b.size(),b)==0&&(a[b.size()]==L'\\'||a[b.size()]==L'/'));}
inline void removeOwned(const fs::path& file,const fs::path& root){check(within(file,root)&&file!=root,"Refusing cleanup outside the study output");if(fs::is_regular_file(file))fs::remove(file);}
inline std::string tail(const fs::path& path,size_t limit=8192){std::ifstream in(path,std::ios::binary);if(!in)return {};in.seekg(0,std::ios::end);auto end=in.tellg();if(end>std::streamoff(limit))in.seekg(end-std::streamoff(limit));else in.seekg(0);std::ostringstream s;s<<in.rdbuf();return s.str();}
inline json lastPicture(const fs::path& path){auto text=tail(path);auto p=text.rfind("POC ");if(p==std::string::npos)return json();auto line=text.substr(p,text.find('\n',p)-p);return {{"last_picture_log",line}};}
inline json runChild(const fs::path& exe,const std::vector<std::string>& args,const fs::path& log,const fs::path& cwd,const std::function<void(const json&)>& progress) {
  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
  Handle output{CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
  Handle input{CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
  check(output.value!=INVALID_HANDLE_VALUE&&input.value!=INVALID_HANDLE_VALUE,"Cannot create process log");
  Handle job{CreateJobObjectW(nullptr,nullptr)};check(job.value!=nullptr,"Cannot create child process job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  check(SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=0,"Cannot set job lifetime");
  STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;si.StartupInfo.hStdInput=input.value;si.StartupInfo.hStdOutput=si.StartupInfo.hStdError=output.value;
  SIZE_T attributeBytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&attributeBytes);std::vector<unsigned char> attributes(attributeBytes);
  si.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());check(InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&attributeBytes)!=0,"Cannot initialize child handle list");
  struct AttributeCleanup{LPPROC_THREAD_ATTRIBUTE_LIST value;~AttributeCleanup(){DeleteProcThreadAttributeList(value);}} cleanup{si.lpAttributeList};
  HANDLE allowedHandles[2]={input.value,output.value};check(UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,allowedHandles,sizeof(allowedHandles),nullptr,nullptr)!=0,"Cannot restrict child handles");
  std::wstring command=quote(exe.wstring());for(const auto& s:args)command+=L" "+quote(wide(s));
  PROCESS_INFORMATION pi{};const auto start=std::chrono::steady_clock::now();
  const BOOL created=CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,cwd.c_str(),&si.StartupInfo,&pi);
  const auto error=created?0:GetLastError();check(created!=0,"Cannot start "+exe.u8string()+"; Windows error "+std::to_string(error));
  Handle process{pi.hProcess},thread{pi.hThread};
  if(!AssignProcessToJobObject(job.value,process.value)){TerminateProcess(process.value,1);throw std::runtime_error("Cannot attach encoder/decoder to runner lifetime");}
  check(ResumeThread(thread.value)!=DWORD(-1),"Cannot resume child");
  while(true){const auto wait=WaitForSingleObject(process.value,2000);check(wait==WAIT_TIMEOUT||wait==WAIT_OBJECT_0,"Child wait failed");
    // A dashboard callback must never unwind the job handle and kill a codec.
    advisoryProgress([&]{auto status=lastPicture(log);if(status.is_null())status=json::object();status["child_pid"]=pi.dwProcessId;status["child_seconds"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();progress(status);});
    if(wait==WAIT_OBJECT_0)break;}
  DWORD code=1;check(GetExitCodeProcess(process.value,&code)!=0,"Cannot read exit status");
  return {{"executable",exe.u8string()},{"arguments",args},{"log",log.u8string()},{"exit_code",code},
    {"seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()}};
}
inline json pictureLog(const fs::path& path,bool decoder){std::ifstream in(path);check(bool(in),"Missing picture log");json rows=json::array();std::string line;
  while(std::getline(in,line)){if(line.rfind("POC ",0)!=0)continue;std::istringstream s(line);std::string token;int poc=-1;s>>token>>poc;json row={{"poc",poc}};
    auto qp=line.find(" QP ");if(qp==std::string::npos)qp=line.find(" QP");if(qp!=std::string::npos)row["actual_qp"]=std::stoi(line.substr(qp+3));
    if(decoder)row["md5_ok"]=line.find("[MD5:")!=std::string::npos&&line.find("(OK)")!=std::string::npos;rows.push_back(row);}return rows;}
}
