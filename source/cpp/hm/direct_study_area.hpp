#pragma once
#include "mode_area_report.hpp"
namespace directstudy {
using namespace hmvalidation;
inline json poolAreas(const json& channels) {
  json out={{"scopes",json::object()},{"aggregation","Sum of valid spatial areas across physical R/G/B channels; Real/Imag counted once per complex sample"}};
  check(channels.size()==3,"Expected physical R/G/B area maps");
  for(const auto* scope:{"all_frames","inter_frames"}) {
    auto& pooled=out["scopes"][scope];pooled={{"area_pixels",uint64_t(0)},{"views",json::object()}};
    for(const auto& channel:channels.items()) {
      const auto& src=channel.value().at("scopes").at(scope);
      pooled["area_pixels"]=pooled.at("area_pixels").get<uint64_t>()+src.at("area_pixels").get<uint64_t>();
      for(const auto& view:src.at("views").items())for(const auto& component:view.value().items())for(const auto& module:component.value().items()) {
        auto& target=pooled["views"][view.key()][component.key()][module.key()];target=(target.is_null()?0:target.get<uint64_t>())+module.value().get<uint64_t>();
      }
    }
    for(const auto& view:pooled.at("views"))for(const auto& component:view){uint64_t sum=0;for(const auto& count:component)sum+=count.get<uint64_t>();check(sum==pooled.at("area_pixels").get<uint64_t>(),"Pooled module area mismatch");}
  }
  const auto all=out.at("scopes").at("all_frames").at("area_pixels").get<uint64_t>(),eligible=out.at("scopes").at("inter_frames").at("area_pixels").get<uint64_t>();
  const auto ps=out.at("scopes").at("all_frames").at("views").at("prediction").at("shared").at("PS").get<uint64_t>();
  check(ps<=eligible,"PS selected area exceeds eligible area");
  out["ps_selected_area_pixels"]=ps;out["ps_eligible_area_pixels"]=eligible;out["all_frame_area_pixels"]=all;
  out["ps_area_ratio"]=eligible?json(double(ps)/double(eligible)):json();out["ps_area_ratio_all_frames"]=all?json(double(ps)/double(all)):json();
  out["tool_notes"]={{"BDPCM","Not supported by HM; RDPCM is a separate view"},{"denominator","P/B pixels including native Intra CUs; secondary all_frames scope includes I pictures"}};
  return out;
}
inline void areaRows(std::ostringstream& csv,const std::string& sequence,const std::string& method,int qp,const std::string& physical,const json& areas) {
  for(const auto& scope:areas.at("scopes").items()) {
    const auto denominator=scope.value().at("area_pixels").get<uint64_t>();
    for(const auto& view:scope.value().at("views").items())for(const auto& component:view.value().items())for(const auto& module:component.value().items()) {
      const auto count=module.value().get<uint64_t>();csv<<sequence<<','<<method<<','<<qp<<','<<physical<<','<<scope.key()<<','<<view.key()<<','<<component.key()<<','<<module.key()<<','<<count<<','<<denominator<<',';
      if(denominator)csv<<std::setprecision(12)<<100.0*double(count)/double(denominator);csv<<'\n';
    }
  }
}
inline void compareAreas(std::ostringstream& csv,const std::string& sequence,int qp,const std::string& physical,const json& off,const json& on) {
  for(const auto& scope:off.at("scopes").items()) {
    const auto& b=on.at("scopes").at(scope.key());const auto denominator=scope.value().at("area_pixels").get<uint64_t>();
    check(denominator==b.at("area_pixels").get<uint64_t>(),"Off/On area denominators differ");
    for(const auto& view:scope.value().at("views").items())for(const auto& component:view.value().items())for(const auto& module:component.value().items()) {
      const auto av=module.value().get<uint64_t>(),bv=b.at("views").at(view.key()).at(component.key()).at(module.key()).get<uint64_t>();
      csv<<sequence<<','<<qp<<','<<physical<<','<<scope.key()<<','<<view.key()<<','<<component.key()<<','<<module.key()<<','<<av<<','<<bv<<','<<denominator<<',';
      if(denominator)csv<<std::setprecision(12)<<100.0*double(av)/double(denominator)<<','<<100.0*double(bv)/double(denominator)<<','<<100.0*(double(bv)-double(av))/double(denominator);
      else csv<<",,";csv<<'\n';
    }
  }
}
inline void predictionHtml(std::ostringstream& html,int qp,const json& off,const json& on) {
  const char* names[]={"PS","INTRA","INTER_AMVP","INTER_MERGE","INTER_SKIP","PCM"};
  const char* colors[]={"#52b788","#f4c95d","#5680c2","#7ac4cc","#aaa","#ae81ba"};
  html<<"<h2>QP "<<qp<<"</h2>";
  for(const auto* scope:{"all_frames","inter_frames"}) {
    const auto& a=off.at("scopes").at(scope);const auto& b=on.at("scopes").at(scope);const double total=a.at("area_pixels");
    html<<"<h3>"<<scope<<"</h3>";if(total==0)continue;
    for(unsigned method=0;method<2;++method) {
      const auto& counts=(method?b:a).at("views").at("prediction").at("shared");html<<"<p>"<<(method?"On":"Off")<<"</p><div class=bar>";
      for(unsigned i=0;i<6;++i){double p=100*counts.at(names[i]).get<double>()/total;html<<"<span style='background:"<<colors[i]<<";width:"<<p<<"%' title='"<<names[i]<<" "<<p<<"%'>"<<names[i]<<"</span>";}html<<"</div>";
    }
    html<<"<table><tr><th>Mode</th><th>Off %</th><th>On %</th><th>On-Off (pp)</th></tr>";
    for(const auto* name:names){double av=100*a.at("views").at("prediction").at("shared").at(name).get<double>()/total,bv=100*b.at("views").at("prediction").at("shared").at(name).get<double>()/total;html<<"<tr><td>"<<name<<"</td><td>"<<av<<"</td><td>"<<bv<<"</td><td>"<<bv-av<<"</td></tr>";}html<<"</table>";
  }
}
}
