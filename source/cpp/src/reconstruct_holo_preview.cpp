#include "holo.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace holo;

namespace {
json load(const fs::path& path){std::ifstream s(path);if(!s)throw std::runtime_error("Cannot open "+path.string());json j;s>>j;return j;}
std::string name(size_t index){std::ostringstream s;s<<"z_"<<std::setw(2)<<std::setfill('0')<<index;return s.str();}
}

int main(int argc,char** argv) try {
  fs::path input,output,config_path;
  for(int i=1;i<argc;++i){const std::string arg=argv[i];auto value=[&](){if(++i>=argc)throw std::runtime_error("Missing value for "+arg);return fs::path(argv[i]);};
    if(arg=="--input")input=value();else if(arg=="--output")output=value();else if(arg=="--config")config_path=value();else throw std::runtime_error("Unknown argument: "+arg);}
  if(input.empty()||output.empty()||config_path.empty())throw std::runtime_error("Usage: reconstruct_holo_preview --input FILE --output DIR --config FILE");
  const auto cfg=load(config_path);
  const auto frame=read_holo(input);
  const auto planes=cfg.at("object_plane_z_m").get<std::vector<double>>();
  const auto& o=cfg.at("optics");
  if(frame.channels!=3||frame.width!=cfg.at("target_width")||frame.height!=cfg.at("target_height"))throw std::runtime_error("Unexpected hologram dimensions/channels");
  OpticalConfig optical;optical.pitch_x=o.at("pixel_pitch_x_m");optical.pitch_y=o.at("pixel_pitch_y_m");optical.wavelengths=o.at("wavelengths_m").get<std::vector<double>>();
  optical.padding_factor=o.at("padding_factor");optical.bandlimit=o.at("phase_gradient_bandlimit");optical.threads=o.at("threads");optical.cache_mib=o.at("transfer_cache_mib");
  const double inverse_scale=1.0/o.at("stored_field_scale").get<double>();const size_t pixels=frame.pixels();Propagator propagator(frame.width,frame.height,optical);
  std::vector<std::vector<Complex>> spectra(3);for(unsigned c=0;c<3;++c){std::vector<Complex> field(pixels);for(size_t i=0;i<pixels;++i)field[i]=Complex(frame.real[c*pixels+i],frame.imag[c*pixels+i])*inverse_scale;spectra[c]=propagator.spectrum(field);}
  json depths=json::array();for(size_t d=0;d<planes.size();++d){std::vector<float> amplitude(pixels*3);for(unsigned c=0;c<3;++c){const auto reconstructed=propagator.propagate(spectra[c],optical.wavelengths[c],planes[d]);for(size_t i=0;i<pixels;++i)amplitude[c*pixels+i]=float(std::abs(reconstructed[i]));}
    const auto stem=name(d);write_rgb_png(output/(stem+".png"),frame.width,frame.height,amplitude,0,1);depths.push_back({{"index",d},{"object_plane_z_m",planes[d]},{"image",stem+".png"}});}
  write_json(output/"preview.json",{{"complete",true},{"input",fs::absolute(input).u8string()},{"frame",frame.frame_index},{"depths",depths},{"display",{{"quantity","amplitude"},{"clip",{0,1}},{"gamma",1}}}});
  std::cout<<"Preview reconstruction complete: "<<output<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<"\n";return 1;}
