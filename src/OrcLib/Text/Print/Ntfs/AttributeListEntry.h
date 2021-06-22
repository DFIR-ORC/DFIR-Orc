//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright � 2020 ANSSI. All Rights Reserved.
//
// Author(s): fabienfl (ANSSI)
//

#pragma once

#include "Text/Print.h"

#include <AttributeList.h>

#include "Utils/TypeTraits.h"

namespace Orc {
namespace Text {

template <>
struct Printer<AttributeListEntry>
{
    template <typename T>
    static void Output(Orc::Text::Tree<T>& root, const AttributeListEntry& entry)
    {
        std::error_code ec;

        const auto attributeName =
            Utf16ToUtf8(std::wstring_view(entry.AttributeName(), entry.AttributeNameLength()), ec);
        assert(!ec);

        const auto attributeType = Utf16ToUtf8(std::wstring_view(entry.TypeStr()), ec);
        assert(!ec);

        root.AddWithoutEOL(
            "Type: '{}', Name: '{}', Form: '{}', Id: {:02}",
            entry.Instance(),
            attributeType,
            attributeName,
            entry.FormCode() == RESIDENT_FORM ? "R" : "NR");

        if (entry.LowestVCN() > 0)
        {
            root.Append(", LowestVCN={:#018x}\n", entry.LowestVCN());
        }
        else
        {
            root.AddEOL();
        }
    }
};

}  // namespace Text
}  // namespace Orc
