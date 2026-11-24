#ifndef PS3DMVP_H
#define PS3DMVP_H

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

class TComDataCU;
class TComPic;
class TComYuv;

// Research syntax v5: direct PS prediction + native signed residual coding. The two input planes use the SAME symmetric affine map:
// real/imag = (2 * sample - 255) * amplitude / 255. Plane 2 is zero.
namespace ps3dmvp {
struct Vector { int x = 0, y = 0; };
struct Motion {
  bool used = false;
  int direction = 0;
  int ref[2] = {-1, -1};
  int refPoc[2] = {0, 0};
  bool longTerm[2] = {false, false};
  Vector mv[2], mvd[2];
  int mvp[2] = {0, 0};
  int theta[2] = {0, 0};
  int origin = -1; // CTU-local Z index; history visits a leaf only once.

};
struct Config {
  bool enabled = false;
  std::string bitstream;
  std::string stats;
  std::string complexRecon; // Optional temporary float32 complex export, never .amp.
  int thetaDivisions = 16;
  int searchRange = 32;
  int candidates = 4;
  bool fullSearch = true;
  bool optimizedSearch = true; // Encoder-only, identical candidates/order/cost arithmetic.
};
struct Candidate { Motion motion; double cost = 0; };

Config& config();
bool enabled();
const Motion& emptyMotion();
void validateConfig();
void openEncoder();
void openDecoder();
void closeEncoder();
void closeDecoder();
void validatePicture(TComPic* pic);
void writePicture(TComPic* pic);
void beginDecodePicture(TComDataCU* ctu);
void decodeLeaf(TComDataCU* cu, unsigned part, unsigned depth);
void applyMotion(TComDataCU* cu, unsigned part, unsigned depth, const Motion& motion);
std::array<Vector, 2> predictors(TComDataCU* cu, unsigned part, unsigned depth, int list, int ref);
unsigned syntaxBits(TComDataCU* cu, const Motion& motion);
void makePrediction(TComDataCU* cu, TComYuv* destination);
void reconstructPicture(TComPic* pic);
void recordDirectTrial(double psCost, double nativeCost, double distortionDelta, double totalBitsDelta);
std::vector<Candidate> search(TComDataCU* cu, TComYuv* original, double lambda);
std::uint64_t realImagSse(TComYuv* original, TComYuv* reconstructed, unsigned width, unsigned height);
}
#endif
