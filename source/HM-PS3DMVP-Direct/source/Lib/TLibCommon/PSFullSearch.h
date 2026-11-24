#ifndef PS_FULL_SEARCH_H
#define PS_FULL_SEARCH_H
namespace ps3dmvp {
// Inclusive integer-pixel scan. Caller's cache removes overlaps between MVP windows.
template<class Evaluate> void fullSearch(int minX,int maxX,int minY,int maxY,Evaluate evaluate) {
  for(int y=minY;y<=maxY;++y)for(int x=minX;x<=maxX;++x)evaluate(x,y);
}
}
#endif
