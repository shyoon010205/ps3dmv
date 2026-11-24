#include "holo.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

using namespace holo;

namespace {
constexpr double pi=3.1415926535897932384626433832795;
struct Args { fs::path config; bool resume=false; unsigned max_frames=0; std::string mode="both"; };

Args args(int argc,char** argv) {
  Args a;
  for(int i=1;i<argc;++i) {
    const std::string s=argv[i];
    auto value=[&]() -> std::string {if(++i>=argc)throw std::runtime_error("Missing value for "+s);return argv[i];};
    if(s=="--config")a.config=value();
    else if(s=="--resume")a.resume=true;
    else if(s=="--max-frames")a.max_frames=unsigned(std::stoul(value()));
    else if(s=="--mode")a.mode=value();
    else throw std::runtime_error("Unknown argument: "+s);
  }
  if(a.config.empty())throw std::runtime_error("Usage: generate_sintel_holograms --config FILE [--resume] [--max-frames N] [--mode static|dynamic|both]");
  if(a.mode!="static"&&a.mode!="dynamic"&&a.mode!="both")throw std::runtime_error("Invalid --mode: "+a.mode);
  return a;
}

fs::path absolute(const fs::path& root,const fs::path& path) {return path.is_absolute()?path:root/path;}

std::vector<uint8_t> bytes(const fs::path& path,size_t expected) {
  std::ifstream input(path,std::ios::binary|std::ios::ate);
  if(!input)throw std::runtime_error("Cannot open "+path.string());
  if(size_t(input.tellg())!=expected)throw std::runtime_error("Unexpected size: "+path.string());
  std::vector<uint8_t> result(expected);input.seekg(0);input.read(reinterpret_cast<char*>(result.data()),std::streamsize(result.size()));
  if(!input)throw std::runtime_error("Incomplete read: "+path.string());return result;
}

uint64_t fnv(const std::string& value,uint64_t seed=14695981039346656037ull) {
  uint64_t h=seed;for(unsigned char c:value){h^=c;h*=1099511628211ull;}return h;
}
uint64_t splitmix(uint64_t x) {
  x+=0x9e3779b97f4a7c15ull;x=(x^(x>>30))*0xbf58476d1ce4e5b9ull;
  x=(x^(x>>27))*0x94d049bb133111ebull;return x^(x>>31);
}
double unit(uint64_t x) {return double(splitmix(x)>>11)*(1.0/9007199254740992.0);}

std::string frame_name(unsigned index,const char* extension) {
  std::ostringstream s;s<<"frame_"<<std::setw(6)<<std::setfill('0')<<index<<extension;return s.str();
}

json load(const fs::path& path) {std::ifstream s(path);if(!s)throw std::runtime_error("Cannot open "+path.string());json j;s>>j;return j;}

void publish_dataset(const fs::path& root,const std::string& id,const json& config,const json& manifest,const std::string& policy) {
  fs::create_directories(root);
  const std::string phase_model=config.at("phase").value("model",std::string("replacement_mask"));
  const std::string formula=phase_model=="static_plus_dynamic_perturbation"
    ?"sum_k ASM(O_k * exp(i*(static_mask + dynamic_perturbation)), -z_k); dynamic_perturbation is zero only for static mode; then apply the CGH-wide rotation exp(i*frame*pi/8)"
    :"sum_k ASM(O_k * exp(i*M), -z_k); then multiply exp(i*frame*pi/8)";
  write_json(root/"dataset.json",{{"schema_version",1},{"complete",false},{"dataset_id",id},
    {"format","PSHCF01"},{"source","MPI Sintel training clean RGB-D"},{"width",config.at("target_width")},
    {"height",config.at("target_height")},{"frames_per_sequence",config.at("frames")},{"sequences",config.at("sequences")},
    {"depth_mapping",manifest.at("depth_mapping")},{"object_plane_z_m",config.at("object_plane_z_m")},
    {"phase",config.at("phase")},{"random_mask_temporal_policy",policy},
    {"generation_formula",formula},
    {"stored_scale",config.at("optics").at("stored_field_scale")},{"physical_channels",{"R","G","B"}},
    {"wavelengths_m",config.at("optics").at("wavelengths_m")}});
}
}

