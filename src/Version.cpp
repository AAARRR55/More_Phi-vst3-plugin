/*
 * More-Phi — Version.cpp
 * Sole translation unit that touches __DATE__ / __TIME__ to avoid
 * forcing recompilation of every TU that includes Version.h.
 */
#include "Version.h"

namespace more_phi {

const char* BUILD_DATE = __DATE__;
const char* BUILD_TIME = __TIME__;

} // namespace more_phi
