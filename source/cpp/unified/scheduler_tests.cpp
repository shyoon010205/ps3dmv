#define RA_SUITE_EMBEDDED
#include "ra_suite.cpp"
int main(int argc,char** argv){
 fs::path temp;try{
   check(argc==2,"Workspace argument required");const auto root=fs::u8path(argv[1]);auto cfg=holo::read_json(root/"config/suite.json");
   cfg["output_root"]="results/_scheduler_selftest_"+std::to_string(GetCurrentProcessId());Engine e(root,cfg);temp=e.out;check(!fs::exists(temp),"Test path already exists");fs::create_directories(temp);
   e.cfg["retry"]["backoff_seconds"]=0;
   std::atomic<unsigned> concurrent{0},peak{0},done{0};std::vector<Task> work;
   for(unsigned i=0;i<40;++i)work.push_back({std::to_string(i),temp/"progress.json",[&]{const unsigned now=++concurrent;unsigned old=peak;while(old<now&&!peak.compare_exchange_weak(old,now)){}std::this_thread::sleep_for(std::chrono::milliseconds(60));--concurrent;++done;}});
   check(queue(e,work,16,"selftest encode queue"),"Queue unexpectedly stopped");check(done==40&&peak<=16&&peak>1,"Global 16-slot queue failed");
   work.resize(6);peak=0;done=0;check(queue(e,work,2,"selftest evaluation queue"),"Evaluation queue stopped");check(done==6&&peak==2,"Two-worker evaluation cap failed");
   const auto groups=selectedGroups(e,"");const auto codecs=validationGroups(e,groups);
   check(groups.size()==4&&codecs.size()==2&&codecs[0].at("id")=="hm"&&codecs[1].at("id")=="vvenc","Dataset validation was not deduplicated by codec");
   check(validationGroup(e,groups[0])==validationGroup(e,groups[1])&&validationGroup(e,groups[2])==validationGroup(e,groups[3]),"Datasets do not share their codec gate");
   check(validationGroups(e,{groups[3]}).at(0).at("dataset")==cfg.at("validation").at("dataset"),"Single-dataset selection changed validation input");
   work.resize(2);peak=0;done=0;check(queue(e,work,2,"selftest parallel codec validation",{},true),"Validation queue stopped");check(done==2&&peak==2,"Two codec validations did not run concurrently");
   unsigned validationAttempts=0,validationPending=0;bool validationRejected=false;
   try{queue(e,{{"invalid",temp/"validation-fail/progress.json",[&]{++validationAttempts;throw std::runtime_error("injected validation failure");}},
                {"pending",temp/"validation-pending/progress.json",[&]{++validationPending;}}},1,"selftest validation failure",{},true);}catch(...){validationRejected=true;}
   check(validationRejected&&validationAttempts==1&&validationPending==0,"Validation retried or launched queued work after a failure");
   unsigned experimentCalls=0,gateCalls=0;bool barrierRejected=false;
   check(!validationThenExperiments(true,true,[]{return false;},[&]{++gateCalls;return true;},[&]{++experimentCalls;return true;}),"Stopped validation passed the barrier");
   check(gateCalls==0&&experimentCalls==0,"Experiment ran after incomplete validation");
   try{validationThenExperiments(false,true,[]{return true;},[&]{for(unsigned i=0;i<2;++i){++gateCalls;if(i==1)throw std::runtime_error("second codec lacks PASS");}return true;},[&]{++experimentCalls;return true;});}catch(...){barrierRejected=true;}
   check(barrierRejected&&gateCalls==2&&experimentCalls==0,"RunOnly began experiments before every codec gate passed");
   std::string order;check(validationThenExperiments(true,true,[&]{order+='V';return true;},[&]{order+='G';return true;},[&]{order+='E';return true;})&&order=="VGE","Validation/experiment stage order differs");
   json quick={{"group",cfg.at("groups").at(0)},{"method",e.methods.at("baseline")},{"width",256},{"height",256},{"frames",3}};
   check(e.overlay(quick).find("FramesToBeEncoded : 3\n")!=std::string::npos,"Quick validation still encodes 16 frames");
   quick["frames"]=16;check(e.overlay(quick).find("FramesToBeEncoded : 16\n")!=std::string::npos,"Full experiment frame count changed");
   const auto raw=temp/"three_frames.yuv";atomicText(raw,std::string(size_t(256)*256*3*3,'\0'));
   vvcvalidation::Condition shortCondition{256,256,3,8,8,30};
   {vvcvalidation::Frames shortFrames(raw,shortCondition,false);
     for(unsigned f=0;f<3;++f)check(shortFrames.read().size()==size_t(256)*256*3,"Short reconstruction frame size differs");shortFrames.finish();}
   bool wrongCountRejected=false;try{auto longCondition=shortCondition;longCondition.frames=16;vvcvalidation::Frames wrongFrames(raw,longCondition,false);}catch(...){wrongCountRejected=true;}
   check(wrongCountRejected,"Three decoded frames were accepted as sixteen");
    json ep=json::array({{{"poc",0},{"area_pixels",100},{"views",{{"inter_direction",{{"shared",{{"L0",20},{"L1",10},{"BI",50},{"NOT_INTER",20}}}}},{"prediction",{{"shared",{{"INTER",80},{"INTRA",20}}}}}}}}});
    auto dp=ep;dp[0]["views"]["inter_direction"]["shared"]={{"L0",15},{"L1",5},{"BI",60},{"NOT_INTER",20}};
     check(vvcarea::stablePicturesEqual(ep,dp),"Equivalent VVenC/VVdeC inter-direction redistribution was rejected");
    dp[0]["views"]["inter_direction"]["shared"]["NOT_INTER"]=21;dp[0]["views"]["inter_direction"]["shared"]["BI"]=59;
    check(!vvcarea::stablePicturesEqual(ep,dp),"Different Inter/Not-Inter area was accepted");
    dp=ep;dp[0]["views"]["prediction"]["shared"]["INTER"]=79;
    check(!vvcarea::stablePicturesEqual(ep,dp),"Different stable mode area was accepted");
   unsigned attempts=0;Task retry{"recoverable",temp/"retry/progress.json",[&]{if(++attempts<3)throw std::runtime_error("injected transient failure");}};
   check(queue(e,{retry},1,"selftest retry")&&attempts==3,"Bounded retry did not recover");
   unsigned exhausted=0;bool rejected=false;try{queue(e,{{"persistent",temp/"failure/progress.json",[&]{++exhausted;throw std::runtime_error("injected persistent failure");}}},1,"selftest exhausted");}catch(...){rejected=true;}
   check(rejected&&exhausted==3,"Persistent failure did not stop at three attempts");
   stopRequested=false;unsigned launched=0;Task stop{"stop",temp/"stop/progress.json",[&]{++launched;stopRequested=true;}};
   check(!queue(e,{stop,stop,stop},1,"selftest stop")&&launched==1,"Stop launched work after drain request");stopRequested=false;
   const auto a=std::vector<Point>{{100,10},{200,20},{400,30},{800,40}},b=std::vector<Point>{{90,10},{180,20},{360,30},{720,40}};
   auto bd=bdComparison(a,b,4);check(bd.at("status")=="VALID"&&std::abs(bd.at("bd_rate_percent").get<double>()+10)<1e-10,"Known 10-percent BD saving differs");
   std::vector<Point> noOverlap{{900,50},{1000,60},{1100,70},{1200,80}};check(bdComparison(a,noOverlap,4).at("status")=="PARTIAL_OR_NO_OVERLAP","BD extrapolated");
   auto ac=cfg;ac["adaptive_qp"]["qp_min"]=0;ac["adaptive_qp"]["qp_max"]=51;
   json plan={{"qps",{{"test",cfg.at("qp_lists")}}}};auto request=nextQp(ac,plan,"test",{{"requests",json::array({{{"method","ps3dmvp"},{"direction",-1},{"reason","insufficient overlap"}}})}});
   check(request.at("qp")==5&&request.at("method")=="ps3dmvp","Adaptive QP did not extend the requested curve");
   json display={{"state","ERROR"},{"error","<unsafe & error>"},{"completed",3},{"total",4},{"slots",2},{"active",json::array()}};
   auto html=dashboardHtml(temp,cfg,display,false);
   check(html.find("charset=\"utf-8\"")!=std::string::npos&&html.find("Codec validation")!=std::string::npos,"Dashboard encoding/header missing");
   check(html.find("&lt;unsafe &amp; error&gt;")!=std::string::npos&&html.find("<strong>3 / 4</strong>")!=std::string::npos,"Dashboard error escaping or signed counts failed");
   const auto firstGroup=cfg.at("groups").at(0).at("id").get<std::string>();
   check(html.find("href=\""+firstGroup+"/results.html\"")==std::string::npos,"Dashboard links to a missing result");
   atomicText(temp/firstGroup/"results.html","result");html=dashboardHtml(temp,cfg,display,false);
   check(html.find("href=\""+firstGroup+"/results.html\"")!=std::string::npos,"Existing result link missing");
   display["state"]="VALIDATING";check(dashboardHtml(temp,cfg,display,false).find("INTERRUPTED")!=std::string::npos,"Dead runner remains displayed as active");
   status(e,{{"state","ERROR"},{"error","dashboard refresh test"}});
   {std::ifstream page(temp/"status.html");std::string rendered((std::istreambuf_iterator<char>(page)),{});check(rendered.find("dashboard refresh test")!=std::string::npos,"Error snapshot did not refresh dashboard");}
   check(within(temp,root/"results")&&temp.filename().u8string().rfind("_scheduler_selftest_",0)==0,"Unsafe test cleanup");fs::remove_all(temp);
   std::cout<<"PASS: concurrency 16/2/2, codec-shared validation and barrier, 3/16 frame separation, retry/recovery/exhaustion, drain/resume, analytic BD and dashboard rendering. No codec or dataset reconstruction executed.\n";return 0;
 }catch(const std::exception& err){stopRequested=false;std::cerr<<err.what()<<"; test evidence: "<<temp.u8string()<<'\n';return 1;}
}
