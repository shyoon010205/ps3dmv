#ifndef HM_MODE_AREA_H
#define HM_MODE_AREA_H
#include <string>
class TComPic;
namespace modearea {
std::string& outputPath();
void writePicture(TComPic* picture);
}
#endif
