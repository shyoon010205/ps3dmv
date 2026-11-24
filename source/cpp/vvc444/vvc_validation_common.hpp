#pragma once
#include "../hm/study_runtime.hpp"
#include <set>

using namespace hmvalidation;
using study::atomicJson;

namespace vvcvalidation {
struct Condition {
  int width, height, frames, depth, qp, fps;
  size_t pixels() const { return size_t(width) * size_t(height); }
  size_t frameBytes() const { return pixels() * 3 * (depth == 8 ? 1 : 2); }
};

Condition validate(const json& cfg) {
  Condition c{cfg.at("crop").at("width"), cfg.at("crop").at("height"),
    cfg.at("frames"), cfg.at("internal_bit_depth"), cfg.at("qp"), cfg.at("frame_rate")};
  check(c.width >= 64 && c.height >= 64 && c.width <= 4096 && c.height <= 4096 &&
        c.width % 8 == 0 && c.height % 8 == 0, "Crop must be 64..4096 and a multiple of 8");
  check(c.frames == 17 && cfg.at("gop_size") == 16, "This crosscheck uses RA GOP16 / 17 frames");
  check(c.depth == 8 || c.depth == 10, "Internal/output depth must be 8 or 10");
  check(c.qp >= 0 && c.qp <= 63 && c.fps > 0, "Invalid QP/frame rate");
  check(cfg.at("schema_version") == 1, "Unsupported config schema");
  check(cfg.at("input_kind") == "hologram" || cfg.at("input_kind") == "synthetic",
        "input_kind must be hologram or synthetic");
  return c;
}

fs::path framePath(const fs::path& dir, int frame) {
  std::ostringstream name;
  name << "frame_" << std::setfill('0') << std::setw(6) << frame << ".holo";
  return dir / name.str();
}

json prepareInput(const fs::path& root, const json& cfg, const fs::path& path, const Condition& c) {
  const int channel = cfg.at("channel"), first = cfg.at("first_frame");
  const int x0 = cfg.at("crop").at("x"), y0 = cfg.at("crop").at("y");
  check(channel >= 0 && channel < 3 && first >= 0 && x0 >= 0 && y0 >= 0, "Invalid source selection");
  const auto source = root / fs::u8path(cfg.at("source_directory").get<std::string>());
  const bool synthetic = cfg.at("input_kind") == "synthetic";
  json originals = json::array();
  double amplitude = 0;
  auto validateFrame = [&](const holo::Frame& frame, int index) {
    check(frame.frame_index == uint64_t(index) && channel < int(frame.channels) &&
          x0 + c.width <= int(frame.width) && y0 + c.height <= int(frame.height), "Source/crop mismatch");
  };
  if (!synthetic) {
    for (int f = first; f < first + c.frames; ++f) {
      auto p = framePath(source, f);
      originals.push_back(fileInfo(p));
      auto frame = holo::read_holo(p);
      validateFrame(frame, f);
      for (int y = 0; y < c.height; ++y) for (int x = 0; x < c.width; ++x) {
        const size_t i = size_t(channel) * frame.pixels() + size_t(y0+y) * frame.width + x0+x;
        check(std::isfinite(frame.real[i]) && std::isfinite(frame.imag[i]), "Non-finite original sample");
        amplitude = std::max(amplitude, std::hypot(frame.real[i], frame.imag[i]));
      }
    }
  }
  if (amplitude == 0) amplitude = 1;
  amplitude=std::nextafter(amplitude,std::numeric_limits<double>::infinity());
  std::ofstream output(path, std::ios::binary);
  check(bool(output), "Cannot write input");
  std::vector<unsigned char> packed(c.pixels() * 3, 0);
  for (int f = 0; f < c.frames; ++f) {
    if (synthetic) {
      // Distinct, nonconstant planes expose swaps, subsampling, and stride mistakes.
      for (int k = 0; k < 3; ++k) for (int y = 0; y < c.height; ++y) for (int x = 0; x < c.width; ++x)
        packed[size_t(k)*c.pixels()+size_t(y)*c.width+x] = static_cast<unsigned char>(
          (x*(11+2*k) + y*(3+6*k) + f*(17+4*k) + ((x+y+f)&1)*97 + 41*k) & 255);
    } else {
      auto frame = holo::read_holo(framePath(source, first+f));
      validateFrame(frame, first+f);
      for (int k = 0; k < 2; ++k) for (int y = 0; y < c.height; ++y) for (int x = 0; x < c.width; ++x) {
        const size_t i = size_t(channel)*frame.pixels() + size_t(y0+y)*frame.width+x0+x;
        const double v = k == 0 ? frame.real[i] : frame.imag[i];
        packed[size_t(k)*c.pixels()+size_t(y)*c.width+x] = static_cast<unsigned char>(
          std::clamp(std::floor((v/amplitude+1)*127.5+0.5), 0.0, 255.0));
      }
    }
    output.write(reinterpret_cast<const char*>(packed.data()), std::streamsize(packed.size()));
  }
  output.close();
  check(bool(output), "Input write failed");
  for (const auto& original : originals)
    check(sha256(fs::u8path(original.at("path").get<std::string>())) == original.at("sha256"),
          "Original changed during input preparation");
  return {{"kind", cfg.at("input_kind")}, {"input", fileInfo(path)}, {"sources", originals},
    {"input_bit_depth",8}, {"packing",synthetic ? "three distinct synthetic planes" : "Y=Real,Cb=Imag,Cr=0; no color conversion"},
    {"amplitude",amplitude}, {"mapping","floor((v/A+1)*127.5+0.5), clip [0,255]; common A across both components and all "+std::to_string(c.frames)+" crops"},
    {"originals_unchanged",true}};
}

class Frames {
  std::ifstream stream;
  bool y4m;
  Condition c;
  std::string line() {
    std::string value; char ch;
    while (stream.get(ch)) {
      if (ch == '\n') return value;
      check(value.size() < 4096, "Oversized Y4M header");
      if (ch != '\r') value += ch;
    }
    throw std::runtime_error("Truncated Y4M header");
  }
public:
  json header;
  Frames(const fs::path& path, Condition condition, bool isY4m)
    : stream(path, std::ios::binary), y4m(isY4m), c(condition) {
    check(bool(stream), "Missing reconstruction: " + path.u8string());
    if (!y4m) {
      check(fs::file_size(path) == uint64_t(c.frameBytes())*c.frames, "Raw reconstruction byte count mismatch");
      return;
    }
    const auto text = line();
    std::istringstream tokens(text); std::string token;
    tokens >> token;
    check(token == "YUV4MPEG2", "Missing Y4M magic");
    int w = 0, h = 0; std::string chroma, interlace;
    while (tokens >> token) {
      if (token[0] == 'W') w = std::stoi(token.substr(1));
      if (token[0] == 'H') h = std::stoi(token.substr(1));
      if (token[0] == 'C') chroma = token.substr(1);
      if (token[0] == 'I') interlace = token.substr(1);
    }
    check(w == c.width && h == c.height && chroma == (c.depth == 8 ? "444" : "444p10") && interlace == "p",
          "VVdeC output is not the expected progressive 444 dimensions/depth");
    header = {{"text",text},{"width",w},{"height",h},{"chroma",chroma},{"bit_depth",c.depth}};
  }
  std::vector<unsigned char> read() {
    if (y4m) { const auto tag = line(); check(tag == "FRAME" || tag.rfind("FRAME ",0) == 0, "Missing Y4M frame delimiter"); }
    std::vector<unsigned char> bytes(c.frameBytes());
    stream.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    check(stream.gcount() == std::streamsize(bytes.size()), "Truncated reconstructed frame");
    return bytes;
  }
  void finish() { check(stream.peek() == std::char_traits<char>::eof() && !stream.bad(), "Unexpected trailing frames/data"); }
};

json compareSamples(const fs::path& reference, const fs::path& decoded, const fs::path& csv,
                    const Condition& c, bool y4m) {
  Frames a(reference, c, false), b(decoded, c, y4m);
  json result = {{"reference",fileInfo(reference)},{"decoded",fileInfo(decoded)},
    {"decoder_header",b.header},{"identical",false},{"first_mismatch",nullptr}};
  uint64_t totalErrors=0, totalSse=0; int maxError=0;
  std::ostringstream rows;
  rows << "display_frame,plane,samples,mismatch_count,max_abs_error,sse\n";
  for (int f=0; f<c.frames; ++f) {
    const auto x=a.read(), y=b.read();
    for (int k=0; k<3; ++k) {
      uint64_t errors=0, sse=0; int peak=0;
      for (size_t i=0; i<c.pixels(); ++i) {
        const size_t offset=(size_t(k)*c.pixels()+i)*(c.depth==8?1:2);
        const int v=int(x[offset])+(c.depth==8?0:(int(x[offset+1])<<8));
        const int w=int(y[offset])+(c.depth==8?0:(int(y[offset+1])<<8));
        check(v<(1<<c.depth) && w<(1<<c.depth), "Decoded sample exceeds signalled bit depth");
        const int d=std::abs(v-w);
        if (d) {
          ++errors;
          if (result["first_mismatch"].is_null()) result["first_mismatch"] = {
            {"display_frame",f},{"plane",k},{"x",i%size_t(c.width)},{"y",i/size_t(c.width)},
            {"reference",v},{"decoded",w}};
        }
        peak=std::max(peak,d); sse+=uint64_t(d)*d;
      }
      totalErrors+=errors; totalSse+=sse; maxError=std::max(maxError,peak);
      rows<<f<<','<<k<<','<<c.pixels()<<','<<errors<<','<<peak<<','<<sse<<'\n';
    }
  }
  a.finish(); b.finish();
  result["frames"]=c.frames; result["samples"]=uint64_t(c.pixels())*3*c.frames;
  result["mismatch_count"]=totalErrors; result["sse"]=totalSse; result["max_abs_error"]=maxError;
  result["identical"]=totalErrors==0; result["per_frame_plane_csv"]=csv.u8string();
  writeText(csv, rows.str());
  return result;
}

json vtmHashes(const fs::path& log, int frames) {
  std::ifstream input(log); std::string line; std::set<int> pocs; int ok=0, seen=0;
  while (std::getline(input,line)) {
    if (line.find("[MD5:") == std::string::npos) continue;
    ++seen;
    auto p=line.find("POC ");
    check(p!=std::string::npos, "Cannot identify VTM hash POC");
    const int poc=std::stoi(line.substr(p+4));
    check(poc>=0 && poc<frames && pocs.insert(poc).second, "Duplicate/out-of-range hash POC");
    if (line.find("(OK)")!=std::string::npos) ++ok;
  }
  return {{"count",seen},{"valid",ok},{"passed",seen==frames&&ok==frames}};
}

std::vector<std::string> encoderArgs(const fs::path& base, const fs::path& overlay,
  const fs::path& input, const fs::path& bitstream, const fs::path& reco, const Condition& c) {
  return {"-c",base.u8string(),"-c",overlay.u8string(),"-i",input.u8string(),"-b",bitstream.u8string(),
    "-o",reco.u8string(),"--SourceWidth="+std::to_string(c.width),"--SourceHeight="+std::to_string(c.height),
    "--FramesToBeEncoded="+std::to_string(c.frames),"--FrameRate="+std::to_string(c.fps),
    "--QP="+std::to_string(c.qp),"--InternalBitDepth="+std::to_string(c.depth),
    "--OutputBitDepth="+std::to_string(c.depth),
    "--OutputBitDepthC="+std::to_string(c.depth),"--Verbosity=6"};
}

}
