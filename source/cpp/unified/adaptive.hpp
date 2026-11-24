#pragma once
#include "engine.hpp"
namespace unified {
  json assess(const json& a,const json& b,const json& cfg,const std::string& left,const std::string& right) {
    const auto& ac=cfg.at("adaptive_qp");json requests=json::array(),domains=json::object();bool enough=true;
    auto request=[&](const char* method,int direction,const std::string& reason){requests.push_back({{"method",std::string(method)=="baseline"?left:right},{"direction",direction},{"reason",reason}});};
    for(const auto& metric:std::array<std::string,3>{"reconstruction_psnr","reconstruction_ssim","log_rate"}){
      if(metric=="reconstruction_ssim"&&!cfg.at("metrics").at("ssim").get<bool>())continue;
      std::array<std::vector<double>,2> v;for(unsigned k=0;k<2;++k)for(const auto& r:k?b:a){auto q=quality(r,metric);if(q.is_number()&&std::isfinite(q.get<double>()))v[k].push_back(q.get<double>());}
      bool count=true;for(unsigned k=0;k<2;++k){std::sort(v[k].begin(),v[k].end());v[k].erase(std::unique(v[k].begin(),v[k].end()),v[k].end());if(v[k].size()<ac.at("minimum_points_per_curve").get<unsigned>()){count=false;request(k?"candidate":"baseline",0,metric+": fewer than minimum finite points");}}
      json d={{"baseline_finite_points",v[0].size()},{"candidate_finite_points",v[1].size()},{"sufficient",false}};
      if(v[0].empty()||v[1].empty()){domains[metric]=d;enough=false;continue;}
      const double lo0=v[0].front(),hi0=v[0].back(),lo1=v[1].front(),hi1=v[1].back();
      const double span=std::max(0.0,std::min(hi0,hi1)-std::max(lo0,lo1)),fraction=ac.at("minimum_overlap_fraction");
      const double absolute=metric=="reconstruction_psnr"?ac.at("minimum_reconstruction_psnr_overlap_db").get<double>():metric=="reconstruction_ssim"?ac.at("minimum_reconstruction_ssim_overlap").get<double>():std::log(ac.at("minimum_rate_ratio").get<double>());
      const double required=std::max(absolute,fraction*std::min(hi0-lo0,hi1-lo1));const bool covered=span>=required&&span>0;
      d.update({{"baseline_range",{lo0,hi0}},{"ours_range",{lo1,hi1}},{"common_range",{std::max(lo0,lo1),std::min(hi0,hi1)}},{"common_span",span},{"required_span",required},{"sufficient",count&&covered}});
      if(!covered){
        const auto reason=metric+": overlap "+std::to_string(span)+" < required "+std::to_string(required);
        if(lo0>lo1)request("baseline",1,reason+"; extend lower quality/rate");
        if(lo1>lo0)request("candidate",1,reason+"; extend lower quality/rate");
        if(hi0<hi1)request("baseline",-1,reason+"; extend higher quality/rate");
        if(hi1<hi0)request("candidate",-1,reason+"; extend higher quality/rate");
        // If both curves cover the same narrow interval, extend both ends.
        if(lo0==lo1&&hi0==hi1){request("baseline",1,reason);request("candidate",1,reason);request("baseline",-1,reason);request("candidate",-1,reason);}
      }
      enough=enough&&count&&covered;domains[metric]=d;
    }
    return {{"sufficient",enough},{"domains",domains},{"requests",requests},{"overlap_fraction_definition","common span / smaller individual span; rate evaluated in natural-log domain"}};
  }
  inline unsigned extraCount(const json& cfg,const json& plan,const std::string& seq,const std::string& method) {
    unsigned n=0;const auto originalQps=cfg.at("qp_lists").at(method).get<std::vector<int>>();for(const auto& q:plan.at("qps").at(seq).at(method))if(std::find(originalQps.begin(),originalQps.end(),q.get<int>())==originalQps.end())++n;return n;
  }
  inline json nextQp(const json& cfg,const json& plan,const std::string& seq,const json& assessment) {
    const auto& ac=cfg.at("adaptive_qp");auto requests=assessment.at("requests").get<std::vector<json>>();
    // Give a less sampled method priority without abandoning the requested edge.
    std::stable_sort(requests.begin(),requests.end(),[&](const json& a,const json& b){return extraCount(cfg,plan,seq,a.at("method"))<extraCount(cfg,plan,seq,b.at("method"));});
    for(const auto& request:requests){const auto method=request.at("method").get<std::string>();const auto& limit=ac.at("maximum_extra_points_per_method");if(!limit.is_null()&&extraCount(cfg,plan,seq,method)>=limit.get<unsigned>())continue;
      auto qps=plan.at("qps").at(seq).at(method).get<std::vector<int>>();std::sort(qps.begin(),qps.end());const int low=ac.at("qp_min"),high=ac.at("qp_max"),step=ac.at("step"),direction=request.at("direction");int candidate=-1;
      if(direction>0)candidate=std::min(high,qps.back()+step);
      else if(direction<0)candidate=std::max(low,qps.front()-step);
      else {int gap=1;for(size_t i=1;i<qps.size();++i){int a=std::max(low,qps[i-1]),b=std::min(high,qps[i]);if(b-a>gap){gap=b-a;candidate=a+(b-a)/2;}}if(candidate<0&&qps.back()<high)candidate=std::min(high,qps.back()+step);if(candidate<0&&qps.front()>low)candidate=std::max(low,qps.front()-step);}
      if(candidate<low||candidate>high||std::find(qps.begin(),qps.end(),candidate)!=qps.end())continue;
      auto answer=request;answer["qp"]=candidate;return answer;
    }return nullptr;
  }
}
