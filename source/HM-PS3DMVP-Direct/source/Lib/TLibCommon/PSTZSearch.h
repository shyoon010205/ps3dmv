#ifndef PS_TZ_SEARCH_H
#define PS_TZ_SEARCH_H
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace ps3dmvp {
// Integer-only TZ-style estimation. The caller supplies complex SSE + MV rate.
// Unlike HM's SAD kernels, this never subsamples the Real/Imag distortion.
template<class Point, class Evaluate>
Point tzEstimate(Point seed, int radius, int minX, int maxX, int minY, int maxY,
                 Evaluate evaluate) {
  Point best = seed;
  double bestCost = evaluate(best);
  int bestDistance = 0;
  auto visit = [&](Point p, int distance) {
    if (p.x < minX || p.x > maxX || p.y < minY || p.y > maxY) return;
    const double cost = evaluate(p);
    if (cost < bestCost) { bestCost = cost; best = p; bestDistance = distance; }
  };
  auto diamond = [&](Point c, int d) {
    visit(Point{c.x-d,c.y},d); visit(Point{c.x+d,c.y},d);
    visit(Point{c.x,c.y-d},d); visit(Point{c.x,c.y+d},d);
    const int h = d == 1 ? 1 : d / 2;
    visit(Point{c.x-h,c.y-h},d); visit(Point{c.x+h,c.y-h},d);
    visit(Point{c.x-h,c.y+h},d); visit(Point{c.x+h,c.y+h},d);
  };
  for (int d=1; d<=radius; d*=2) diamond(seed,d);
  // Include the requested endpoint even when the radius is not a power of two.
  if (radius > 0 && (radius & (radius-1))) diamond(seed,radius);
  // HM TZSearch uses raster=5 after a distant improvement.
  if (bestDistance > 5) {
    for (int y=minY; y<=maxY; y+=5)
      for (int x=minX; x<=maxX; x+=5) visit(Point{x,y},5);
  }
  // Recenter the expanding star until it cannot improve; strictly decreasing
  // cached cost guarantees termination. Finish with all 8 integer neighbours.
  for (;;) {
    Point center=best;
    for (int d=1; d<=radius; d*=2) diamond(center,d);
    if (best.x==center.x && best.y==center.y) break;
  }
  return best;
}
}
#endif
