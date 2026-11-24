#pragma once
#include "../hm/study_runtime.hpp"
#include "../hm/study_metrics.hpp"
#include <functional>
namespace unified {
using namespace study;
const std::array<std::string,3> channels={"R","G","B"};
std::string escape(std::string s){std::string o;for(char c:s){if(c=='&')o+="&amp;";else if(c=='<')o+="&lt;";else if(c=='>')o+="&gt;";else if(c=='\"')o+="&quot;";else o+=c;}return o;}
std::string value(const json& j){return j.is_null()?"":j.is_string()?j.get<std::string>():j.dump();}
fs::path original(const fs::path& folder,unsigned frame){std::ostringstream s;s<<"frame_"<<std::setw(6)<<std::setfill('0')<<frame<<".holo";return folder/s.str();}
holo::OpticalConfig optics(const json& j){holo::OpticalConfig o;o.pitch_x=j.at("pixel_pitch_x_m");o.pitch_y=j.at("pixel_pitch_y_m");o.field_scale=j.at("field_scale_for_reconstruction_only");o.wavelengths=j.at("wavelengths_m").get<std::vector<double>>();o.plane_z=j.at("reconstruction_plane_z_m").get<std::vector<double>>();o.propagation_sign=j.at("propagation_distance_sign");o.padding_factor=j.at("padding_factor");o.bandlimit=j.at("phase_gradient_bandlimit");o.threads=j.at("threads");o.cache_mib=j.at("transfer_cache_mib");check(o.wavelengths.size()==3&&!o.plane_z.empty()&&o.field_scale>0,"Invalid optics");return o;}

struct Evaluation {
 fs::path root,out,sourceRoot; json cfg; holo::OpticalConfig optical; unsigned width,height,frames,first; std::map<std::string,json> prepared; std::function<void(const json&)> progress;
 void tick(const json& j) {if(progress)advisoryProgress([&]{progress(j);});}
 Evaluation(fs::path r, fs::path o, const json& c, const std::string& dataset):root(r),out(o),sourceRoot(r/"data/holo"/dataset),cfg(c),width(c.at("width")),height(c.at("height")),frames(c.at("frames")),first(c.at("first_frame")) {auto recon=holo::read_json(r/"config"/(dataset+".json")); recon["optics"]["threads"]=c.at("resources").at("reconstruction_threads"); recon["optics"]["transfer_cache_mib"]=c.at("resources").at("transfer_cache_mib"); optical=optics(recon.at("optics"));}
  json prepare(const std::string& seq){tick({{"sequence",seq},{"stage","checking originals and mapping"}});const auto folder=sourceRoot/seq;json sources=json::array();std::array<double,3> scale{};unsigned bits=0;
    for(unsigned f=0;f<frames;++f){auto p=original(folder,first+f);auto data=holo::read_holo(p);check(data.width==width&&data.height==height&&data.channels==3&&data.frame_index==first+f,"Source dimensions/frame differ");if(!f)bits=data.scalar_bits;check(bits==data.scalar_bits,"Mixed source precision");sources.push_back(fileInfo(p));for(unsigned c=0;c<3;++c)for(size_t i=0;i<data.pixels();++i){const auto j=c*data.pixels()+i;check(std::isfinite(data.real[j])&&std::isfinite(data.imag[j]),"Non-finite original sample");scale[c]=std::max(scale[c],std::hypot(data.real[j],data.imag[j]));}tick({{"stage","checking originals"},{"frame",f+1},{"frames",frames}});}
    for(auto& a:scale)if(a==0)a=1;
    for(auto& a:scale){a=std::nextafter(a,std::numeric_limits<double>::infinity());check(std::isfinite(a)&&a>0,"Invalid amplitude bound");}
    const auto cache=out/"prepared"/seq;fs::create_directories(cache);json mapping={{"schema_version",2},{"amplitude_basis","complex_magnitude"},{"width",width},{"height",height},{"frames",frames},{"first_frame",first},{"amplitude",scale},{"source_scalar_bits",bits},{"sources",sources},{"forward","clip(floor((v/A+1)*127.5+0.5),0,255)"},{"backward","(2*u-255)*A/255"},{"originals_unmapped",true}};
    const auto id=holo::fingerprint(mapping.dump());bool reuse=false;
    if(fs::exists(cache/"prepared.json")){auto old=holo::read_json(cache/"prepared.json");reuse=old.value("source_id","")==id&&old.contains("required_metadata")&&fs::is_regular_file(cache/"mapping.bin")&&sha256(cache/"mapping.bin")==old.at("required_metadata").at("sha256");for(unsigned c=0;c<3&&reuse;++c){auto p=cache/(channels[c]+".yuv");reuse=fs::is_regular_file(p)&&old.contains("inputs")&&sha256(p)==old.at("inputs").at(c).at("sha256");}if(reuse){prepared[seq]=old;return old;}}
    check(fs::space(out).available>uint64_t(cfg.at("storage").at("minimum_free_gib").get<double>()*1024*1024*1024),"Insufficient free disk space");
    std::array<std::ofstream,3> outputs;for(unsigned c=0;c<3;++c){outputs[c].open(cache/(channels[c]+".yuv.partial"),std::ios::binary|std::ios::trunc);check(bool(outputs[c]),"Cannot create prepared input");}
    std::vector<unsigned char> packed(size_t(width)*height*3,0);
    for(unsigned f=0;f<frames;++f){auto data=holo::read_holo(original(folder,first+f));const auto n=data.pixels();for(unsigned c=0;c<3;++c){for(size_t i=0;i<n;++i){packed[i]=static_cast<unsigned char>(std::clamp(std::floor((data.real[c*n+i]/scale[c]+1)*127.5+.5),0.0,255.0));packed[n+i]=static_cast<unsigned char>(std::clamp(std::floor((data.imag[c*n+i]/scale[c]+1)*127.5+.5),0.0,255.0));}outputs[c].write(reinterpret_cast<const char*>(packed.data()),std::streamsize(packed.size()));}tick({{"stage","mapping selected frames"},{"frame",f+1}});}
    json inputs=json::array();for(unsigned c=0;c<3;++c){outputs[c].close();check(bool(outputs[c]),"Prepared input write failed");const auto target=cache/(channels[c]+".yuv");check(MoveFileExW((cache/(channels[c]+".yuv.partial")).c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"Cannot commit input");inputs.push_back(fileInfo(target));}
    for(const auto& s:sources)check(sha256(fs::u8path(s.at("path").get<std::string>()))==s.at("sha256"),"Original changed during preparation");
    // Required decoding metadata: 8 magic + six uint32 + three float64 = 56 bytes.
    std::ofstream metadata(cache/"mapping.bin",std::ios::binary|std::ios::trunc);const char magic[8]={'P','S','M','A','P','0','0','1'};metadata.write(magic,8);const std::array<uint32_t,6> fields={1,width,height,frames,3,first};metadata.write(reinterpret_cast<const char*>(fields.data()),24);metadata.write(reinterpret_cast<const char*>(scale.data()),24);metadata.close();check(bool(metadata)&&fs::file_size(cache/"mapping.bin")==56,"Mapping metadata write failed");
    mapping["source_id"]=id;mapping["inputs"]=inputs;mapping["required_metadata"]=fileInfo(cache/"mapping.bin");atomicJson(cache/"prepared.json",mapping);prepared[seq]=mapping;return mapping;}
  json evaluate(const std::string& seq,const json& mapping,const fs::path& attempt){const size_t pixels=size_t(width)*height;std::array<std::ifstream,3> input,decoded;for(unsigned c=0;c<3;++c){input[c].open(fs::u8path(mapping.at("inputs").at(c).at("path").get<std::string>()),std::ios::binary);decoded[c].open(attempt/channels[c]/"decoder.yuv",std::ios::binary);check(bool(input[c])&&bool(decoded[c]),"Missing evaluation input");}
    holo::Propagator prop(width,height,optical);ImageStats mappedTotal,reconTotal,rawTotal;std::array<ImageStats,2> mappedComponents{};std::array<ImageStats,3> mappedByChannel{},reconByChannel{};std::array<std::array<ImageStats,2>,3> mappedChannelComponents{};std::vector<ImageStats> depthTotal(optical.plane_z.size());ComplexStats complexTotal;std::array<ComplexStats,3> complexChannel{};
    json frameRows=json::array(),detail=json::array();std::ostringstream csv;csv<<"frame,channel,depth_index,z_m,amplitude_psnr_db,amplitude_ssim,unclipped_snr_db,reference_clipped_fraction,test_clipped_fraction\n";
    const bool useSsim=cfg.at("metrics").at("ssim");std::vector<unsigned char> packed(pixels*3),reco(pixels*3);
    const bool savePng=cfg.at("metrics").value("save_reconstruction_png",false);
    const auto pngDirectory=attempt/"reconstruction_png";
    json png={{"enabled",savePng},{"directory",pngDirectory.u8string()},{"expected_count",savePng?frames*optical.plane_z.size():0},{"count",0},{"complete",false},{"clip_low",0},{"clip_high",1},{"gamma",1},{"files",json::array()}};
    std::ostringstream pngRows;
    for(unsigned f=0;f<frames;++f){auto orig=holo::read_holo(original(sourceRoot/seq,first+f));ImageStats frameRecon,frameMapped;ComplexStats frameComplex;std::vector<float> preview;if(cfg.at("metrics").at("save_preview").get<bool>()&&f==cfg.at("metrics").at("preview_frame").get<unsigned>())preview.resize(pixels*3);
      // One frame of depth amplitudes in memory; no .amp depth arrays on disk.
      std::vector<std::vector<float>> rgbPlanes;if(savePng)rgbPlanes.assign(optical.plane_z.size(),std::vector<float>(pixels*3));
      for(unsigned c=0;c<3;++c){input[c].read(reinterpret_cast<char*>(packed.data()),std::streamsize(packed.size()));decoded[c].read(reinterpret_cast<char*>(reco.data()),std::streamsize(reco.size()));check(input[c].gcount()==std::streamsize(packed.size())&&decoded[c].gcount()==std::streamsize(reco.size()),"Short reconstructed frame");
        for(unsigned plane=0;plane<2;++plane){std::vector<float> a(pixels),b(pixels);for(size_t i=0;i<pixels;++i){a[i]=packed[plane*pixels+i];b[i]=float(reco[plane*pixels+i]);}const auto m=imageMetrics(a,b,width,height,255,useSsim);mappedTotal+=m;mappedByChannel[c]+=m;frameMapped+=m;mappedComponents[plane]+=m;mappedChannelComponents[c][plane]+=m;}
        const double amplitude=mapping.at("amplitude").at(c);std::vector<holo::Complex> a(pixels),b(pixels);for(size_t i=0;i<pixels;++i){a[i]={orig.real[c*pixels+i],orig.imag[c*pixels+i]};b[i]=holo::Complex((2*int(reco[i])-255)*amplitude/255,(2*int(reco[pixels+i])-255)*amplitude/255);}
        const auto cm=complexMetrics(a,b,2*amplitude,amplitude*cfg.at("metrics").at("phase_amplitude_threshold_relative").get<double>());complexTotal+=cm;complexChannel[c]+=cm;frameComplex+=cm;
        for(size_t i=0;i<pixels;++i){a[i]*=optical.field_scale;b[i]*=optical.field_scale;}const auto sa=prop.spectrum(a),sb=prop.spectrum(b);a.clear();b.clear();
        for(size_t d=0;d<optical.plane_z.size();++d){tick({{"stage","numerical reconstruction and metrics"},{"frame",f+1},{"frames",frames},{"physical_channel",channels[c]},{"depth_index",d},{"depths",optical.plane_z.size()}});
          auto ra=prop.propagate(sa,optical.wavelengths[c],optical.plane_z[d]*optical.propagation_sign),rb=prop.propagate(sb,optical.wavelengths[c],optical.plane_z[d]*optical.propagation_sign);
          std::vector<float> aa(pixels),bb(pixels);uint64_t ca=0,cb=0;for(size_t i=0;i<pixels;++i){aa[i]=float(std::abs(ra[i]));bb[i]=float(std::abs(rb[i]));ca+=aa[i]>1;cb+=bb[i]>1;}auto raw=imageMetrics(aa,bb,width,height,1,false);rawTotal+=raw;
          for(size_t i=0;i<pixels;++i){aa[i]=std::clamp(aa[i],0.f,1.f);bb[i]=std::clamp(bb[i],0.f,1.f);}auto m=imageMetrics(aa,bb,width,height,1,useSsim);m.clipped_reference=ca;m.clipped_test=cb;reconTotal+=m;reconByChannel[c]+=m;depthTotal[d]+=m;frameRecon+=m;
          if(!preview.empty()&&d==cfg.at("metrics").at("preview_depth_index").get<unsigned>())std::copy(bb.begin(),bb.end(),preview.begin()+c*pixels);
          if(savePng)std::copy(bb.begin(),bb.end(),rgbPlanes[d].begin()+c*pixels);
          auto j=m.json(1),rawj=raw.json(1);detail.push_back({{"frame",f},{"channel",channels[c]},{"depth_index",d},{"z_m",optical.plane_z[d]},{"amplitude",j},{"unclipped",rawj}});
          csv<<f<<','<<channels[c]<<','<<d<<','<<std::setprecision(17)<<optical.plane_z[d]<<','<<value(j.at("psnr_db"))<<','<<value(j.at("ssim"))<<','<<value(rawj.at("snr_db"))<<','<<value(j.at("reference_clipped_fraction"))<<','<<value(j.at("test_clipped_fraction"))<<'\n';}
      }
      if(!preview.empty())holo::write_rgb_png(attempt/"preview.png",width,height,preview,0,1);
      if(savePng){
        check(fs::space(out).available>uint64_t(cfg.at("storage").at("minimum_free_gib").get<double>()*1024*1024*1024),"Insufficient free disk space for reconstruction PNGs");
        std::ostringstream folder;folder<<"frame_"<<std::setw(6)<<std::setfill('0')<<first+f;
        pngRows<<"<tr><th>"<<first+f<<"</th>";
        for(size_t d=0;d<rgbPlanes.size();++d){
          std::ostringstream filename;filename<<"z_"<<std::setw(2)<<std::setfill('0')<<d<<".png";
          const auto relative=fs::path(folder.str())/filename.str(),path=pngDirectory/relative;
          tick({{"stage","saving reconstruction PNG"},{"frame",f+1},{"depth_index",d},{"png_count",png.at("count")},{"png_expected_count",png.at("expected_count")}});
          holo::write_rgb_png(path,width,height,rgbPlanes[d],0,1);
          png["files"].push_back({{"frame",first+f},{"depth_index",d},{"z_m",optical.plane_z[d]},{"file",relative.generic_u8string()},{"bytes",fs::file_size(path)}});
          png["count"]=png.at("files").size();
          pngRows<<"<td><a href='"<<relative.generic_u8string()<<"'><img loading=lazy width=160 src='"<<relative.generic_u8string()<<"'></a></td>";
        }
        pngRows<<"</tr>";png["complete"]=f+1==frames;atomicJson(pngDirectory/"index.json",png);
        std::ostringstream gallery;gallery<<"<!doctype html><html lang=ko><meta charset=utf-8><title>HM reconstruction PNG</title><style>body{font:16px system-ui}table{border-collapse:collapse}th,td{padding:6px;border:1px solid #ddd}img{height:auto}</style><h1>"<<escape(seq)<<" RGB reconstruction</h1><p>"<<escape(attempt.parent_path().parent_path().filename().u8string())<<" / "<<escape(attempt.parent_path().filename().u8string())<<" / clip [0,1], gamma 1 / "<<png.at("count")<<" images</p><table><tr><th>frame</th>";
        for(size_t d=0;d<optical.plane_z.size();++d)gallery<<"<th>z_"<<d<<" ("<<optical.plane_z[d]*1000<<" mm)</th>";
        gallery<<"</tr>"<<pngRows.str()<<"</table></html>";atomicText(pngDirectory/"index.html",gallery.str());
      }
      frameRows.push_back({{"frame",f},{"mapped",frameMapped.json(255)},{"complex",frameComplex.json()},{"reconstruction",frameRecon.json(1)}});
      atomicText(attempt/"depth_metrics.csv",csv.str());atomicJson(attempt/"metrics_progress.json",{{"complete",false},{"frames_done",f+1},{"frames_total",frames},{"png_count",png.at("count")},{"png_expected_count",png.at("expected_count")},{"reconstruction_so_far",reconTotal.json(1)},{"frames",frameRows}});
    }
    json physical=json::object(),depths=json::array();for(unsigned c=0;c<3;++c)physical[channels[c]]={{"mapped",mappedByChannel[c].json(255)},{"mapped_real",mappedChannelComponents[c][0].json(255)},{"mapped_imag",mappedChannelComponents[c][1].json(255)},{"complex",complexChannel[c].json()},{"reconstruction",reconByChannel[c].json(1)}};
    for(size_t d=0;d<depthTotal.size();++d)depths.push_back({{"depth_index",d},{"z_m",optical.plane_z[d]},{"metrics",depthTotal[d].json(1)}});
    json result={{"mapped",mappedTotal.json(255)},{"complex",complexTotal.json()},{"reconstruction",reconTotal.json(1)},{"unclipped_reconstruction",rawTotal.json(1)},{"physical_channels",physical},{"depths",depths},{"frames",frameRows},{"frame_channel_depth",detail},
      {"definitions",{{"psnr","pooled SSE, mapped peak255; reconstructed amplitude peak1 after shared clip[0,1]"},{"ssim","11x11 Gaussian sigma1.5, population covariance, valid windows, K1=.01 K2=.03, no downsampling"},{"complex","unmapped Original vs inverse-mapped final 8-bit Real/Imag for both methods; direct-v5 final Q20 reconstruction equals these decoded samples; phase weighted by Original energy"},{"null_values","undefined or infinite; inspect perfect_match/SSE before interpreting"},{"depth_amplitude_arrays_saved",false},{"temporary_complex_decoder_output",false}}}};
    result["mapped_real"]=mappedComponents[0].json(255);result["mapped_imag"]=mappedComponents[1].json(255);
    result["reconstruction_png"]=png;
    atomicJson(attempt/"metrics.json",result);atomicJson(attempt/"metrics_progress.json",{{"complete",true},{"frames_done",frames},{"frames_total",frames},{"png_count",png.at("count")},{"png_expected_count",png.at("expected_count")}});return result;}

};
}
