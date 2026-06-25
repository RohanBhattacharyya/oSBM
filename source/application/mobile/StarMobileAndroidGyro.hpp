#pragma once

#include <array>

namespace Star {

#ifdef STAR_SYSTEM_ANDROID
bool takeAndroidGyroData(std::array<float, 3>& data);
#endif

}
