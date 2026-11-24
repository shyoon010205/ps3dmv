#pragma once
#include "../../../../cpp/ps3dmvp/PSCore.h"
namespace vvenc {
struct CodingUnit;class CodingStructure;struct Picture;struct UnitArea;template<typename T>struct UnitBuf;
namespace ps {
ps3dmvp::Context context(const CodingUnit& cu);
void predict(const CodingUnit& cu,UnitBuf<short>& destination);
void apply(CodingUnit& cu,const ps3dmvp::Motion& motion);
void validate(const CodingStructure& cs);
void beginCtu(CodingStructure& cs,int ctuX,int ctuY);
void commitCtu(CodingStructure& cs,const UnitArea& area);
void writePicture(Picture& picture);
void writeAreas(const Picture& picture);
uint64_t sideBits(const CodingStructure& cs);
uint64_t distortion(const CodingStructure& cs);
}
}