int main(int argc,char** argv) try {
  const auto a=args(argc,argv);
  const auto config_path=fs::absolute(a.config);
  const auto root=config_path.parent_path().parent_path();
  const auto config=load(config_path),manifest=load(absolute(root,config.at("paths").at("manifest").get<std::string>()));
  if(!manifest.at("complete"))throw std::runtime_error("Prepared manifest is incomplete");
  const unsigned width=config.at("target_width"),height=config.at("target_height"),frame_count=config.at("frames");
  const size_t pixels=size_t(width)*height;
  const auto planes=config.at("object_plane_z_m").get<std::vector<double>>();
  if(planes.size()!=10)throw std::runtime_error("Expected ten object planes");
  OpticalConfig optical;const auto& o=config.at("optics");
  optical.pitch_x=o.at("pixel_pitch_x_m");optical.pitch_y=o.at("pixel_pitch_y_m");
  optical.wavelengths=o.at("wavelengths_m").get<std::vector<double>>();optical.padding_factor=o.at("padding_factor");
  optical.bandlimit=o.at("phase_gradient_bandlimit");optical.threads=o.at("threads");optical.cache_mib=o.at("transfer_cache_mib");
  const auto& phase_config=config.at("phase");
  const std::string phase_model=phase_config.value("model",std::string("replacement_mask"));
  const bool additive_dynamic=phase_model=="static_plus_dynamic_perturbation";
  if(!additive_dynamic&&phase_model!="replacement_mask")throw std::runtime_error("Unsupported phase model: "+phase_model);
  const double scale=o.at("stored_field_scale"),
    static_phase_min=additive_dynamic?phase_config.at("static_random_min_rad").get<double>():phase_config.at("random_min_rad").get<double>(),
    static_phase_max=additive_dynamic?phase_config.at("static_random_max_rad").get<double>():phase_config.at("random_max_rad").get<double>(),
    dynamic_phase_min=additive_dynamic?phase_config.at("dynamic_perturbation_min_rad").get<double>():static_phase_min,
    dynamic_phase_max=additive_dynamic?phase_config.at("dynamic_perturbation_max_rad").get<double>():static_phase_max,
    ramp=phase_config.contains("cgh_phase_ramp_step_rad")?phase_config.at("cgh_phase_ramp_step_rad").get<double>():phase_config.at("frame_ramp_step_rad").get<double>();
  if(!(static_phase_min<static_phase_max)||!(dynamic_phase_min<dynamic_phase_max))throw std::runtime_error("Invalid phase range");
  const uint64_t base_seed=phase_config.at("base_seed");
  const fs::path static_root=absolute(root,config.at("paths").at("static_output_root").get<std::string>()),
    dynamic_root=absolute(root,config.at("paths").at("dynamic_output_root").get<std::string>());
  const std::string static_id=config.value("static_dataset_id",std::string("static"));
  const std::string dynamic_id=config.value("dynamic_dataset_id",std::string("dynamic"));
  if(a.mode!="dynamic")publish_dataset(static_root,static_id,config,manifest,"one deterministic static spatial mask per sequence reused for every frame");
  if(a.mode!="static")publish_dataset(dynamic_root,dynamic_id,config,manifest,
    additive_dynamic?"one deterministic static spatial mask per sequence plus an independent perturbation for every frame, including frame zero"
                    :"one deterministic spatial mask per sequence and frame; frame zero equals static");
  std::array<std::unique_ptr<Propagator>,3> propagators;
  for(auto& p:propagators)p=std::make_unique<Propagator>(width,height,optical);
  unsigned processed=0;
  for(const auto& sequence:config.at("sequences")) {
    const std::string name=sequence;
    const uint64_t sequence_seed=fnv(name,base_seed);
    for(unsigned frame=0;frame<frame_count;++frame) {
      if(a.max_frames && processed>=a.max_frames)goto done;
      const fs::path prepared=absolute(root,config.at("paths").at("prepared_root").get<std::string>())/name;
      const auto rgb=bytes(prepared/frame_name(frame,".rgb8"),pixels*3),depth=bytes(prepared/frame_name(frame,".depth_u8"),pixels);
      for(const bool dynamic:{false,true}) {
        if((a.mode=="static"&&dynamic)||(a.mode=="dynamic"&&!dynamic))continue;
        const fs::path output_root=dynamic?dynamic_root:static_root,out=output_root/name/frame_name(frame,".holo"),meta=output_root/name/frame_name(frame,".json");
        if(a.resume && fs::exists(out) && fs::exists(meta)) {
          const auto old=load(meta);if(old.value("complete",false)&&old.at("frame")==frame&&old.at("sequence")==name)continue;
        }
        if(fs::exists(out)||fs::exists(meta))throw std::runtime_error("Output exists without validated --resume: "+out.string());
        Frame result;result.width=width;result.height=height;result.channels=3;result.scalar_bits=32;result.frame_index=frame;
        result.real.assign(pixels*3,0);result.imag.assign(pixels*3,0);
        const uint64_t mask_seed=dynamic&&frame?splitmix(sequence_seed^uint64_t(frame)):sequence_seed;
        const uint64_t perturbation_seed=dynamic?splitmix(sequence_seed^uint64_t(frame)^0xd1b54a32d192ed03ull):0;
        std::vector<Complex> random_phase(pixels);
#pragma omp parallel for
        for(int64_t i=0;i<int64_t(pixels);++i) {
          double angle;
          if(additive_dynamic) {
            angle=static_phase_min+(static_phase_max-static_phase_min)*unit(sequence_seed^uint64_t(i));
            if(dynamic)angle+=dynamic_phase_min+(dynamic_phase_max-dynamic_phase_min)*unit(perturbation_seed^uint64_t(i));
          } else {
            angle=static_phase_min+(static_phase_max-static_phase_min)*unit(mask_seed^uint64_t(i));
          }
          random_phase[size_t(i)]={std::cos(angle),std::sin(angle)};
        }
        const double global_angle=double(frame)*ramp,gr=std::cos(global_angle),gi=std::sin(global_angle);
        for(unsigned channel=0;channel<3;++channel) {
          std::vector<Complex> sum(pixels);
          for(unsigned layer=0;layer<planes.size();++layer) {
            std::vector<Complex> object(pixels);
#pragma omp parallel for
            for(int64_t i=0;i<int64_t(pixels);++i)if(depth[size_t(i)]==layer)
              object[size_t(i)]=random_phase[size_t(i)]*(double(rgb[size_t(i)*3+channel])/255.0);
            auto spectrum=propagators[channel]->spectrum(object);
            auto hologram=propagators[channel]->propagate(spectrum,optical.wavelengths[channel],-planes[layer]);
#pragma omp parallel for
            for(int64_t i=0;i<int64_t(pixels);++i)sum[size_t(i)]+=hologram[size_t(i)];
          }
#pragma omp parallel for
          for(int64_t i=0;i<int64_t(pixels);++i) {
            const auto value=sum[size_t(i)]*Complex(gr,gi)*scale;
            result.real[size_t(channel)*pixels+size_t(i)]=value.real();result.imag[size_t(channel)*pixels+size_t(i)]=value.imag();
          }
        }
        write_holo(out,result);
        json metadata={{"complete",true},{"sequence",name},{"frame",frame},{"sintel_frame",frame+config.at("first_sintel_frame").get<unsigned>()},
          {"mode",dynamic?dynamic_id:static_id},{"phase_model",phase_model},
          {"global_frame_phase_rad_unwrapped",global_angle},
          {"global_frame_phase_rad_wrapped",std::atan2(std::sin(global_angle),std::cos(global_angle))},
          {"cgh_global_phase_rotation_rad_unwrapped",global_angle},
          {"cgh_global_phase_rotation_rad_wrapped",std::atan2(std::sin(global_angle),std::cos(global_angle))},
          {"cgh_global_phase_rotation_applied_after_propagation",true},
          {"rgb_policy","same spatial phase and same global angle for RGB"},{"depth_policy","same spatial phase for all ten layers"},
          {"stored_scale",scale},{"output",out.u8string()},{"statistics",frame_statistics(result)}};
        if(additive_dynamic) {
          metadata["static_mask_seed"]=sequence_seed;
          metadata["static_random_phase_range_rad"]={static_phase_min,static_phase_max};
          metadata["dynamic_perturbation_seed"]=dynamic?json(perturbation_seed):json(nullptr);
          metadata["dynamic_perturbation_phase_range_rad"]={dynamic_phase_min,dynamic_phase_max};
          metadata["dynamic_perturbation_applied"]=dynamic;
        } else {
          metadata["mask_seed"]=mask_seed;
          metadata["random_phase_range_rad"]={static_phase_min,static_phase_max};
        }
        write_json(meta,metadata);
        std::cout<<(dynamic?"dynamic":"static")<<" "<<name<<" frame "<<frame<<" complete\n"<<std::flush;
      }
      ++processed;
    }
  }
done:
  const std::array<std::pair<fs::path,std::string>,2> outputs{{{static_root,"static"},{dynamic_root,"dynamic"}}};
  for(const auto& item:outputs) {
    if((a.mode=="static"&&item.second=="dynamic")||(a.mode=="dynamic"&&item.second=="static"))continue;
    auto dataset=load(item.first/"dataset.json");dataset["complete"]=!a.max_frames;dataset["processed_source_frames_this_run"]=processed;
    write_json(item.first/"dataset.json",dataset);
  }
  std::cout<<"Generation finished: source frames processed="<<processed<<"\n";return 0;
} catch(const std::exception& e) {std::cerr<<"ERROR: "<<e.what()<<"\n";return 1;}
