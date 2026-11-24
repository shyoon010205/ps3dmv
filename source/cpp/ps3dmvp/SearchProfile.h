#pragma once
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <algorithm>
// Encoder diagnostics only. No per-MV output and no influence on search/RDO.
namespace psinstrument {
using Clock=std::chrono::steady_clock;
struct Profile {
  uint64_t calls=0,mvs=0,thetas=0,pairs=0,samples=0,peakScratch=0,lookupBytes=0;
  double uniSeconds=0,biSeconds=0;bool started=false;
  void flush(const std::string& stats,int poc){
    if(stats.empty())return;
    std::ofstream f(stats+".search.csv",started?std::ios::app:std::ios::trunc);
    if(!f)return;
    if(!started)f<<"poc,search_calls,unique_mv,theta_tests,bi_pairs,sample_pairs,uni_seconds,bi_seconds,peak_scratch_bytes,projection_lookup_bytes\n";
    f.precision(17);f<<poc<<','<<calls<<','<<mvs<<','<<thetas<<','<<pairs<<','<<samples<<','<<uniSeconds<<','<<biSeconds<<','<<peakScratch<<','<<lookupBytes<<'\n';
    started=true;calls=mvs=thetas=pairs=samples=peakScratch=0;uniSeconds=biSeconds=0;
  }
};
inline Profile& profile(){static Profile value;return value;}
struct SearchMeasure {
  Clock::time_point start=Clock::now(),biStart;bool bi=false;
  SearchMeasure(){++profile().calls;}
  void beginBi(){biStart=Clock::now();bi=true;profile().uniSeconds+=std::chrono::duration<double>(biStart-start).count();}
  ~SearchMeasure(){const auto end=Clock::now();if(bi)profile().biSeconds+=std::chrono::duration<double>(end-biStart).count();else profile().uniSeconds+=std::chrono::duration<double>(end-start).count();}
};
}
