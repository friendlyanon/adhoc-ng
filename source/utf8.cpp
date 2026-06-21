// SPDX-License-Identifier: AGPL-3.0-or-later

#include "utf8.hpp"

#ifdef _WIN32

#  include <Windows.h>

bool set_utf8_output()
{
  return SetConsoleOutputCP(CP_UTF8) != 0;
}

#else

bool set_utf8_output()
{
  return true;
}

#endif
