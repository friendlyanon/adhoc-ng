// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace adhoc
{

template<typename... Bases>
struct overload : Bases...
{
  using is_transparent = void;
  using Bases::operator()...;
};

using transparent_string_hash =
    overload<std::hash<std::string>, std::hash<std::string_view>>;

}  // namespace adhoc
