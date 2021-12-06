//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright © 2021 ANSSI. All Rights Reserved.
//
// Author(s): fabienfl
//

#include "LazyFileStreambuf.h"

namespace Orc {
namespace Command {

template class LazyFileStreambuf<char>;
template class LazyFileStreambuf<wchar_t>;

}  // namespace Command
}  // namespace Orc
