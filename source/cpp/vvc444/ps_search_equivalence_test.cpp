#include "../ps3dmvp/PSCore.cpp"
#include <iostream>

int main() {
  using namespace ps3dmvp;
  try {
    auto& searchConfig=config();searchConfig.enabled=true;searchConfig.fullSearch=true;searchConfig.candidates=4;
    uint32_t random=1234567;
    auto next=[&](){random=random*1664525u+1013904223u;return random;};
    constexpr int width=64,height=48;
    std::array<std::vector<int16_t>,8> refs;
    for(auto& plane:refs){plane.resize(width*height);for(auto& sample:plane)sample=int16_t(next()>>24);}
    size_t candidates=0,syntaxCases=0;
    for(unsigned trial=0;trial<8;++trial) {
      searchConfig.thetaDivisions=trial%3==0?1:trial%3==1?4:16;makeRotation();searchConfig.searchRange=trial==7?32:8;
      Context ctx;ctx.poc=20;ctx.width=width;ctx.height=height;ctx.ctuSize=32;
      ctx.block={trial%2?48:0,trial%2?32:0,16,16};ctx.bi=trial%2!=0;ctx.tmvp=true;ctx.checkLDC=true;
      for(int l=0;l<2;++l)for(int r=0;r<2;++r){Ref ref;ref.poc=16-4*r+8*l;ref.real={refs[l*4+r*2].data(),width,width,height};ref.imag={refs[l*4+r*2+1].data(),width,width,height};ctx.refs[l].push_back(ref);}
      Motion neighbour;neighbour.used=true;neighbour.direction=3;neighbour.refPoc[0]=16;neighbour.refPoc[1]=24;
      neighbour.mv[0]={trial==6?8190:9,-7};neighbour.mv[1]={-5,trial==6?-8190:11};
      ctx.spatial=[=](int,int){return neighbour;};ctx.history.push_back(neighbour);
      resetMap(16,width,height);commitMap(16,{0,0,width,height},neighbour);
      std::array<std::vector<int16_t>,2> input;for(auto& plane:input){plane.resize(256);for(auto& sample:plane)sample=trial==0?128:int16_t(next()>>24);}
      const std::array<Plane,2> org={{{input[0].data(),16,16,16},{input[1].data(),16,16,16}}};
      const double lambda=trial%3==0?0:trial%3==1?.25:12.5;
      searchConfig.optimizedSearch=false;const auto reference=search(ctx,org,lambda);
      searchConfig.optimizedSearch=true;const auto optimized=search(ctx,org,lambda);
      require(reference.size()==optimized.size(),"shortlist size differs");
      for(size_t i=0;i<reference.size();++i){Bits a,b;encodeMotion(a,ctx,reference[i].motion);encodeMotion(b,ctx,optimized[i].motion);
        require(a.count==b.count&&a.data==b.data&&reference[i].cost==optimized[i].cost,"search candidate/order/cost differs");++candidates;}
      for(unsigned i=0;i<500;++i){auto m=neighbour;m.used=i%7!=0;m.direction=ctx.bi?int(i%3)+1:1;
        for(int l=0;l<2;++l){m.ref[l]=int(next()%2);m.mvp[l]=int(next()%2);m.theta[l]=int(next()%searchConfig.thetaDivisions);m.mvd[l]={int(next()%32765)-16382,int(next()%32765)-16382};}
        Bits b;encodeMotion(b,ctx,m);require(syntaxBits(ctx,m)==b.count,"direct syntax bit count differs from actual writer");++syntaxCases;}
    }
    std::cout<<"PASS: "<<candidates<<" ordered candidates/costs and "<<syntaxCases<<" syntax counts; edges, ties, MVP overlap, temporal map, extreme motion, theta 1/4/16 and full range32. No codec executed.\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
