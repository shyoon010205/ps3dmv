#include "holo.hpp"
#include "frame_phase.hpp"
#include "reference_resume.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

using namespace holo;
namespace {
constexpr const char* implementation="cpp_hologram_reconstruction_v1_png_only";
struct Arguments {
  fs::path config="config/reconstruction.json";
  std::string sequence;
  std::optional<uint64_t> first,maximum;
  std::optional<uint64_t> fft_threads,cache_mib;
  bool convert_only=false,resume=false,overwrite=false,check=false;
};
uint64_t natural(const std::string& text) {
  if (text.empty() || text.find_first_not_of("0123456789")!=std::string::npos) throw std::runtime_error("Expected nonnegative integer: "+text);
  return std::stoull(text);
}
Arguments arguments(int argc,char** argv) {
  Arguments a;
  for (int i=1;i<argc;++i) {
    const std::string arg=argv[i];
    auto value=[&]() {if (++i>=argc) throw std::runtime_error("Missing value for "+arg); return std::string(argv[i]);};
    if (arg=="--config") a.config=fs::u8path(value());
    else if (arg=="--sequence") a.sequence=value();
    else if (arg=="--first-frame") a.first=natural(value());
    else if (arg=="--max-frames") a.maximum=natural(value());
    else if (arg=="--convert-only") a.convert_only=true;
    else if (arg=="--resume") a.resume=true;
    else if (arg=="--overwrite") a.overwrite=true;
    else if (arg=="--fft-threads") a.fft_threads=natural(value());
    else if (arg=="--transfer-cache-mib") a.cache_mib=natural(value());
    else if (arg=="--check") a.check=true;
    else if (arg=="--help") {
      std::cout<<"reconstruct_original --config FILE [--sequence NAME] [--first-frame N] [--max-frames N]\n"
        "                     [--convert-only] [--resume | --overwrite]\n"
        "                     [--fft-threads N] [--transfer-cache-mib N] [--check]\n"
        "Frame range comes from config (default: frames 0-16 per sequence).\n"
        "Saves original .holo, display PNG and metadata only; no reconstructed numeric arrays.\n";
      std::exit(0);
    } else throw std::runtime_error("Unknown option: "+arg);
  }
  if ((a.maximum && *a.maximum==0) || (a.resume && a.overwrite)) throw std::runtime_error("Invalid frame limit or conflicting resume/overwrite options");
  if(a.fft_threads && (*a.fft_threads<1 || *a.fft_threads>256))throw std::runtime_error("FFT threads must be 1..256");
  return a;
}
fs::path resolved(const fs::path& base,const std::string& path) {
  const auto p=fs::u8path(path); return fs::weakly_canonical(p.is_absolute()?p:base/p);
}
bool inside(const fs::path& child,const fs::path& parent) {
  auto a=fs::weakly_canonical(child).generic_u8string(), b=fs::weakly_canonical(parent).generic_u8string();
#ifdef _WIN32
  std::transform(a.begin(),a.end(),a.begin(),[](unsigned char c){return char(std::tolower(c));});
  std::transform(b.begin(),b.end(),b.begin(),[](unsigned char c){return char(std::tolower(c));});
#endif
  return a==b || a.rfind(b+"/",0)==0;
}
std::string frame_name(uint64_t n) {std::ostringstream s;s<<"frame_"<<std::setw(6)<<std::setfill('0')<<n;return s.str();}
std::string depth_name(size_t n) {std::ostringstream s;s<<"z_"<<std::setw(2)<<std::setfill('0')<<n;return s.str();}
double seconds(std::chrono::steady_clock::time_point t) {return std::chrono::duration<double>(std::chrono::steady_clock::now()-t).count();}
OpticalConfig optics(const json& j) {
  OpticalConfig o;
  o.pitch_x=j.at("pixel_pitch_x_m");o.pitch_y=j.at("pixel_pitch_y_m");
  o.field_scale=j.at("field_scale_for_reconstruction_only");
  o.wavelengths=j.at("wavelengths_m").get<std::vector<double>>();
  o.plane_z=j.at("reconstruction_plane_z_m").get<std::vector<double>>();
  o.propagation_sign=j.at("propagation_distance_sign");o.padding_factor=j.at("padding_factor");
  o.bandlimit=j.at("phase_gradient_bandlimit");o.threads=j.at("threads");o.cache_mib=j.at("transfer_cache_mib");
  if (!std::isfinite(o.field_scale) || o.field_scale<=0 || (o.propagation_sign!=-1 && o.propagation_sign!=1) ||
      o.wavelengths.size()!=3 || o.plane_z.empty() || o.plane_z.size()>1000)
    throw std::runtime_error("Invalid optical parameters");
  for (auto x:o.wavelengths) if (!std::isfinite(x) || x<=0) throw std::runtime_error("Invalid wavelength");
  for (auto x:o.plane_z) if (!std::isfinite(x)) throw std::runtime_error("Invalid reconstruction plane");
  return o;
}
}

