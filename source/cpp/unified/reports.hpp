#pragma once
#include "adaptive.hpp"
namespace unified {
inline const std::array<std::pair<std::string,std::string>,3>& pairs(){static const std::array<std::pair<std::string,std::string>,3> p={{{"baseline","ps3dmvp"},{"baseline","ps3dmvp_theta"},{"ps3dmvp","ps3dmvp_theta"}}};return p;}
inline json rowsFor(Engine& e,const json& group,const json& plan,const std::string& seq,const std::string& method){
 json rows=json::array();for(const auto& q:plan.at("qps").at(seq).at(method)){
   auto r=readIf(e.trialDir(group,seq,method,q)/"result.json");
   if(!r.is_null()&&r.value("status","")=="COMPLETE")rows.push_back(r);
 }return rows;
}
inline json reports(Engine& e,const json& group,const json& plan,const json& config){
 const auto dir=e.groupDir(group);json report={{"group",group},{"suite_key",e.identity.at("key")},{"updated_local",timestamp()},{"sequences",json::object()},{"adaptive_history",plan.at("adaptive_history")}};
 std::ostringstream summary,bd,area,html;
 summary<<"sequence,method,theta_divisions,qp,total_bytes,anchor_bytes,ps_bytes,mapping_bytes,bpp_rgb,kbps,compression_ratio,reconstruction_psnr_db,reconstruction_ssim,reconstruction_mse,reconstruction_mae,complex_snr_db,complex_nmse,complex_correlation_abs,phase_rmse_rad,mapped_psnr_db,mapped_ssim,encode_seconds,decode_seconds,evaluation_seconds,ps_area_percent,ps_all_frame_area_percent\n";
 bd<<"sequence,reference,test,metric,status,bd_rate_percent,bd_quality_delta,coverage_sufficient\n";
 area<<"sequence,method,qp,physical_channel,scope,view,component,module,area_pixels,denominator_pixels,area_percent\n";
 html<<"<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=15><style>body{font:15px system-ui;margin:28px}table{border-collapse:collapse}td,th{border:1px solid #ddd;padding:8px}a{margin-right:15px}</style><h1>"<<escape(group.at("id").get<std::string>())<<"</h1><p><a href=summary.csv>Metrics CSV</a><a href=bd_results.csv>BD CSV</a><a href=mode_area.csv>Prediction-mode area CSV</a></p><table><tr><th>Sequence</th><th>Method / θ divisions</th><th>QP</th><th>Total bytes</th><th>Reconstruction PSNR</th><th>SSIM</th><th>PS area % (P/B)</th><th>PNG</th></tr>";
 unsigned done=0,planned=0;
 for(const auto& name:e.cfg.at("sequences")){
   const std::string seq=name;std::map<std::string,json> rows;
   for(const auto& method:e.methods){rows[method.first]=rowsFor(e,group,plan,seq,method.first);planned+=unsigned(plan.at("qps").at(seq).at(method.first).size());
     for(const auto& r:rows.at(method.first)){++done;const auto& rate=r.at("rate");const auto& m=r.at("metrics");const auto& recon=m.at("reconstruction");const auto& cm=m.at("complex");const auto& time=r.at("timing");const auto& ar=r.at("mode_area");
       summary<<seq<<','<<method.first<<','<<method.second.at("theta_divisions")<<','<<r.at("qp");
       for(const auto& item:std::vector<json>{rate.at("total_bytes"),rate.at("anchor_bytes"),rate.at("ps_bytes"),rate.at("mapping_bytes"),rate.at("bits_per_rgb_hologram_pixel"),rate.at("kbps"),rate.at("compression_ratio_original_payload"),recon.at("psnr_db"),recon.at("ssim"),recon.at("mse"),recon.at("mae"),cm.at("complex_snr_db"),cm.at("complex_nmse"),cm.at("complex_correlation_abs"),cm.at("phase_weighted_rmse_rad"),m.at("mapped").at("psnr_db"),m.at("mapped").at("ssim"),time.at("encode_seconds"),time.at("decode_seconds"),time.at("evaluation_seconds"),ar.at("ps_area_ratio").is_number()?json(100*ar.at("ps_area_ratio").get<double>()):json(),ar.at("ps_area_ratio_all_frames").is_number()?json(100*ar.at("ps_area_ratio_all_frames").get<double>()):json()})summary<<','<<value(item);summary<<'\n';
       html<<"<tr><td>"<<seq<<"</td><td>"<<method.first<<" / "<<method.second.at("theta_divisions")<<"</td><td>"<<r.at("qp")<<"</td><td>"<<rate.at("total_bytes")<<"</td><td>"<<value(recon.at("psnr_db"))<<"</td><td>"<<value(recon.at("ssim"))<<"</td><td>"<<value(ar.at("ps_area_ratio").is_number()?json(100*ar.at("ps_area_ratio").get<double>()):json())<<"</td><td><a href='"<<seq<<'/'<<method.first<<"_qp"<<r.at("qp")<<"/reconstruction_png/index.html'>160 images</a></td></tr>";
       for(const auto& scope:ar.at("scopes").items())for(const auto& view:scope.value().at("views").items())for(const auto& component:view.value().items()){
         if(component.key()=="Zero")continue;const double denominator=scope.value().at("area_pixels");
         for(const auto& mode:component.value().items())area<<seq<<','<<method.first<<','<<r.at("qp")<<",pooled,"<<scope.key()<<','<<view.key()<<','<<component.key()<<','<<mode.key()<<','<<mode.value()<<','<<denominator<<','<<(denominator?100*mode.value().get<double>()/denominator:0)<<'\n';
       }
     }
   }
   for(const auto& pair:pairs()){
     const auto coverage=assess(rows.at(pair.first),rows.at(pair.second),config,pair.first,pair.second);auto& dst=report["sequences"][seq][pair.first+"__"+pair.second];dst["coverage"]=coverage;
     for(const auto& metric:{"reconstruction_psnr","reconstruction_ssim","reconstruction_ssim_db","complex_snr","mapped_psnr"}){
       std::array<std::vector<Point>,2> points;unsigned k=0;for(const auto& method:{pair.first,pair.second}){for(const auto& r:rows.at(method)){auto q=quality(r,metric);if(q.is_number()&&std::isfinite(q.get<double>()))points[k].push_back({r.at("rate").at("bits_per_rgb_hologram_pixel"),q.get<double>()});}++k;}
       auto result=bdComparison(points[0],points[1],config.at("adaptive_qp").at("minimum_points_per_curve"));result["coverage_sufficient"]=coverage.at("sufficient");dst["metrics"][metric]=result;
       bd<<seq<<','<<pair.first<<','<<pair.second<<','<<metric<<','<<result.at("status")<<','<<value(result.value("bd_rate_percent",json()))<<','<<value(result.value("bd_quality_delta",json()))<<','<<coverage.at("sufficient")<<'\n';
     }
   }
 }
 report["completed_trials"]=done;report["planned_trials"]=planned;
 html<<"</table><p>"<<done<<" / "<<planned<<" conditions complete. Codec log PSNR is not the primary quality metric. Original numerical reconstruction is recomputed in floating point.</p>";
 atomicText(dir/"summary.csv",summary.str());atomicText(dir/"bd_results.csv",bd.str());atomicText(dir/"mode_area.csv",area.str());atomicJson(dir/"bd_results.json",report);atomicText(dir/"results.html",html.str());return report;
}
}
