// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>

namespace adhoc
{

auto cstr(auto ptr)
{
  return reinterpret_cast<char const*>(ptr);
}

constexpr std::size_t bounded_strlen(let* data, std::size_t max) noexcept
{
  auto len = 0zu;
  while (len != max && data[len] != 0) {
    ++len;
  }
  return len;
}

}  // namespace adhoc
