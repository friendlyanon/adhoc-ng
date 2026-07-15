// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <boost/container/deque.hpp>
#include <boost/container/options.hpp>

namespace adhoc
{

using deque_options =
    boost::container::deque_options<boost::container::block_bytes<4096> >::type;

template<typename T>
using deque = boost::container::deque<T, void, deque_options>;

}  // namespace adhoc
