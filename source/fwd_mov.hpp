// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <type_traits>

#define MOV(...) \
  static_cast<std::remove_reference_t<decltype(__VA_ARGS__)>&&>(__VA_ARGS__)

#define FWD(...) static_cast<decltype(__VA_ARGS__)&&>(__VA_ARGS__)
