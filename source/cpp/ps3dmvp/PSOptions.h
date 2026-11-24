#pragma once
#include "PSCore.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
namespace ps3dmvp {
inline std::string& modeStatsPath(){static std::string value;return value;}
inline int integerOption(const std::string& value){size_t used=0;int n=std::stoi(value,&used);require(used==value.size(),"invalid integer option");return n;}
// Research CLI options are stripped before the unchanged upstream parser.
// Encoding settings and native syntax are untouched when PS3DMVP=0.
inline void parseOptions(int& argc,char** argv){
  int out=1;
  for(int i=1;i<argc;++i){const std::string arg=argv[i];const auto eq=arg.find('=');const auto key=arg.substr(0,eq);
    const bool known=key=="--PS3DMVP"||key=="--PSBitstream"||key=="--PSStats"||key=="--PS3DMVPThetaDivisions"||key=="--PS3DMVPSearchRange"||key=="--PS3DMVPCandidates"||key=="--PS3DMVPFullSearch"||key=="--PS3DMVPOptimizedSearch"||key=="--ModeStats";
    if(!known){argv[out++]=argv[i];continue;}
    require(eq!=std::string::npos,"research options require --Name=value");const auto value=arg.substr(eq+1);auto& c=config();
    if(key=="--PS3DMVP") {const int n=integerOption(value);require(n==0||n==1,"PS3DMVP must be 0 or 1");c.enabled=n!=0;}
    else if(key=="--PSBitstream")c.bitstream=value;
    else if(key=="--PSStats")c.stats=value;
    else if(key=="--ModeStats")modeStatsPath()=value;
    else if(key=="--PS3DMVPThetaDivisions")c.thetaDivisions=integerOption(value);
    else if(key=="--PS3DMVPSearchRange")c.searchRange=integerOption(value);
    else if(key=="--PS3DMVPCandidates")c.candidates=integerOption(value);
    else if(key=="--PS3DMVPOptimizedSearch"){const int n=integerOption(value);require(n==0||n==1,"PS3DMVPOptimizedSearch must be 0 or 1");c.optimizedSearch=n!=0;}
    else {const int n=integerOption(value);require(n==1,"This experiment uses full integer search only");c.fullSearch=true;}
  }
  argc=out;argv[out]=nullptr;
}
}