int main(int argc,char** argv) {
  try {
    const auto args=arguments(argc,argv);const auto config_path=fs::absolute(args.config);
    const auto cfg=read_json(config_path);const auto base=config_path.parent_path();
    const auto first=args.first.value_or(cfg.value("first_frame",uint64_t(0)));
    const auto maximum=args.maximum.value_or(cfg.value("max_frames_per_sequence",uint64_t(17)));
    if (!maximum || cfg.value("save_reconstructed_numeric_arrays",false))
      throw std::runtime_error("Frame count must be positive; reconstructed numeric array files are disabled");
    if (cfg.at("schema_version")!=1 || cfg.at("input_forward_mapping")!=false || cfg.at("input_clipping")!="none")
      throw std::runtime_error("Code (1) only accepts unmapped, unclipped original input");
    const auto input=resolved(base,cfg.at("input_root")),converted=resolved(base,cfg.at("original_cpp_root")),reference=resolved(base,cfg.at("reference_root"));
    if (!fs::is_directory(input)) throw std::runtime_error("Input directory does not exist: "+input.string());
    if (inside(converted,input) || inside(reference,input) || inside(input,converted) || inside(input,reference) ||
        inside(converted,reference) || inside(reference,converted)) throw std::runtime_error("Input and output roots must be separate directories");
    auto optical=optics(cfg.at("optics"));
    // Runtime resource limits do not change optical identity or legacy output paths.
    if(args.fft_threads)optical.threads=size_t(*args.fft_threads);
    if(args.cache_mib)optical.cache_mib=size_t(*args.cache_mib);
    const double clip_low=cfg.at("display").at("clip_low"),clip_high=cfg.at("display").at("clip_high");
    if (clip_low!=0 || clip_high!=1 || cfg.at("display").at("quantity")!="amplitude" ||
        cfg.at("display").at("gamma")!=1 || cfg.at("display").at("independent_image_normalization")!=false)
      throw std::runtime_error("This reconstruction profile uses amplitude clipping [0,1]");
    json identity={{"implementation",implementation},{"optics",cfg.at("optics")},{"display",cfg.at("display")},
      {"amplitude_buffer","unclipped_float32_in_memory"},{"saved_outputs","png_and_metadata_only"}};
    const auto phase=cfg.value("frame_phase",json{{"enabled",false}});
    identity["frame_phase"]=phase;
    // The pre-migration random-phase v2 reference used the same propagation,
    // scaling and float32 amplitude path but had no phase configuration field.
    // Reuse its authenticated frame/depth checkpoints only when phase is disabled.
    const auto compatibility=cfg.value("reference_identity_compatibility",std::string());
    if(!compatibility.empty()) {
      if(compatibility!="cpp_original_v2_png_only" || phase.value("enabled",false))
        throw std::runtime_error("Legacy reference identity is valid only for unchanged random-phase originals");
      identity["implementation"]=compatibility;identity.erase("frame_phase");
    }
    const auto reference_id=fingerprint(identity.dump());
    if(!args.check){fs::create_directories(converted);fs::create_directories(reference);}
    std::vector<fs::path> sequences;
    for (const auto& e:fs::directory_iterator(input)) if (e.is_directory() && (args.sequence.empty() || e.path().filename().u8string()==args.sequence)) sequences.push_back(e.path());
    std::sort(sequences.begin(),sequences.end());
    if (sequences.empty()) throw std::runtime_error("No selected sequence directory");
    const auto start=std::chrono::steady_clock::now();
    const auto run_id=std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    json report={{"implementation",implementation},{"config",cfg},{"reference_id",reference_id},
      {"convert_only",args.convert_only},{"first_frame",first},{"max_frames_per_sequence",maximum},
      {"numeric_arrays_saved",false},{"complete",false},{"frames",json::array()}};
    report["runtime"]={{"fft_backend","pocketfft"},{"fft_threads",optical.threads},{"transfer_cache_mib",optical.cache_mib},
      {"resume_validation","source content fingerprint, optical identity, PNG dimensions/chunks/CRC"}};
    const auto report_path=reference/"_runs"/(run_id+".json");
    size_t total=0,reused_frames=0,reused_pngs=0,generated_pngs=0,forward_ffts=0,inverse_ffts=0;
    auto record_frame=[&](json record,std::chrono::steady_clock::time_point t) {
      record["elapsed_seconds"]=seconds(t);report["frames"].push_back(record);++total;
      report["processed_frames"]=total;report["reused_frames"]=reused_frames;
      report["reused_pngs"]=reused_pngs;report["generated_pngs"]=generated_pngs;
      report["forward_fft_count"]=forward_ffts;report["inverse_fft_count"]=inverse_ffts;
      report["elapsed_seconds"]=seconds(start);
      if(!args.check)write_json(report_path,report);
    };
    std::unique_ptr<Propagator> propagator;uint32_t current_w=0,current_h=0;
    std::cout<<"C++ original pipeline; reference_id="<<reference_id<<"\n";
    std::cout<<"Runtime: FFT threads="<<optical.threads<<", transfer cache="<<optical.cache_mib<<" MiB\n";
    for (const auto& sequence:sequences) {
      std::vector<std::pair<uint64_t,fs::path>> files;
      for (const auto& e:fs::directory_iterator(sequence)) if (e.is_regular_file() && e.path().extension()==".mat")
        files.emplace_back(natural(e.path().stem().u8string()),e.path());
      std::sort(files.begin(),files.end());
      if (files.empty()) continue;
      for (size_t i=1;i<files.size();++i) if (files[i-1].first==files[i].first) throw std::runtime_error("Duplicate numeric frame index");
      const auto name=sequence.filename().u8string();size_t selected=0;
      json inventory=json::array();for (const auto& f:files) inventory.push_back(f.first);
      bool dataset_written=false;
      for (const auto& [index,path]:files) {
        if (index<first || selected>=maximum) continue;
        ++selected;const auto frame_start=std::chrono::steady_clock::now();
        const auto folder=reference/name/reference_id/frame_name(index),meta=folder/"frame.json";
        std::vector<json> saved(optical.plane_z.size());
        std::string checked_source;
        if(!args.convert_only && (args.resume || args.check) && fs::is_directory(folder)) {
          checked_source=fingerprint(read_bytes(path));
          saved=reusable_depths(folder,identity,checked_source,reference_id,cfg.at("expected_width"),cfg.at("expected_height"),optical.plane_z.size());
        }
        const size_t existing=size_t(std::count_if(saved.begin(),saved.end(),[](const json& j){return !j.is_null();}));
        reused_pngs+=existing;
        if(existing==saved.size() || args.check) {
          const bool full=existing==saved.size();if(full)++reused_frames;
          json record={{"sequence",name},{"frame",index},{"reference_folder",folder.u8string()},
            {"reference_reused",full},{"reused_pngs",existing},{"pending_pngs",saved.size()-existing},
            {"mat_parsed",false},{"forward_fft_count",0},{"inverse_fft_count",0}};
          record_frame(record,frame_start);
          std::cout<<name<<" frame "<<index<<(full?" reused ":" pending ")<<existing<<"/"<<saved.size()<<" PNGs; "<<seconds(frame_start)<<" s\n"<<std::flush;
          continue;
        }
        auto mat=read_mat5(path,cfg.at("mat_variable"));auto& f=mat.frame;f.frame_index=index;
        if(!checked_source.empty() && checked_source!=mat.source_fingerprint)throw std::runtime_error("Source changed during resume verification: "+path.string());
        if (f.channels!=3 || f.width!=cfg.at("expected_width").get<uint32_t>() || f.height!=cfg.at("expected_height").get<uint32_t>())
          throw std::runtime_error("Unexpected dimensions; no automatic resize or guessed permutation: "+path.string());
        const auto phase_audit=apply_frame_phase(f,phase);
        if (!dataset_written) {
          write_json(converted/name/"dataset.json",{{"dataset_id",name},{"source_root",sequence.u8string()},{"source_format","MAT_v5_complex_tensor"},
            {"width",f.width},{"height",f.height},{"physical_channels",{"R","G","B"}},{"wavelengths_m",optical.wavelengths},
            {"scalar_bits",f.scalar_bits},{"frame_indices_expected",inventory},{"format","PSHCF01"},
            {"layout","channel_then_real_plane_then_imag_plane_row_major"},{"forward_mapping",false},{"input_clipping","none"},
            {"stored_scale",1.0},{"frame_phase",phase},{"reference_signal_bits_per_pixel",2*f.scalar_bits*f.channels}});
          dataset_written=true;
        }
        const auto out=converted/name/(frame_name(index)+".holo");bool wrote=false;
        if (fs::exists(out)) {
          if (!identical(f,read_holo(out))) {
            if (!args.overwrite) throw std::runtime_error("Existing converted frame differs: "+out.string());
            write_holo(out,f);wrote=true;
          }
        } else {write_holo(out,f);wrote=true;}
        if (!identical(f,read_holo(out))) throw std::runtime_error("Original roundtrip is not bit-exact: "+out.string());
        json record={{"sequence",name},{"frame",index},{"source",path.u8string()},{"source_bytes",fs::file_size(path)},
          {"source_fingerprint_fnv1a64",mat.source_fingerprint},{"converted",out.u8string()},{"written",wrote},
          {"scalar_bits",f.scalar_bits},{"roundtrip_bit_exact",true},{"roundtrip_scope","phase-transformed complex samples at source precision"},
          {"frame_phase",phase_audit},{"statistics",frame_statistics(f)}};
        write_json(converted/name/(frame_name(index)+".json"),record);
        if (!args.convert_only) {
          if (fs::exists(folder) && !args.resume && !args.overwrite)
            throw std::runtime_error("Reference exists; use --resume or --overwrite: "+folder.string());
          {
            if (!propagator || current_w!=f.width || current_h!=f.height) {
              propagator=std::make_unique<Propagator>(f.width,f.height,optical);current_w=f.width;current_h=f.height;
            }
            std::vector<std::vector<Complex>> spectra(f.channels);
            for (uint32_t c=0;c<f.channels;++c) {
              std::vector<Complex> field(f.pixels());
              for (size_t i=0;i<f.pixels();++i) field[i]=Complex(f.real[c*f.pixels()+i],f.imag[c*f.pixels()+i])*optical.field_scale;
              spectra[c]=propagator->spectrum(field);++forward_ffts;
            }
            json depths=json::array();
            for (size_t d=0;d<optical.plane_z.size();++d) {
              if(!saved[d].is_null()){depths.push_back(saved[d]);continue;}
              std::vector<float> a(f.samples());
              for(uint32_t c=0;c<f.channels;++c) {
                const auto recon=propagator->propagate(spectra[c],optical.wavelengths[c],optical.propagation_sign*optical.plane_z[d]);++inverse_ffts;
                for(size_t i=0;i<f.pixels();++i)a[c*f.pixels()+i]=float(std::abs(recon[i]));
              }
              const auto prefix=depth_name(d);
              write_rgb_png(folder/(prefix+".png"),f.width,f.height,a,clip_low,clip_high);
              const auto clipped=std::count_if(a.begin(),a.end(),[&](float v){return v>clip_high || v<clip_low;});
              json depth={{"index",d},{"plane_z_m",optical.plane_z[d]},{"propagation_distance_m",optical.propagation_sign*optical.plane_z[d]},
                {"image_file",prefix+".png"},{"amplitude_max",*std::max_element(a.begin(),a.end())},
                {"display_clipped_fraction",double(clipped)/a.size()}};
              write_json(folder/(prefix+".json"),{{"complete",true},{"reference_id",reference_id},{"identity",identity},
                {"source_fingerprint_fnv1a64",mat.source_fingerprint},{"width",f.width},{"height",f.height},
                {"numeric_arrays_saved",false},{"depth",depth},{"runtime",report["runtime"]}});
              depths.push_back(depth);++generated_pngs;
              std::cout<<name<<" frame "<<index<<" "<<prefix<<" saved; cached kernels="<<propagator->cached_kernel_count()<<", "<<seconds(frame_start)<<" s\n"<<std::flush;
            }
            write_json(meta,{{"complete",true},{"reference_id",reference_id},{"source_fingerprint_fnv1a64",mat.source_fingerprint},
              {"sequence",name},{"frame",index},{"width",f.width},{"height",f.height},{"source_scalar_bits",f.scalar_bits},
              {"converted_original",out.u8string()},{"identity",identity},{"runtime",report["runtime"]},{"depths",depths},{"numeric_arrays_saved",false},
              {"evaluation_rule","Recompute original and decoded amplitudes in memory from complex inputs with the same optics. Apply shared clip [0,1] in floating point for amplitude PSNR/SSIM; also compute unclipped error. Save metrics only, not numeric arrays. PNG is display only."}});
          }
          record["reference_folder"]=folder.u8string();record["reference_reused"]=false;
          record["reused_pngs"]=existing;record["generated_pngs"]=saved.size()-existing;
          record["forward_fft_count"]=3;record["inverse_fft_count"]=3*(saved.size()-existing);
          report["kernel_build_count"]=propagator->kernel_build_count();
          report["cached_kernel_count"]=propagator->cached_kernel_count();
          report["cached_kernel_bytes"]=propagator->cached_kernel_bytes();
        }
        record_frame(record,frame_start);
        std::cout<<name<<" frame "<<index<<" verified; "<<std::fixed<<std::setprecision(2)<<seconds(frame_start)<<" s\n"<<std::flush;
      }
    }
    if (!total) throw std::runtime_error("No frames selected");
    if(args.check){std::cout<<"CHECK: frames="<<total<<", complete="<<reused_frames<<", reusable PNGs="<<reused_pngs<<", pending PNGs="<<total*optical.plane_z.size()-reused_pngs<<"; no output written or FFT executed\n";return 0;}
    report["complete"]=true;report["elapsed_seconds"]=seconds(start);write_json(report_path,report);
    std::cout<<"Completed "<<total<<" frames. Report: "<<report_path.string()<<"\n";return 0;
  } catch (const std::exception& e) {std::cerr<<"ERROR: "<<e.what()<<"\n";return 1;}
}
