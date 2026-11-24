#pragma once
#include "../../../../../cpp/ps3dmvp/PSCore.h"
namespace vvdec {
struct CodingUnit;class CodingStructure;class Slice;struct Picture;template<typename T>struct UnitBuf;
namespace ps {
ps3dmvp::Context context(const CodingUnit& cu);
void beginCtu(CodingStructure& cs,const Slice& slice,unsigned ctuAddr);
void decodeLeaf(CodingUnit& cu);
void apply(CodingUnit& cu,const ps3dmvp::Motion& motion);
void predict(const CodingUnit& cu,UnitBuf<short>& destination);
void writeAreas(const Picture& picture);
}
}
