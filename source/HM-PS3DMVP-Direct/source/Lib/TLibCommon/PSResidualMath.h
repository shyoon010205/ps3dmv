#ifndef PS_RESIDUAL_MATH_H
#define PS_RESIDUAL_MATH_H
#include <algorithm>
#include <cstdint>
#include <array>
#include <cmath>

// All arithmetic is in units of A/255: an original sample u represents 2u-255.
// A is the common Real/Imag mapping amplitude. Phase rotation remains Q20.
namespace ps3dmvp {
constexpr std::int64_t phaseOne = 1LL << 20;
constexpr int residualScale = 1; // Clip each residual component to [-A,A]; step=2A/255.
inline std::int64_t residualClip(std::int64_t residual) {
  constexpr auto limit=255LL*residualScale*phaseOne;
  return std::max<std::int64_t>(-limit,std::min<std::int64_t>(limit,residual));
}
inline std::array<std::int64_t,2> amplitudeLimited(std::array<std::int64_t,2> v) {
  constexpr std::int64_t radius=255*phaseOne-1; // Strictly below A.
  if(v[0]>=-radius&&v[0]<=radius&&v[1]>=-radius&&v[1]<=radius&&v[0]*v[0]+v[1]*v[1]<=radius*radius)return v;
  const long double norm=std::hypot(static_cast<long double>(v[0]),static_cast<long double>(v[1]));
  for(auto& c:v)c=std::int64_t(static_cast<long double>(c)*radius/norm);
  // Correct any boundary rounding in floating-point normalization with an
  // exact integer check. This operation does not alter the stored final field.
  while(v[0]*v[0]+v[1]*v[1]>radius*radius) {
    auto& c=std::abs(v[0])>=std::abs(v[1])?v[0]:v[1];c+=c<0?1:-1;
  }
  return v;
}
inline std::int64_t restoredComplex(int residual, std::int64_t predictionQ20) {
  return (2LL * residual - 255) * residualScale * phaseOne + predictionQ20;
}
inline std::int64_t nearestDivide(std::int64_t a, std::int64_t b) {
  return a < 0 ? -((-a + b / 2) / b) : (a + b / 2) / b;
}
inline int sampleClip(std::int64_t v) {
  return int(std::max<std::int64_t>(0, std::min<std::int64_t>(255, v)));
}
inline int residualSample(int original, std::int64_t predictionQ20) {
  const auto residual = residualClip((2LL * original - 255) * phaseOne - predictionQ20);
  return sampleClip(nearestDivide(residual + 255LL * residualScale * phaseOne,
                                  2LL * residualScale * phaseOne));
}
inline int restoredSample(int residual, std::int64_t predictionQ20) {
  return sampleClip(nearestDivide((2LL * residual - 255) * residualScale * phaseOne
                                  + predictionQ20 + 255LL * phaseOne, 2 * phaseOne));
}
}
#endif
