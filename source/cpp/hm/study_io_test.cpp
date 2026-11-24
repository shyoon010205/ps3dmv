#include "study_runtime.hpp"
#include <future>
#include <mutex>
using namespace study;
int main(int argc,char** argv) {
  try {
    if(argc==3&&std::string(argv[1])=="--fixture-child") {
      std::this_thread::sleep_for(std::chrono::milliseconds(2300));
      writeText(fs::u8path(argv[2]),"child completed despite telemetry error\n");return 0;
    }
    check(argc==2,"Usage: study_io_test NEW_OUTPUT_DIRECTORY");
    const auto root=fs::absolute(fs::u8path(argv[1]));
    check(fs::create_directory(root),"Test directory must not already exist");
    // Four publishers and eight readers, each snapshot checksummed by content.
    for(unsigned i=0;i<4;++i)atomicJson(root/(std::to_string(i)+".json"),{{"version",0},{"payload",std::string(4096,'A')}});
    std::atomic<unsigned> finished{0},reads{0};std::vector<std::future<void>> tasks;
    for(unsigned i=0;i<4;++i)tasks.push_back(std::async(std::launch::async,[&,i]{
      struct Done {std::atomic<unsigned>& count;~Done(){++count;}} done{finished};
      for(unsigned n=1;n<=200;++n)atomicJson(root/(std::to_string(i)+".json"),{{"version",n},{"payload",std::string(4096,char('A'+n%26))}});
    }));
    for(unsigned i=0;i<8;++i)tasks.push_back(std::async(std::launch::async,[&,i]{
      unsigned last=0;
      do {
        const auto j=holo::read_json(root/(std::to_string(i%4)+".json"));const auto n=j.at("version").get<unsigned>();
        check(n>=last&&j.at("payload")==std::string(4096,char('A'+n%26)),"Torn or regressing snapshot");last=n;++reads;
      } while(finished<4);
    }));
    std::exception_ptr failure;
    for(auto& task:tasks){try{task.get();}catch(...){if(!failure)failure=std::current_exception();}}
    if(failure)std::rethrow_exception(failure);
    // Real Windows sharing conflict: a reader must retry until the owner closes.
    const auto locked=root/"0.json";
    HANDLE handle=CreateFileW(locked.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
    check(handle!=INVALID_HANDLE_VALUE,"Cannot establish sharing-conflict test");
    auto pending=std::async(std::launch::async,[&]{return holo::read_json(locked);});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));CloseHandle(handle);
    check(pending.get().at("version")==200,"Reader did not recover from sharing conflict");
    writeText(root/"bad.json","{broken");bool strict=false;
    try{(void)holo::read_json(root/"bad.json");}catch(const json::parse_error&){strict=true;}
    check(strict,"Malformed result/config JSON must not be accepted");
    const auto status=workerStatus(root/"bad.json","test","baseline",8);
    check(status.at("state")=="STATUS_UNAVAILABLE"&&status.contains("status_read_error"),"Advisory telemetry error was lost");
    // Regression for cave QP4: a failed status commit must not close the
    // kill-on-close job while its child is still running.
    const auto lockedStatus=root/"locked.json";atomicJson(lockedStatus,{{"state","old"}});
    Handle denyRename{CreateFileW(lockedStatus.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr)};
    check(denyRename.value!=INVALID_HANDLE_VALUE,"Cannot lock telemetry snapshot");
    const auto errorsBefore=telemetryErrors.load();
    advisoryProgress([&]{atomicJson(lockedStatus,{{"state","new"}});});
    check(telemetryErrors==errorsBefore+1,"Failed commit should be advisory");
    CloseHandle(denyRename.value);denyRename.value=INVALID_HANDLE_VALUE;
    atomicJson(lockedStatus,{{"state","recovered"}});
    check(holo::read_json(lockedStatus).at("state")=="recovered","Next snapshot did not recover");
    wchar_t module[32768];check(GetModuleFileNameW(nullptr,module,32768)>0,"Cannot resolve test executable");
    unsigned callbacks=0;
    const auto child=runChild(fs::path(module),{"--fixture-child",(root/"child.done").u8string()},root/"child.log",root,[&](const json&){++callbacks;throw std::runtime_error("injected status failure");});
    check(callbacks>0&&child.at("exit_code")==0&&fs::exists(root/"child.done"),"Telemetry exception killed a running child");
    // Concurrent writers to the same path must not share a .partial file.
    std::vector<std::future<void>> writers;
    for(unsigned i=0;i<4;++i)writers.push_back(std::async(std::launch::async,[&,i]{for(unsigned n=0;n<20;++n)atomicJson(root/"shared.json",{{"writer",i},{"payload",std::string(4096,char('A'+i))}});}));
    for(auto& f:writers)f.get();const auto shared=holo::read_json(root/"shared.json");
    check(shared.at("payload")==std::string(4096,char('A'+shared.at("writer").get<unsigned>())),"Concurrent publish corrupted snapshot");
    for(unsigned i=0;i<4;++i)fs::remove(root/(std::to_string(i)+".json"));
    for(const auto* name:{"bad.json","locked.json","child.done","child.log","shared.json"})fs::remove(root/name);
    check(fs::is_empty(root),"Atomic temporary files leaked");fs::remove(root);
    std::cout<<"PASS: 800 atomic publishes, "<<reads<<" consistent reads, sharing retry, strict JSON, failed commits, child survives callback exceptions and same-path publishers\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
