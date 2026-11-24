#ifndef PS_PREDICTION_MATH_H
#define PS_PREDICTION_MATH_H
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ps3dmvp {
constexpr std::int64_t phaseOne = 1LL << 20;
inline std::array<std::int64_t,2> amplitudeLimited(std::array<std::int64_t,2> v) {
  constexpr std::int64_t radius=255*phaseOne-1;
  if(v[0]>=-radius&&v[0]<=radius&&v[1]>=-radius&&v[1]<=radius&&v[0]*v[0]+v[1]*v[1]<=radius*radius)return v;
  const long double norm=std::hypot(static_cast<long double>(v[0]),static_cast<long double>(v[1]));
  for(auto& c:v)c=std::int64_t(static_cast<long double>(c)*radius/norm);
  while(v[0]*v[0]+v[1]*v[1]>radius*radius) {
    auto& c=std::abs(v[0])>=std::abs(v[1])?v[0]:v[1];c+=c<0?1:-1;
  }
  return v;
}
inline int predictionSample(std::int64_t predictionQ20) {
  // Same original mapping step 2A/255. Only the prediction is mapped to 8-bit;
  // its difference from the input stays a signed HM residual without clipping.
  const auto shifted=predictionQ20+255LL*phaseOne;
  const auto rounded=shifted<0?-((-shifted+phaseOne)/(2*phaseOne)):(shifted+phaseOne)/(2*phaseOne);
  return int(std::max<std::int64_t>(0,std::min<std::int64_t>(255,rounded)));
}
}
#endif
