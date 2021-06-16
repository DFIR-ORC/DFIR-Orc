//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright © 2011-2019 ANSSI. All Rights Reserved.
//
// Author(s): Jean Gautier (ANSSI)
//

#include "stdafx.h"

#include "FileFind.h"

#include "CryptoHashStream.h"

#include "WideAnsi.h"
#include "ParameterCheck.h"
#include "ConfigFile.h"
#include "MFTWalker.h"
#include "DevNullStream.h"

#include "SnapshotVolumeReader.h"

#include "TableOutputWriter.h"
#include "StructuredOutputWriter.h"

#include "Convert.h"

#include "ConfigFile_Common.h"

#include "SystemDetails.h"

#include <sstream>
#include <Shlwapi.h>
#include <iomanip>

#include <boost\algorithm\searching\boyer_moore.hpp>

#include <fmt/format.h>

constexpr const unsigned int FILESPEC_FILENAME_INDEX = 1;
constexpr const unsigned int FILESPEC_SPEC_INDEX = 3;
constexpr const unsigned int FILESPEC_SUBNAME_INDEX = 4;

using namespace std;
using namespace Orc;

std::wregex& FileFind::DOSPattern()
{
    static std::wregex g_DOSPattern(std::wstring(L"\\*|\\?"));
    return g_DOSPattern;
}

std::wregex& FileFind::RegexPattern()
{
    static std::wregex g_RegexPattern(std::wstring(L"\\*|\\?|\\:|\\+|\\{|\\}|\\[|\\]|\\(|\\)"));
    return g_RegexPattern;
}

std::wregex& FileFind::RegexOnlyPattern()
{
    static std::wregex g_RegexOnlyPattern(std::wstring(L"\\:|\\+|\\{|\\}|\\[|\\]|\\(|\\)"));
    return g_RegexOnlyPattern;
}

std::wregex& FileFind::FileSpecPattern()
{
    static std::wregex g_FileSpecPattern(std::wstring(L"([^:#]*)((#|:)(.*))?"));
    return g_FileSpecPattern;
}

HRESULT Orc::FileFind::Match::AddAttributeMatch(
    const logger& pLog,
    const std::shared_ptr<Orc::VolumeReader> pVolReader,
    const std::shared_ptr<MftRecordAttribute>& pAttribute,
    std::optional<MatchingRuleCollection> matchedRules)
{
    HRESULT hr = E_FAIL;
    for (auto& matched_attr : MatchingAttributes)
    {
        if (matched_attr.Type == pAttribute->Header()->TypeCode
            && matched_attr.InstanceID == pAttribute->Header()->Instance
            && matched_attr.AttrName.length() == pAttribute->NameLength())
        {
            if (!matched_attr.AttrName.compare(
                0L, pAttribute->NameLength(), pAttribute->NamePtr(), pAttribute->NameLength()))
            {
                // Attribute is already added to the matching list
                if (matchedRules.has_value())
                {
                    if (matched_attr.YaraRules.has_value())
                        matched_attr.YaraRules.value().insert(
                            end(matched_attr.YaraRules.value()),
                            begin(matchedRules.value()),
                            end(matchedRules.value()));
                    else
                        std::swap(matched_attr.YaraRules, matchedRules);
                }

                return S_OK;
            }
        }
    }

    hr = pAttribute->GetStreams(pLog, pVolReader);
    if (FAILED(hr))
    {
        return hr;
    }

    AttributeMatch aMatch(pAttribute);
    pAttribute->DataSize(pVolReader, aMatch.DataSize);
    std::swap(aMatch.YaraRules, matchedRules);
    MatchingAttributes.push_back(std::move(aMatch));
    return S_OK;
}

HRESULT Orc::FileFind::Match::AddAttributeMatch(
    const std::shared_ptr<MftRecordAttribute>& pAttribute,
    std::optional<MatchingRuleCollection> matchedRules)
{
    for (auto& matched_attr : MatchingAttributes)
    {
        if (matched_attr.Type == pAttribute->Header()->TypeCode
            && matched_attr.InstanceID == pAttribute->Header()->Instance
            && matched_attr.AttrName.length() == pAttribute->NameLength())
        {
            if (!matched_attr.AttrName.compare(
                0L, pAttribute->NameLength(), pAttribute->NamePtr(), pAttribute->NameLength()))
            {
                // Attribute is already added to the matching list
                if (matchedRules.has_value())
                {
                    if (matched_attr.YaraRules.has_value())
                        matched_attr.YaraRules.value().insert(
                            end(matched_attr.YaraRules.value()),
                            begin(matchedRules.value()),
                            end(matchedRules.value()));
                    else
                        std::swap(matched_attr.YaraRules, matchedRules);
                }

                return S_OK;
            }
        }
    }
    auto aMatch = AttributeMatch(pAttribute);
    std::swap(aMatch.YaraRules, matchedRules);
    MatchingAttributes.push_back(std::move(aMatch));
    return S_OK;
}

HRESULT FileFind::Match::GetMatchFullName(
    const FileFind::Match::NameMatch& nameMatch,
    const FileFind::Match::AttributeMatch& attrMatch,
    std::wstring& strName)
{
    if (attrMatch.AttrName.empty())
    {
        strName = nameMatch.FullPathName;
    }
    else
    {
        strName.reserve(nameMatch.FullPathName.length() + 1 + attrMatch.AttrName.length());

        strName.assign(nameMatch.FullPathName);
        switch (attrMatch.Type)
        {
        case $DATA:
            strName.append(L":");
            break;
        default:
            strName.append(L"#");
            break;
        }
        strName.append(attrMatch.AttrName);
    }
    return S_OK;
}

HRESULT FileFind::Match::GetMatchFullNames(std::vector<std::wstring>& strNames)
{
    for (auto name_it = begin(MatchingNames); name_it != end(MatchingNames); ++name_it)
    {
        for (auto data_it = begin(MatchingAttributes); data_it != end(MatchingAttributes); ++data_it)
        {
            std::wstring strName;

            if (SUCCEEDED(GetMatchFullName(*name_it, *data_it, strName)))
                strNames.push_back(std::move(strName));
        }
    }
    return S_OK;
}

HRESULT FileFind::Match::Write(const logger& pLog, ITableOutput& output)
{
    wstring strMatchDescr = Term->GetDescription();

    GUID SnapshotID;
    auto reader = std::dynamic_pointer_cast<SnapshotVolumeReader>(VolumeReader);
    if (reader)
    {
        SnapshotID = reader->GetSnapshotID();
    }
    else
    {
         SnapshotID = GUID_NULL;
    }


    if (MatchingAttributes.empty())
    {
        for (auto name_it = begin(MatchingNames); name_it != end(MatchingNames); ++name_it)
        {
            SystemDetails::WriteComputerName(output);

            output.WriteInteger(VolumeReader->VolumeSerialNumber());

            output.WriteString(name_it->FullPathName.c_str());

            {
                LARGE_INTEGER* pLI = (LARGE_INTEGER*) & (FRN);
                output.WriteInteger((DWORDLONG)pLI->QuadPart);
            }
            {
                LARGE_INTEGER* pLI = (LARGE_INTEGER*) & (name_it->FILENAME()->ParentDirectory);
                output.WriteInteger((DWORDLONG)pLI->QuadPart);
            }

            output.WriteNothing();

            output.WriteString(strMatchDescr.c_str());

            if (StandardInformation)
            {
                output.WriteFileTime(StandardInformation->CreationTime);
                output.WriteFileTime(StandardInformation->LastModificationTime);
                output.WriteFileTime(StandardInformation->LastAccessTime);
                output.WriteFileTime(StandardInformation->LastChangeTime);
            }
            else
            {
                output.WriteNothing();
                output.WriteNothing();
                output.WriteNothing();
                output.WriteNothing();
            }
            if (name_it->FILENAME())
            {
                output.WriteFileTime(name_it->FILENAME()->Info.CreationTime);
                output.WriteFileTime(name_it->FILENAME()->Info.LastModificationTime);
                output.WriteFileTime(name_it->FILENAME()->Info.LastAccessTime);
                output.WriteFileTime(name_it->FILENAME()->Info.LastChangeTime);
            }
            else
            {
                output.WriteNothing();
                output.WriteNothing();
                output.WriteNothing();
                output.WriteNothing();
            }

            output.WriteNothing();
            output.WriteNothing();
            output.WriteNothing();

            output.WriteGUID(SnapshotID);
            
            output.WriteEndOfLine();
        }
    }
    else
    {
        for (const auto& data_match : MatchingAttributes)
        {
            for (auto name_it = begin(MatchingNames); name_it != end(MatchingNames); ++name_it)
            {
                SystemDetails::WriteComputerName(output);
                output.WriteInteger(VolumeReader->VolumeSerialNumber());

                output.WriteString(name_it->FullPathName.c_str());
                {
                    LARGE_INTEGER* pLI = (LARGE_INTEGER*) & (FRN);
                    output.WriteInteger((DWORDLONG)pLI->QuadPart);
                }
                {
                    LARGE_INTEGER* pLI = (LARGE_INTEGER*) & (name_it->FILENAME()->ParentDirectory);
                    output.WriteInteger((DWORDLONG)pLI->QuadPart);
                }


                output.WriteFileSize(data_match.DataSize);

                output.WriteString(strMatchDescr.c_str());

                output.WriteFileTime(StandardInformation->CreationTime);
                output.WriteFileTime(StandardInformation->LastModificationTime);
                output.WriteFileTime(StandardInformation->LastAccessTime);
                output.WriteFileTime(StandardInformation->LastChangeTime);

                output.WriteFileTime(name_it->FILENAME()->Info.CreationTime);
                output.WriteFileTime(name_it->FILENAME()->Info.LastModificationTime);
                output.WriteFileTime(name_it->FILENAME()->Info.LastAccessTime);
                output.WriteFileTime(name_it->FILENAME()->Info.LastChangeTime);

                output.WriteBytes(data_match.MD5);
                output.WriteBytes(data_match.SHA1);
                output.WriteBytes(data_match.SHA256);

                output.WriteGUID(SnapshotID);

                output.WriteEndOfLine();
            }
        }
    }
    return S_OK;
}

HRESULT FileFind::Match::Write(
    const logger& pLog,
    IStructuredOutput& pWriter,
    LPCWSTR szElement)
{
    wstring strMatchDescr = Term->GetDescription();

    pWriter.BeginElement(szElement);

    pWriter.WriteNamed(L"description", strMatchDescr.c_str());

    pWriter.BeginElement(L"record");
    {
        LARGE_INTEGER* pLI = (LARGE_INTEGER*)&FRN;
        pWriter.WriteNamed(L"frn", (ULONGLONG)pLI->QuadPart, true);

        pWriter.WriteNamed(L"volume_id", VolumeReader->VolumeSerialNumber(), true);

        auto reader = std::dynamic_pointer_cast<SnapshotVolumeReader>(VolumeReader);
        if (reader)
        {
            pWriter.WriteNamed(L"snapshot_id", reader->GetSnapshotID());
        }
        else
        {
            pWriter.WriteNamed(L"snapshot_id", GUID_NULL);
        }

        if (StandardInformation != nullptr)
        {
            pWriter.BeginElement(L"standardinformation");
            {
                pWriter.WriteNamedFileTime(L"creation", StandardInformation->CreationTime);
                pWriter.WriteNamedFileTime(L"lastmodification", StandardInformation->LastModificationTime);
                pWriter.WriteNamedFileTime(L"lastaccess", StandardInformation->LastAccessTime);
                pWriter.WriteNamedFileTime(L"lastentrychange", StandardInformation->LastChangeTime);
                pWriter.WriteNamedAttributes(L"attributes", StandardInformation->FileAttributes);
            }
            pWriter.EndElement(L"standardinformation");
        }

        if (MatchingAttributes.empty())
        {
            pWriter.BeginCollection(L"i30");
            for (auto name_it = begin(MatchingNames); name_it != end(MatchingNames); ++name_it)
            {
                pWriter.BeginElement(nullptr);
                pWriter.WriteNamed(L"fullname", name_it->FullPathName.c_str());

                LARGE_INTEGER* pParentLI = (LARGE_INTEGER*)&name_it->FILENAME()->ParentDirectory;
                pWriter.WriteNamed(L"parentfrn", (ULONGLONG)pParentLI->QuadPart, true);

                pWriter.WriteNamedFileTime(L"creation", name_it->FILENAME()->Info.CreationTime);
                pWriter.WriteNamedFileTime(L"lastmodification", name_it->FILENAME()->Info.LastModificationTime);
                pWriter.WriteNamedFileTime(L"lastaccess", name_it->FILENAME()->Info.LastAccessTime);
                pWriter.WriteNamedFileTime(L"lastentrychange", name_it->FILENAME()->Info.LastChangeTime);
                pWriter.EndElement(nullptr);
            }
            pWriter.EndCollection(L"i30");
        }
        else
        {
            pWriter.BeginCollection(L"filename");
            for (auto name_it = begin(MatchingNames); name_it != end(MatchingNames); ++name_it)
            {
                pWriter.BeginElement(nullptr);
                {
                    pWriter.WriteNamed(L"fullname", name_it->FullPathName.c_str());

                    LARGE_INTEGER* pParentLI = (LARGE_INTEGER*)&name_it->FILENAME()->ParentDirectory;
                    pWriter.WriteNamed(L"parentfrn", (ULONGLONG)pParentLI->QuadPart, true);

                    pWriter.WriteNamedFileTime(L"creation", name_it->FILENAME()->Info.CreationTime);
                    pWriter.WriteNamedFileTime(L"lastmodification", name_it->FILENAME()->Info.LastModificationTime);
                    pWriter.WriteNamedFileTime(L"lastaccess", name_it->FILENAME()->Info.LastAccessTime);
                    pWriter.WriteNamedFileTime(L"lastentrychange", name_it->FILENAME()->Info.LastChangeTime);
                }
                pWriter.EndElement(nullptr);
            }
            pWriter.EndCollection(L"filename");

            pWriter.BeginCollection(L"data");
            for (auto data_it = begin(MatchingAttributes); data_it != end(MatchingAttributes); ++data_it)
            {
                pWriter.BeginElement(nullptr);
                {
                    pWriter.WriteNamed(L"filesize", data_it->DataSize);
                    pWriter.WriteNamed(L"MD5", data_it->MD5, false);
                    pWriter.WriteNamed(L"SHA1", data_it->SHA1, false);
                    pWriter.WriteNamed(L"SHA256", data_it->SHA256, false);
                }
                pWriter.EndElement(nullptr);
            }
            pWriter.EndCollection(L"data");
        }
    }
    pWriter.EndElement(L"record");
    pWriter.EndElement(szElement);

    return S_OK;
}

HRESULT Orc::FileFind::CheckYara()
{
    std::vector<std::string> allrules;

    for (const auto& term : m_AllTerms)
    {
        if (term->Required & SearchTerm::Criteria::YARA)
        {
            allrules.insert(end(allrules), begin(term->YaraRules), end(term->YaraRules));
        }
    }

    std::sort(begin(allrules), end(allrules));
    auto new_end = std::unique(begin(allrules), end(allrules));
    allrules.erase(new_end, end(allrules));

    auto [scanned, notscanned] = m_YaraScan->ScannedRules(allrules);

    if (!notscanned.empty())
    {
        for (const auto& rule : notscanned)
        {
            for (const auto& term : m_AllTerms)
            {
                for (const auto& term_rule : term->YaraRules)
                {
                    if (term_rule == rule)
                    {
                        log::Warning(
                            _L_,
                            E_INVALIDARG,
                            L"Term \"%s\"'s rule spec %S does not match any rule in yara\r\n",
                            term->GetDescription().c_str(),
                            term_rule.c_str());
                    }
                }
            }
        }
    }

    return S_OK;
}

std::shared_ptr<FileFind::SearchTerm> FileFind::GetSearchTermFromConfig(const ConfigItem& item, logger pLog)
{
    HRESULT hr = E_FAIL;

    std::shared_ptr<FileFind::SearchTerm> fs = make_shared<FileFind::SearchTerm>();

    if (item[CONFIG_FILEFIND_NAME])
    {
        fs->FileName = item[CONFIG_FILEFIND_NAME];
        fs->Required |= FileFind::SearchTerm::NAME_EXACT;
    }
    if (item[CONFIG_FILEFIND_NAME_MATCH])
    {
        fs->FileName = item[CONFIG_FILEFIND_NAME_MATCH];
        fs->Required |= FileFind::SearchTerm::NAME_MATCH;
    }
    if (item[CONFIG_FILEFIND_NAME_REGEX])
    {
        fs->FileName = item[CONFIG_FILEFIND_NAME_REGEX];
        fs->FileNameRegEx.assign((const std::wstring&)item[CONFIG_FILEFIND_NAME_REGEX], regex_constants::icase);
        fs->Required |= FileFind::SearchTerm::NAME_REGEX;
    }
    if (item[CONFIG_FILEFIND_PATH])
    {
        fs->Path = item[CONFIG_FILEFIND_PATH];
        fs->Required |= FileFind::SearchTerm::PATH_EXACT;
    }
    if (item[CONFIG_FILEFIND_PATH_MATCH])
    {
        fs->Path = item[CONFIG_FILEFIND_PATH_MATCH];
        fs->Required |= FileFind::SearchTerm::PATH_MATCH;
    }
    if (item[CONFIG_FILEFIND_PATH_REGEX])
    {
        fs->Path = item[CONFIG_FILEFIND_PATH_REGEX];
        fs->PathRegEx.assign((const std::wstring&)item[CONFIG_FILEFIND_PATH_REGEX], regex_constants::icase);
        fs->Required |= FileFind::SearchTerm::PATH_REGEX;
    }
    if (item[CONFIG_FILEFIND_EA])
    {
        fs->EAName = item[CONFIG_FILEFIND_EA];
        fs->Required |= FileFind::SearchTerm::EA_EXACT;
    }
    if (item[CONFIG_FILEFIND_EA_MATCH])
    {
        fs->EAName = item[CONFIG_FILEFIND_EA_MATCH];
        fs->Required |= FileFind::SearchTerm::EA_MATCH;
    }
    if (item[CONFIG_FILEFIND_EA_REGEX])
    {
        fs->EAName = item[CONFIG_FILEFIND_EA_REGEX];
        fs->EANameRegEx.assign((const std::wstring&)item[CONFIG_FILEFIND_EA_REGEX], regex_constants::icase);
        fs->Required |= FileFind::SearchTerm::EA_REGEX;
    }
    if (item[CONFIG_FILEFIND_ADS])
    {
        fs->ADSName = item[CONFIG_FILEFIND_ADS];
        fs->Required |= FileFind::SearchTerm::ADS_EXACT;
    }
    if (item[CONFIG_FILEFIND_ADS_MATCH])
    {
        fs->ADSName = item[CONFIG_FILEFIND_ADS_MATCH];
        fs->Required |= FileFind::SearchTerm::ADS_MATCH;
    }
    if (item[CONFIG_FILEFIND_ADS_REGEX])
    {
        fs->ADSName = item[CONFIG_FILEFIND_ADS_REGEX];
        fs->ADSNameRegEx.assign((const std::wstring&)item[CONFIG_FILEFIND_ADS_REGEX], regex_constants::icase);
        fs->Required |= FileFind::SearchTerm::ADS_REGEX;
    }
    if (item[CONFIG_FILEFIND_EA])
    {
        fs->EAName = item[CONFIG_FILEFIND_EA];
        fs->Required |= FileFind::SearchTerm::EA;
    }
    if (item[CONFIG_FILEFIND_EA_MATCH])
    {
        fs->EAName = item[CONFIG_FILEFIND_EA_MATCH];
        fs->Required |= FileFind::SearchTerm::EA_MATCH;
    }
    if (item[CONFIG_FILEFIND_EA_REGEX])
    {
        fs->EAName = item[CONFIG_FILEFIND_EA_REGEX];
        fs->EANameRegEx.assign((const std::wstring&)item[CONFIG_FILEFIND_EA_REGEX], regex_constants::icase);
        fs->Required |= FileFind::SearchTerm::EA_REGEX;
    }
    if (item[CONFIG_FILEFIND_ATTR_NAME])
    {
        fs->AttrName = item[CONFIG_FILEFIND_ATTR_NAME];
        fs->Required |= FileFind::SearchTerm::ATTR_NAME_EXACT;
    }
    if (item[CONFIG_FILEFIND_ATTR_MATCH])
    {
        fs->AttrName = item[CONFIG_FILEFIND_ATTR_MATCH];
        fs->Required |= FileFind::SearchTerm::ATTR_NAME_MATCH;
    }
    if (item[CONFIG_FILEFIND_ATTR_REGEX])
    {
        fs->AttrName = item[CONFIG_FILEFIND_ATTR_REGEX];
        fs->AttrNameRegEx.assign((const std::wstring&)item[CONFIG_FILEFIND_ATTR_REGEX], regex_constants::icase);
        fs->Required |= FileFind::SearchTerm::ATTR_NAME_REGEX;
    }
    if (item[CONFIG_FILEFIND_ATTR_TYPE])
    {
        using namespace std::string_view_literals;

        fs->Required = static_cast<FileFind::SearchTerm::Criteria>(fs->Required | FileFind::SearchTerm::ATTR_TYPE);

        if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$STANDARD_INFORMATION"sv)
            fs->dwAttrType = $STANDARD_INFORMATION;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$ATTRIBUTE_LIST"sv)
            fs->dwAttrType = $ATTRIBUTE_LIST;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$FILE_NAME"sv)
            fs->dwAttrType = $FILE_NAME;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$OBJECT_ID"sv)
            fs->dwAttrType = $OBJECT_ID;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$SECURITY_DESCRIPTOR"sv)
            fs->dwAttrType = $SECURITY_DESCRIPTOR;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$VOLUME_NAME"sv)
            fs->dwAttrType = $VOLUME_NAME;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$VOLUME_INFORMATION"sv)
            fs->dwAttrType = $VOLUME_INFORMATION;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$DATA"sv)
            fs->dwAttrType = $DATA;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$INDEX_ROOT"sv)
            fs->dwAttrType = $INDEX_ROOT;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$INDEX_ALLOCATION"sv)
            fs->dwAttrType = $INDEX_ALLOCATION;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$BITMAP"sv)
            fs->dwAttrType = $BITMAP;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$REPARSE_POINT"sv)
            fs->dwAttrType = $REPARSE_POINT;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$EA_INFORMATION"sv)
            fs->dwAttrType = $EA_INFORMATION;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$EA"sv)
            fs->dwAttrType = $EA;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$LOGGED_UTILITY_STREAM"sv)
            fs->dwAttrType = $LOGGED_UTILITY_STREAM;
        else if ((const std::wstring&)item[CONFIG_FILEFIND_ATTR_TYPE] == L"$FIRST_USER_DEFINED_ATTRIBUTE"sv)
            fs->dwAttrType = $FIRST_USER_DEFINED_ATTRIBUTE;
        else
        {
            fs->dwAttrType = _wtoi(item[CONFIG_FILEFIND_ATTR_TYPE].c_str());
            if (fs->dwAttrType == 0)
            {
                log::Warning(
                    pLog,
                    hr,
                    L"Invalid attribute type passed (%s), ignored\r\n",
                    item[CONFIG_FILEFIND_ATTR_TYPE].c_str());
                fs->Required = static_cast<FileFind::SearchTerm::Criteria>(fs->Required & ~FileFind::SearchTerm::ATTR_TYPE);
            }
        }
        
    }
    if (item[CONFIG_FILEFIND_SIZE])
    {
        try
        {
            fs->SizeEQ = (DWORD64)item[CONFIG_FILEFIND_SIZE];
            fs->Required |= FileFind::SearchTerm::SIZE_EQ;
        }
        catch (Orc::Exception & e)
        {
            log::Warning(pLog, e.GetHRESULT(), L"%S", e.what());
            log::Warning(
                pLog, hr, L"Invalid file size passed (%s), ignored\r\n", item[CONFIG_FILEFIND_SIZE].c_str());
        }
    }
    if (item[CONFIG_FILEFIND_SIZE_GT])
    {
        try
        {
            fs->SizeG = (DWORD64)item[CONFIG_FILEFIND_SIZE_GT];
            fs->Required |= FileFind::SearchTerm::SIZE_GT;
        }
        catch (Orc::Exception & e)
        {
            log::Warning(pLog, e.GetHRESULT(), L"%S", e.what());
            log::Warning(
                pLog,
                hr,
                L"Invalid file size_gt passed (%s), ignored\r\n",
                item[CONFIG_FILEFIND_SIZE_GT].c_str());
        }
    }
    if (item[CONFIG_FILEFIND_SIZE_GE])
    {
        try
        {
            fs->SizeG = (DWORD64)item[CONFIG_FILEFIND_SIZE_GE];
            fs->Required |= FileFind::SearchTerm::SIZE_GE;
        }
        catch (Orc::Exception & e)
        {
            log::Warning(pLog, e.GetHRESULT(), L"%S", e.what());
            log::Warning(
                pLog,
                hr,
                L"Invalid file size_ge passed (%s), ignored\r\n",
                item[CONFIG_FILEFIND_SIZE_GE].c_str());
        }
    }
    if (item[CONFIG_FILEFIND_SIZE_LT])
    {
        try
        {
            fs->SizeL = (DWORD64)item[CONFIG_FILEFIND_SIZE_LT];
            fs->Required |= FileFind::SearchTerm::SIZE_LT;
        }
        catch (Orc::Exception & e)
        {
            log::Warning(pLog, e.GetHRESULT(), L"%S", e.what());
            log::Warning(
                pLog,
                hr,
                L"Invalid file size_lt passed (%s), ignored\r\n",
                item[CONFIG_FILEFIND_SIZE_LT].c_str());
        }
    }
    if (item[CONFIG_FILEFIND_SIZE_LE])
    {
        try
        {
            fs->SizeL = (DWORD64)item[CONFIG_FILEFIND_SIZE_LE];
            fs->Required |= FileFind::SearchTerm::SIZE_LE;
        }
        catch (Orc::Exception & e)
        {
            log::Warning(pLog, e.GetHRESULT(), L"%S", e.what());
            log::Warning(
                pLog,
                hr,
                L"Invalid file size_le passed (%s), ignored\r\n",
                item[CONFIG_FILEFIND_SIZE_LE].c_str());
        }
    }
    if (item[CONFIG_FILEFIND_MD5])
    {
        fs->MD5.SetCount(BYTES_IN_MD5_HASH);
        if (SUCCEEDED(
            hr = GetBytesFromHexaString(
                item[CONFIG_FILEFIND_MD5].c_str(),
                (DWORD)item[CONFIG_FILEFIND_MD5].size(),
                fs->MD5.GetData(),
                BYTES_IN_MD5_HASH)))
            fs->Required |= FileFind::SearchTerm::DATA_MD5;
        else
        {
            log::Warning(
                pLog,
                hr,
                L"Invalid hexa string passed as md5 (%s), ignored\r\n",
                item[CONFIG_FILEFIND_MD5].c_str());
        }
    }
    if (item[CONFIG_FILEFIND_SHA1])
    {
        fs->SHA1.SetCount(BYTES_IN_SHA1_HASH);
        if (SUCCEEDED(
            hr = GetBytesFromHexaString(
                item[CONFIG_FILEFIND_SHA1].c_str(),
                (DWORD)item[CONFIG_FILEFIND_SHA1].size(),
                fs->SHA1.GetData(),
                BYTES_IN_SHA1_HASH)))
            fs->Required |= FileFind::SearchTerm::DATA_SHA1;
        else
        {
            log::Warning(
                pLog,
                hr,
                L"Invalid hexa string passed as sha1 (%s), ignored\r\n",
                item[CONFIG_FILEFIND_SHA1].c_str());
        }
    }
    if (item[CONFIG_FILEFIND_SHA256])
    {
        fs->SHA256.SetCount(BYTES_IN_SHA256_HASH);
        if (SUCCEEDED(
            hr = GetBytesFromHexaString(
                item[CONFIG_FILEFIND_SHA256].c_str(),
                (DWORD)item[CONFIG_FILEFIND_SHA256].size(),
                fs->SHA256.GetData(),
                BYTES_IN_SHA256_HASH)))
            fs->Required |= FileFind::SearchTerm::DATA_SHA256;
        else
        {
            log::Warning(
                pLog,
                hr,
                L"Invalid hexa string passed as sha256 (%s), ignored\r\n",
                item[CONFIG_FILEFIND_SHA256].c_str());
        }
    }
    if (item[CONFIG_FILEFIND_CONTAINS])
    {
        if (SUCCEEDED(
            hr = WideToAnsi(
                pLog,
                item[CONFIG_FILEFIND_CONTAINS].c_str(),
                (DWORD)item[CONFIG_FILEFIND_CONTAINS].size(),
                fs->Contains)))
        {
            fs->Required |= FileFind::SearchTerm::CONTAINS;
        }
        else
        {
            log::Warning(
                pLog,
                hr,
                L"string (%s) passed as binstring could not be converted to ANSI, ignored\r\n",
                item[CONFIG_FILEFIND_CONTAINS].c_str());
        }
    }
    if (item[CONFIG_FILEFIND_CONTAINS_HEX])
    {
        if (SUCCEEDED(
            hr = GetBytesFromHexaString(
                item[CONFIG_FILEFIND_CONTAINS_HEX].c_str(),
                (DWORD)item[CONFIG_FILEFIND_CONTAINS_HEX].size(),
                fs->Contains)))
        {
            fs->Required |= FileFind::SearchTerm::CONTAINS;
            fs->bContainsIsHex = true;
        }
        else
        {
            log::Warning(
                pLog,
                hr,
                L"Invalid hexa string passed as binstring (%s), ignored\r\n",
                item[CONFIG_FILEFIND_CONTAINS_HEX].c_str());
        }
    }
    if (item[CONFIG_FILEFIND_HEADER])
    {
        if (SUCCEEDED(
            hr = WideToAnsi(
                pLog,
                item[CONFIG_FILEFIND_HEADER].c_str(),
                (DWORD)item[CONFIG_FILEFIND_HEADER].size(),
                fs->Header)))
        {
            fs->HeaderLen = (DWORD)fs->Header.GetCount();
            fs->Required |= FileFind::SearchTerm::HEADER;
        }
        else
        {
            log::Error(
                pLog,
                hr,
                L"WARNING: string (%s) passed as header string could not be converted to ANSI, ignored\r\n",
                item[CONFIG_FILEFIND_HEADER].c_str());
        }
    }
    if (item[CONFIG_FILEFIND_HEADER_HEX])
    {
        if (SUCCEEDED(
            hr = GetBytesFromHexaString(
                item[CONFIG_FILEFIND_HEADER_HEX].c_str(),
                (DWORD)item[CONFIG_FILEFIND_HEADER_HEX].size(),
                fs->Header)))
        {
            fs->HeaderLen = (DWORD)fs->Header.GetCount();
            fs->Required |= FileFind::SearchTerm::HEADER_HEX;
        }
        else
        {
            log::Warning(
                pLog,
                hr,
                L"Invalid hexa string passed as header (%s), ignored\r\n",
                item[CONFIG_FILEFIND_HEADER_HEX].c_str(),
                hr);
        }
    }
    if (item[CONFIG_FILEFIND_HEADER_REGEX])
    {
        std::string ansiRegEx;
        if (FAILED(hr = WideToAnsi(pLog, (std::wstring_view)item[CONFIG_FILEFIND_HEADER_REGEX], ansiRegEx)))
        {
            log::Warning(
                pLog,
                hr,
                L"Invalid hexa string passed as header (%s), ignored\r\n",
                item[CONFIG_FILEFIND_HEADER_HEX].c_str(),
                hr);
        }
        else
        {
            fs->HeaderRegEx.assign(ansiRegEx, regex_constants::icase);
            fs->strHeaderRegEx = item[CONFIG_FILEFIND_HEADER_REGEX];
            fs->Required |= FileFind::SearchTerm::Criteria::HEADER_REGEX;
        }
    }
    if (item[CONFIG_FILEFIND_HEADER_LENGTH])
    {
        LARGE_INTEGER liSize = { 0, 0 };
        if (SUCCEEDED(GetIntegerFromArg(item[CONFIG_FILEFIND_HEADER_LENGTH].c_str(), liSize)))
        {
            fs->Header.SetCount(liSize.LowPart);
            fs->HeaderLen = liSize.LowPart;
        }
    }
    if (item[CONFIG_FILEFIND_YARA_RULE])
    {
        fs->YaraRulesSpec = item[CONFIG_FILEFIND_YARA_RULE];
        fs->YaraRules = YaraScanner::GetRulesSpec(pLog, fs->YaraRulesSpec.c_str());

        fs->Required |= FileFind::SearchTerm::YARA;
    }
    return fs;
}

HRESULT FileFind::AddTermsFromConfig(const ConfigItem& item)
{
    for (const auto& item : item.NodeList)
    {
        auto fs = GetSearchTermFromConfig(item, _L_);
        AddTerm(fs);
    }

    return S_OK;
}

HRESULT FileFind::AddExcludeTermsFromConfig(const ConfigItem& item)
{
    for (const auto& item : item.NodeList)
    {
        auto fs = GetSearchTermFromConfig(item, _L_);
        AddExcludeTerm(fs);
    }

    return S_OK;
}

wstring FileFind::SearchTerm::GetDescription() const
{
    wstringstream stream;
    wstring retval;
    bool bFirst = true;

    if (Required & SearchTerm::Criteria::NAME)
    {
        stream << L"Name spec is " << Name;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::NAME_EXACT)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"Name is " << FileName;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::NAME_MATCH)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"Name matches " << FileName;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::NAME_REGEX)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"Name matches regex " << FileName;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::PATH_EXACT)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"Path is " << Path;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::PATH_MATCH)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"Path matches " << Path;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::PATH_REGEX)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"Path matches regex " << Path;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::ADS_EXACT)
    {
        if (!bFirst)
            stream << L", ";
        stream << "ADS name is " << ADSName;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::ADS_MATCH)
    {
        if (!bFirst)
            stream << L", ";
        stream << "ADS name matches " << ADSName;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::ADS_REGEX)
    {
        if (!bFirst)
            stream << L", ";
        stream << "ADS name matches regex " << ADSName;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::EA_EXACT)
    {
        if (!bFirst)
            stream << L", ";
        stream << "EA name is " << EAName;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::EA_MATCH)
    {
        if (!bFirst)
            stream << L", ";
        stream << "EA name matches " << EAName;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::EA_REGEX)
    {
        if (!bFirst)
            stream << L", ";
        stream << "EA name matches regex " << EAName;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::ATTR_NAME_EXACT)
    {
        if (!bFirst)
            stream << L", ";
        stream << "Attribute name is " << AttrName;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::ATTR_NAME_MATCH)
    {
        if (!bFirst)
            stream << L", ";
        stream << "Attribute name matches " << AttrName;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::ATTR_NAME_REGEX)
    {
        if (!bFirst)
            stream << L", ";
        stream << "Attribute name matches regex " << AttrName;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::ATTR_TYPE)
    {
        if (!bFirst)
            stream << L", ";
        stream << "Attribute type is ";
        ;
        switch (dwAttrType)
        {
        case $STANDARD_INFORMATION:
            stream << "$STANDARD_INFORMATION";
            break;
        case $ATTRIBUTE_LIST:
            stream << "$ATTRIBUTE_LIST";
            break;
        case $FILE_NAME:
            stream << "$FILE_NAME";
            break;
        case $OBJECT_ID:
            stream << "$OBJECT_ID";
            break;
        case $SECURITY_DESCRIPTOR:
            stream << "$SECURITY_DESCRIPTOR";
            break;
        case $VOLUME_NAME:
            stream << "$VOLUME_NAME";
            break;
        case $VOLUME_INFORMATION:
            stream << "$VOLUME_INFORMATION";
            break;
        case $DATA:
            stream << "$DATA";
            break;
        case $INDEX_ROOT:
            stream << "$INDEX_ROOT";
            break;
        case $INDEX_ALLOCATION:
            stream << "$INDEX_ALLOCATION";
            break;
        case $BITMAP:
            stream << "$BITMAP";
            break;
        case $REPARSE_POINT:
            stream << "$REPARSE_POINT";
            break;
        case $EA_INFORMATION:
            stream << "$EA_INFORMATION";
            break;
        case $EA:
            stream << "$EA";
            break;
        case $LOGGED_UTILITY_STREAM:
            stream << "$LOGGED_UTILITY_STREAM";
            break;
        case $FIRST_USER_DEFINED_ATTRIBUTE:
            stream << "$FIRST_USER_DEFINED_ATTRIBUTE";
            break;
        default:
            stream << "Custom(" << dwAttrType << ")";
            break;
        }

        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::SIZE_EQ)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"Size=" << SizeEQ;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::SIZE_GT)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"Size>" << SizeG;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::SIZE_LT)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"Size<" << SizeL;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::SIZE_GE)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"Size>=" << SizeG;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::SIZE_LE)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"Size<=" << SizeL;
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::DATA_MD5)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"MD5=";
        for (DWORD i = 0; i < BYTES_IN_MD5_HASH; i++)
            stream << fmt::format(L"{:02X}", MD5[i]);
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::DATA_SHA1)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"SHA1=";

        for (DWORD i = 0; i < BYTES_IN_SHA1_HASH; i++)
            stream << fmt::format(L"{:02X}", SHA1[i]);
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::DATA_SHA256)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"SHA256=";

        for (DWORD i = 0; i < BYTES_IN_SHA256_HASH; i++)
            stream << fmt::format(L"{:02X}", SHA256[i]);
        bFirst = false;
    }

    if (Required & SearchTerm::Criteria::CONTAINS)
    {
        if (!bFirst)
            stream << L", ";

        if (bContainsIsHex)
        {
            stream << L"Contains=0x" << hex;

            for (DWORD i = 0; i < Contains.GetCount(); i++)
                stream << Contains.Get<BYTE>(i);
            stream << dec;
        }
        else
        {
            CBinaryBuffer toPrint;
            AnsiToWide(nullptr, (PCSTR)Contains.GetData(), (DWORD)Contains.GetCount(), toPrint);
            stream << L"Contains=";
            wstring buf;
            buf.assign((LPCWSTR)toPrint.GetData(), toPrint.GetCount() / sizeof(WCHAR));
            stream << buf;
        }
        bFirst = false;
    }

    if (Required & SearchTerm::Criteria::HEADER || Required & SearchTerm::Criteria::HEADER_HEX)
    {
        if (!bFirst)
            stream << L", ";
        stream << "Header=" << hex;

        for (DWORD i = 0; i < Header.GetCount(); i++)
            stream << Header.Get<BYTE>(i);
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::HEADER_REGEX)
    {
        if (!bFirst)
            stream << L", ";
        stream << L"Header matches regex " << strHeaderRegEx << L" (within first " << HeaderLen << L" bytes)";
        bFirst = false;
    }
    if (Required & SearchTerm::Criteria::YARA)
    {
        if (!bFirst)
            stream << L", ";
        if (!YaraRules.empty())
        {
            stream << L"Content matches yara rule(s) : " << YaraRulesSpec;
        }
        bFirst = false;
    }
    return std::move(stream.str());
}

std::pair<bool, std::wstring> Orc::FileFind::SearchTerm::IsValidTerm()
{
    using namespace std::string_literals;

    if (Required & Criteria::SIZE_EQ
        && (Required & Criteria::SIZE_LE || Required & Criteria::SIZE_LT || Required & Criteria::SIZE_GE
            || Required & Criteria::SIZE_GT))
        return { false, L"requirement size=<size> cannot be combined with any other size requirement"s };

    if (Required & Criteria::SIZE_GE && Required & Criteria::SIZE_GT)
        return { false, L"greater requirements cannot be combined"s };

    if (Required & Criteria::SIZE_LE && Required & Criteria::SIZE_LT)
        return { false, L"less requirements cannot be combined"s };

    return { true, L""s };
}

HRESULT FileFind::SearchTerm::AddTermToConfig(ConfigItem& item)
{
    ConfigItem& ntfs_find = item;
    ntfs_find.Type = ConfigItem::NODE;
    ntfs_find.Status = ConfigItem::PRESENT;

    if (Required & NAME_EXACT)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_NAME].strData = FileName;
        ntfs_find.SubItems[CONFIG_FILEFIND_NAME].Status = ConfigItem::PRESENT;
    }
    if (Required & NAME_MATCH)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_NAME_MATCH].strData = FileName;
        ntfs_find.SubItems[CONFIG_FILEFIND_NAME_MATCH].Status = ConfigItem::PRESENT;
    }
    if (Required & NAME_REGEX)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_NAME_REGEX].strData = FileName;
        ntfs_find.SubItems[CONFIG_FILEFIND_NAME_REGEX].Status = ConfigItem::PRESENT;
    }
    if (Required & PATH_EXACT)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_PATH].strData = Path;
        ntfs_find.SubItems[CONFIG_FILEFIND_PATH].Status = ConfigItem::PRESENT;
    }
    if (Required & PATH_MATCH)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_PATH_MATCH].strData = Path;
        ntfs_find.SubItems[CONFIG_FILEFIND_PATH_MATCH].Status = ConfigItem::PRESENT;
    }
    if (Required & PATH_REGEX)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_PATH_REGEX].strData = Path;
        ntfs_find.SubItems[CONFIG_FILEFIND_PATH_REGEX].Status = ConfigItem::PRESENT;
    }
    if (Required & EA_EXACT)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_EA].strData = EAName;
        ntfs_find.SubItems[CONFIG_FILEFIND_EA].Status = ConfigItem::PRESENT;
    }
    if (Required & EA_MATCH)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_EA_MATCH].strData = EAName;
        ntfs_find.SubItems[CONFIG_FILEFIND_EA_MATCH].Status = ConfigItem::PRESENT;
    }
    if (Required & EA_REGEX)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_EA_REGEX].strData = EAName;
        ntfs_find.SubItems[CONFIG_FILEFIND_EA_REGEX].Status = ConfigItem::PRESENT;
    }
    if (Required & ADS_EXACT)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_ADS].strData = ADSName;
        ntfs_find.SubItems[CONFIG_FILEFIND_ADS].Status = ConfigItem::PRESENT;
    }
    if (Required & ADS_MATCH)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_ADS_MATCH].strData = ADSName;
        ntfs_find.SubItems[CONFIG_FILEFIND_ADS_MATCH].Status = ConfigItem::PRESENT;
    }
    if (Required & ADS_REGEX)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_ADS_REGEX].strData = ADSName;
        ntfs_find.SubItems[CONFIG_FILEFIND_ADS_REGEX].Status = ConfigItem::PRESENT;
    }
    if (Required & SIZE_EQ)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_SIZE].strData = std::to_wstring(SizeEQ);
        ntfs_find.SubItems[CONFIG_FILEFIND_SIZE].Status = ConfigItem::PRESENT;
    }
    if (Required & SIZE_GT)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_SIZE_GT].strData = std::to_wstring(SizeG);
        ntfs_find.SubItems[CONFIG_FILEFIND_SIZE_GT].Status = ConfigItem::PRESENT;
    }
    if (Required & SIZE_GE)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_SIZE_GE].strData = std::to_wstring(SizeG);
        ntfs_find.SubItems[CONFIG_FILEFIND_SIZE_GE].Status = ConfigItem::PRESENT;
    }
    if (Required & SIZE_LT)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_SIZE_LT].strData = std::to_wstring(SizeL);
        ntfs_find.SubItems[CONFIG_FILEFIND_SIZE_LT].Status = ConfigItem::PRESENT;
    }
    if (Required & SIZE_LE)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_SIZE_LE].strData = std::to_wstring(SizeL);
        ntfs_find.SubItems[CONFIG_FILEFIND_SIZE_LE].Status = ConfigItem::PRESENT;
    }
    if (Required & DATA_MD5)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_MD5].strData = MD5.ToHex();
        ntfs_find.SubItems[CONFIG_FILEFIND_MD5].Status = ConfigItem::PRESENT;
    }
    if (Required & DATA_SHA1)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_SHA1].strData = SHA1.ToHex();
        ntfs_find.SubItems[CONFIG_FILEFIND_SHA1].Status = ConfigItem::PRESENT;
    }
    if (Required & DATA_SHA256)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_SHA256].strData = SHA256.ToHex();
        ntfs_find.SubItems[CONFIG_FILEFIND_SHA256].Status = ConfigItem::PRESENT;
    }
    if (Required & HEADER || Required & HEADER_HEX)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_HEADER_HEX].strData = Header.ToHex();
        ntfs_find.SubItems[CONFIG_FILEFIND_HEADER_HEX].Status = ConfigItem::PRESENT;
        if (HeaderLen > 0)
        {
            ntfs_find.SubItems[CONFIG_FILEFIND_HEADER_LENGTH].strData = std::to_wstring(HeaderLen);
            ntfs_find.SubItems[CONFIG_FILEFIND_HEADER_LENGTH].Status = ConfigItem::PRESENT;
        }
    }
    if (Required & HEADER_REGEX)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_HEADER_HEX].strData = strHeaderRegEx;
        ntfs_find.SubItems[CONFIG_FILEFIND_HEADER_HEX].Status = ConfigItem::PRESENT;
        if (HeaderLen > 0)
        {
            ntfs_find.SubItems[CONFIG_FILEFIND_HEADER_LENGTH].strData = std::to_wstring(HeaderLen);
            ntfs_find.SubItems[CONFIG_FILEFIND_HEADER_LENGTH].Status = ConfigItem::PRESENT;
        }
    }
    if (Required & ATTR_TYPE)
    {
        switch (dwAttrType)
        {
        case $STANDARD_INFORMATION:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$STANDARD_INFORMATION";
            break;
        case $ATTRIBUTE_LIST:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$ATTRIBUTE_LIST";
            break;
        case $FILE_NAME:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$FILE_NAME";
            break;
        case $OBJECT_ID:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$OBJECT_ID";
            break;
        case $SECURITY_DESCRIPTOR:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$SECURITY_DESCRIPTOR";
            break;
        case $VOLUME_NAME:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$VOLUME_NAME";
            break;
        case $VOLUME_INFORMATION:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$VOLUME_INFORMATION";
            break;
        case $DATA:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$DATA";
            break;
        case $INDEX_ROOT:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$INDEX_ROOT";
            break;
        case $INDEX_ALLOCATION:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$INDEX_ALLOCATION";
            break;
        case $BITMAP:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$BITMAP";
            break;
        case $REPARSE_POINT:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$REPARSE_POINT";
            break;
        case $EA_INFORMATION:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$EA_INFORMATION";
            break;
        case $EA:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$EA";
            break;
        case $LOGGED_UTILITY_STREAM:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$LOGGED_UTILITY_STREAM";
            break;
        case $FIRST_USER_DEFINED_ATTRIBUTE:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = L"$FIRST_USER_DEFINED_ATTRIBUTE";
            break;
        default:
            ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].strData = std::to_wstring(dwAttrType);
            break;
        }
        ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_TYPE].Status = ConfigItem::PRESENT;
    }
    if (Required & ATTR_NAME_EXACT)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_NAME].strData = AttrName;
        ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_NAME].Status = ConfigItem::PRESENT;
    }
    if (Required & ATTR_NAME_MATCH)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_MATCH].strData = AttrName;
        ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_MATCH].Status = ConfigItem::PRESENT;
    }
    if (Required & ATTR_NAME_REGEX)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_REGEX].strData = AttrName;
        ntfs_find.SubItems[CONFIG_FILEFIND_ATTR_REGEX].Status = ConfigItem::PRESENT;
    }
    if (Required & CONTAINS)
    {
        ntfs_find.SubItems[CONFIG_FILEFIND_CONTAINS_HEX].strData = Contains.ToHex();
        ntfs_find.SubItems[CONFIG_FILEFIND_CONTAINS_HEX].Status = ConfigItem::PRESENT;
    }
    if (Required & YARA)
    {
        if (!YaraRulesSpec.empty())
        {
            ntfs_find.SubItems[CONFIG_FILEFIND_YARA_RULE].strData = YaraRulesSpec;
            ntfs_find.SubItems[CONFIG_FILEFIND_YARA_RULE].Status = ConfigItem::PRESENT;
        }
    }
    ntfs_find.Status = ConfigItem::PRESENT;

    return S_OK;
}

HRESULT FileFind::InitializeYara(std::unique_ptr<YaraConfig>& config)
{
    if (m_YaraScan)
        return S_OK;

    std::vector<wstring> yara_content;
    std::vector<string> yara_rules;

    for (const auto& term : m_AllTerms)
    {
        if (term->Required & SearchTerm::Criteria::YARA)
        {
            yara_rules.insert(end(yara_rules), begin(term->YaraRules), end(term->YaraRules));
        }
    }

    if (config)
    {
        yara_content.insert(end(yara_content), begin(config->Sources()), end(config->Sources()));
    }

    {
        std::sort(begin(yara_content), end(yara_content));
        auto new_end = std::unique(begin(yara_content), end(yara_content));
        yara_content.erase(new_end, end(yara_content));
    }

    if (yara_content.empty() && yara_rules.empty())
        return S_OK;

    m_YaraScan = std::make_unique<YaraScanner>(_L_);
    if (!m_YaraScan)
        return E_OUTOFMEMORY;

    HRESULT hr = E_FAIL;
    if (FAILED(hr = m_YaraScan->Initialize()))
        return hr;

    if (FAILED(hr = m_YaraScan->Configure(config)))
    {
        log::Error(_L_, hr, L"Failed to configure yara scanner\r\n");
        return hr;
    }

    for (const auto& yara : yara_content)
    {
        if (FAILED(hr = m_YaraScan->AddRules(yara)))
        {
            log::Error(_L_, hr, L"Failed to load yara rules from source %s\r\n", yara.c_str());
        }
    }

    {
        std::sort(begin(yara_rules), end(yara_rules));
        auto new_end = std::unique(begin(yara_rules), end(yara_rules));
        yara_rules.erase(new_end, end(yara_rules));
    }
    if (!yara_rules.empty())
    {
        m_YaraScan->DisableRule("*");  // we first disable all rules

        for (const auto& rule : yara_rules)
        {
            m_YaraScan->EnableRule(rule.c_str());
        }
    }

    m_YaraScan->PrintConfiguration();

    return S_OK;
}

FileFind::SearchTerm::Criteria FileFind::DiscriminateName(const std::wstring& strName)
{
    if (strName.empty())
        return SearchTerm::Criteria::NONE;
    std::wsmatch m1;
    if (std::regex_search(strName, m1, RegexOnlyPattern()))
        return SearchTerm::Criteria::NAME_REGEX;
    std::wsmatch m2;
    if (std::regex_search(strName, m2, DOSPattern()))
        return SearchTerm::Criteria::NAME_MATCH;
    std::wsmatch m3;
    if (std::regex_search(strName, m3, RegexPattern()))
        return SearchTerm::Criteria::NAME_REGEX;
    return SearchTerm::Criteria::NAME_EXACT;
}

FileFind::SearchTerm::Criteria FileFind::DiscriminateADS(const std::wstring& strADS)
{
    if (strADS.empty())
        return SearchTerm::Criteria::NONE;
    std::wsmatch m1;
    if (std::regex_search(strADS, m1, RegexOnlyPattern()))
        return SearchTerm::Criteria::ADS_REGEX;
    std::wsmatch m2;
    if (std::regex_search(strADS, m2, DOSPattern()))
        return SearchTerm::Criteria::ADS_MATCH;
    std::wsmatch m3;
    if (std::regex_search(strADS, m3, RegexPattern()))
        return SearchTerm::Criteria::ADS_REGEX;
    return SearchTerm::Criteria::ADS_EXACT;
}

FileFind::SearchTerm::Criteria FileFind::DiscriminateEA(const std::wstring& strEA)
{
    if (strEA.empty())
        return SearchTerm::Criteria::NONE;
    std::wsmatch m1;
    if (std::regex_search(strEA, m1, RegexOnlyPattern()))
        return SearchTerm::Criteria::EA_REGEX;
    std::wsmatch m2;
    if (std::regex_search(strEA, m2, DOSPattern()))
        return SearchTerm::Criteria::EA_MATCH;
    std::wsmatch m3;
    if (std::regex_search(strEA, m3, RegexPattern()))
        return SearchTerm::Criteria::EA_REGEX;
    return SearchTerm::Criteria::EA_EXACT;
}

HRESULT FileFind::AddTerm(const shared_ptr<SearchTerm>& pMatch)
{
    if (pMatch->Required == SearchTerm::Criteria::NONE)
    {
        return E_INVALIDARG;
    }

    if ((pMatch->Required & SearchTerm::Criteria::NAME)
        && (pMatch->Required & SearchTerm::Criteria::NAME_EXACT || pMatch->Required & SearchTerm::Criteria::NAME_MATCH
            || pMatch->Required & SearchTerm::Criteria::NAME_REGEX
            || pMatch->Required & SearchTerm::Criteria::PATH_EXACT
            || pMatch->Required & SearchTerm::Criteria::PATH_MATCH
            || pMatch->Required & SearchTerm::Criteria::PATH_REGEX || pMatch->Required & SearchTerm::Criteria::EA_EXACT
            || pMatch->Required & SearchTerm::Criteria::EA_MATCH || pMatch->Required & SearchTerm::Criteria::EA_REGEX
            || pMatch->Required & SearchTerm::Criteria::ADS_EXACT || pMatch->Required & SearchTerm::Criteria::ADS_MATCH
            || pMatch->Required & SearchTerm::Criteria::ADS_REGEX))
    {
        log::Error(
            _L_,
            E_INVALIDARG,
            L"It is unsupported to have both a name and other attributes in file search criteria\r\n");
        return E_INVALIDARG;
    }

    if ((pMatch->Required & SearchTerm::Criteria::EA_EXACT || pMatch->Required & SearchTerm::Criteria::EA_MATCH
        || pMatch->Required & SearchTerm::Criteria::EA_REGEX)
        && (pMatch->Required & SearchTerm::Criteria::ADS_EXACT || pMatch->Required & SearchTerm::Criteria::ADS_MATCH
            || pMatch->Required & SearchTerm::Criteria::ADS_REGEX))
    {
        log::Error(
            _L_, E_INVALIDARG, L"It is unsupported to have both EA name and ADS name into a file search criteria\r\n");
        return E_INVALIDARG;
    }
    if ((pMatch->Required & SearchTerm::Criteria::ATTR_NAME_EXACT
        || pMatch->Required & SearchTerm::Criteria::ATTR_NAME_MATCH
        || pMatch->Required & SearchTerm::Criteria::ATTR_NAME_REGEX
        || pMatch->Required & SearchTerm::Criteria::ATTR_TYPE)
        && (pMatch->Required & SearchTerm::Criteria::ADS_EXACT || pMatch->Required & SearchTerm::Criteria::ADS_MATCH
            || pMatch->Required & SearchTerm::Criteria::ADS_REGEX || pMatch->Required & SearchTerm::Criteria::EA_EXACT
            || pMatch->Required & SearchTerm::Criteria::EA_MATCH || pMatch->Required & SearchTerm::Criteria::EA_REGEX))
    {
        log::Error(
            _L_,
            E_INVALIDARG,
            L"It is unsupported to have both EA name or ADS name combined with attr_* for attribute into a file search "
            L"criteria\r\n");
        return E_INVALIDARG;
    }

    m_AllTerms.push_back(pMatch);

    if (pMatch->Required & SearchTerm::Criteria::NAME)
    {
        // We received a "generic name", doing something more specific
        pMatch->Required = static_cast<SearchTerm::Criteria>(pMatch->Required & ~SearchTerm::Criteria::NAME);

        std::wsmatch m1;
        std::regex_match(pMatch->Name, m1, FileSpecPattern());

        if (m1[FILESPEC_FILENAME_INDEX].matched && !m1[FILESPEC_SUBNAME_INDEX].matched)
        {
            // only a file name was passed, no "sub" name
            std::wsmatch m2;
            std::wstring strFileName = m1[FILESPEC_FILENAME_INDEX].str();

            SearchTerm::Criteria namespec = DiscriminateName(strFileName);
            pMatch->Required = static_cast<SearchTerm::Criteria>(pMatch->Required | namespec);
            pMatch->FileName = strFileName;

            if (namespec & SearchTerm::Criteria::NAME_EXACT)
                m_ExactNameTerms.emplace(strFileName, pMatch);
            else if (pMatch->Required & SearchTerm::Criteria::SIZE_EQ)
            {
                m_SizeTerms.emplace(pMatch->SizeEQ, pMatch);
            }
            else
                m_Terms.push_back(pMatch);

            if (pMatch->DependsOnlyOnNameOrPath())
            {
                // add to the I30 search map
                if (namespec & SearchTerm::Criteria::NAME_EXACT)
                    m_I30ExactNameTerms.emplace(strFileName, pMatch);
                else
                    m_I30Terms.push_back(pMatch);
            }
        }
        else
        {
            // we have both a file name and a sub name
            if (m1[FILESPEC_FILENAME_INDEX].matched)
            {
                pMatch->FileName = m1[FILESPEC_FILENAME_INDEX].str();
                SearchTerm::Criteria namespec = DiscriminateName(pMatch->FileName);
                pMatch->Required |= namespec;

                if (namespec & SearchTerm::Criteria::NAME_EXACT)
                    // This is an exact name
                    m_ExactNameTerms.emplace(pMatch->FileName, pMatch);
                else if (pMatch->Required & SearchTerm::Criteria::SIZE_EQ)
                {
                    m_SizeTerms.emplace(pMatch->SizeEQ, pMatch);
                }
                else
                    m_Terms.push_back(pMatch);

                if (pMatch->DependsOnlyOnNameOrPath())
                {
                    // add to the I30 search map
                    if (namespec & SearchTerm::Criteria::NAME_EXACT)
                        m_I30ExactNameTerms.emplace(pMatch->FileName, pMatch);
                    else
                        m_I30Terms.push_back(pMatch);
                }
            }
            else if (pMatch->Required & SearchTerm::Criteria::SIZE_EQ)
            {
                m_SizeTerms.emplace(pMatch->SizeEQ, pMatch);
            }
            else
            {
                // No name specified
                m_Terms.push_back(pMatch);
            }

            if (m1[FILESPEC_SPEC_INDEX].matched && m1[FILESPEC_SUBNAME_INDEX].matched)
            {

                if (!m1[FILESPEC_SPEC_INDEX].compare(L":"))
                {
                    pMatch->ADSName = m1[FILESPEC_SUBNAME_INDEX].str();
                    pMatch->Required |= DiscriminateADS(pMatch->ADSName);
                }
                else if (!m1[FILESPEC_SPEC_INDEX].compare(L"#"))
                {
                    pMatch->EAName = m1[FILESPEC_SUBNAME_INDEX].str();
                    pMatch->Required |= DiscriminateEA(pMatch->EAName);
                }
                else
                    return E_INVALIDARG;
            }
        }
    }
    else
    {
        // specific name spec used, no need to split hair

        if (pMatch->Required & SearchTerm::Criteria::ADS)
        {
            pMatch->Required = static_cast<SearchTerm::Criteria>(pMatch->Required & ~SearchTerm::Criteria::ADS);
            pMatch->Required |= DiscriminateADS(pMatch->ADSName);
        }
        if (pMatch->Required & SearchTerm::Criteria::EA)
        {
            pMatch->Required = static_cast<SearchTerm::Criteria>(pMatch->Required & ~SearchTerm::Criteria::EA);
            pMatch->Required |= DiscriminateEA(pMatch->EAName);
        }

        if (pMatch->Required & SearchTerm::Criteria::NAME_EXACT)
        {
            m_ExactNameTerms.emplace(pMatch->FileName, pMatch);
            if (pMatch->DependsOnlyOnNameOrPath())
                m_I30ExactNameTerms.insert(pair<wstring, shared_ptr<SearchTerm>>(pMatch->FileName, pMatch));
        }
        else if (pMatch->Required & SearchTerm::Criteria::PATH_EXACT)
        {
            m_ExactPathTerms.emplace(pMatch->Path, pMatch);
            if (pMatch->DependsOnlyOnNameOrPath())
                m_I30ExactPathTerms.insert(pair<wstring, shared_ptr<SearchTerm>>(pMatch->Path, pMatch));
        }
        else if (pMatch->Required & SearchTerm::Criteria::SIZE_EQ)
        {
            m_SizeTerms.emplace(pMatch->SizeEQ, pMatch);
        }
        else
        {
            m_Terms.push_back(pMatch);
            if (pMatch->DependsOnlyOnNameOrPath())
                m_I30Terms.push_back(pMatch);
        }
    }

    return S_OK;
}

HRESULT FileFind::AddExcludeTerm(const shared_ptr<SearchTerm>& pMatch)
{
    if (pMatch->Required == SearchTerm::Criteria::NONE)
    {
        return E_INVALIDARG;
    }

    if ((pMatch->Required & SearchTerm::Criteria::NAME)
        && (pMatch->Required & SearchTerm::Criteria::NAME_EXACT || pMatch->Required & SearchTerm::Criteria::NAME_MATCH
            || pMatch->Required & SearchTerm::Criteria::NAME_REGEX
            || pMatch->Required & SearchTerm::Criteria::PATH_EXACT
            || pMatch->Required & SearchTerm::Criteria::PATH_MATCH
            || pMatch->Required & SearchTerm::Criteria::PATH_REGEX || pMatch->Required & SearchTerm::Criteria::EA_EXACT
            || pMatch->Required & SearchTerm::Criteria::EA_MATCH || pMatch->Required & SearchTerm::Criteria::EA_REGEX
            || pMatch->Required & SearchTerm::Criteria::ADS_EXACT || pMatch->Required & SearchTerm::Criteria::ADS_MATCH
            || pMatch->Required & SearchTerm::Criteria::ADS_REGEX))
    {
        log::Error(
            _L_,
            E_INVALIDARG,
            L"It is unsupported to have both a name and other attributes in file search criteria\r\n");
        return E_INVALIDARG;
    }

    if ((pMatch->Required & SearchTerm::Criteria::EA_EXACT || pMatch->Required & SearchTerm::Criteria::EA_MATCH
        || pMatch->Required & SearchTerm::Criteria::EA_REGEX)
        && (pMatch->Required & SearchTerm::Criteria::ADS_EXACT || pMatch->Required & SearchTerm::Criteria::ADS_MATCH
            || pMatch->Required & SearchTerm::Criteria::ADS_REGEX))
    {
        log::Error(
            _L_, E_INVALIDARG, L"It is unsupported to have both EA name and ADS name into a file search criteria\r\n");
        return E_INVALIDARG;
    }
    if ((pMatch->Required & SearchTerm::Criteria::ATTR_NAME_EXACT
        || pMatch->Required & SearchTerm::Criteria::ATTR_NAME_MATCH
        || pMatch->Required & SearchTerm::Criteria::ATTR_NAME_REGEX
        || pMatch->Required & SearchTerm::Criteria::ATTR_TYPE)
        && (pMatch->Required & SearchTerm::Criteria::ADS_EXACT || pMatch->Required & SearchTerm::Criteria::ADS_MATCH
            || pMatch->Required & SearchTerm::Criteria::ADS_REGEX || pMatch->Required & SearchTerm::Criteria::EA_EXACT
            || pMatch->Required & SearchTerm::Criteria::EA_MATCH || pMatch->Required & SearchTerm::Criteria::EA_REGEX))
    {
        log::Error(
            _L_,
            E_INVALIDARG,
            L"It is unsupported to have both EA name or ADS name combined with attr_* for attribute into a file search "
            L"criteria\r\n");
        return E_INVALIDARG;
    }

    m_AllTerms.push_back(pMatch);

    if (pMatch->Required & SearchTerm::Criteria::NAME)
    {
        // We received a "generic name", doing something more specific
        pMatch->Required = static_cast<SearchTerm::Criteria>(pMatch->Required & ~SearchTerm::Criteria::NAME);

        std::wsmatch m1;
        std::regex_match(pMatch->Name, m1, FileSpecPattern());

        if (m1[FILESPEC_FILENAME_INDEX].matched && !m1[FILESPEC_SUBNAME_INDEX].matched)
        {
            // only a file name was passed, no "sub" name
            std::wsmatch m2;
            std::wstring strFileName = m1[FILESPEC_FILENAME_INDEX].str();

            SearchTerm::Criteria namespec = DiscriminateName(strFileName);
            pMatch->Required = static_cast<SearchTerm::Criteria>(pMatch->Required | namespec);
            pMatch->FileName = strFileName;

            if (namespec & SearchTerm::Criteria::NAME_EXACT)
                m_ExcludeNameTerms.emplace(strFileName, pMatch);
            else if (pMatch->Required & SearchTerm::Criteria::SIZE_EQ)
                m_ExcludeSizeTerms.emplace(pMatch->SizeEQ, pMatch);
            else
                m_ExcludeTerms.push_back(pMatch);
        }
        else
        {
            // we have both a file name and a sub name
            if (m1[FILESPEC_FILENAME_INDEX].matched)
            {
                pMatch->FileName = m1[FILESPEC_FILENAME_INDEX].str();
                SearchTerm::Criteria namespec = DiscriminateName(pMatch->FileName);
                pMatch->Required |= namespec;

                if (namespec & SearchTerm::Criteria::NAME_EXACT)
                    // This is an exact name
                    m_ExcludeNameTerms.emplace(pMatch->FileName, pMatch);
                else if (pMatch->Required & SearchTerm::Criteria::SIZE_EQ)
                    m_ExcludeSizeTerms.emplace(pMatch->SizeEQ, pMatch);
                else
                    m_ExcludeTerms.push_back(pMatch);
            }
            else if (pMatch->Required & SearchTerm::Criteria::SIZE_EQ)
                m_ExcludeSizeTerms.emplace(pMatch->SizeEQ, pMatch);
            else
                m_ExcludeTerms.push_back(pMatch);

            if (m1[FILESPEC_SPEC_INDEX].matched && m1[FILESPEC_SUBNAME_INDEX].matched)
            {

                if (!m1[FILESPEC_SPEC_INDEX].compare(L":"))
                {
                    pMatch->ADSName = m1[FILESPEC_SUBNAME_INDEX].str();
                    pMatch->Required |= DiscriminateADS(pMatch->ADSName);
                }
                else if (!m1[FILESPEC_SPEC_INDEX].compare(L"#"))
                {
                    pMatch->EAName = m1[FILESPEC_SUBNAME_INDEX].str();
                    pMatch->Required |= DiscriminateEA(pMatch->EAName);
                }
                else
                    return E_INVALIDARG;
            }
        }
    }
    else
    {
        // specific name spec used, no need to split hair

        if (pMatch->Required & SearchTerm::Criteria::ADS)
        {
            pMatch->Required = static_cast<SearchTerm::Criteria>(pMatch->Required & ~SearchTerm::Criteria::ADS);
            pMatch->Required |= DiscriminateADS(pMatch->ADSName);
        }
        if (pMatch->Required & SearchTerm::Criteria::EA)
        {
            pMatch->Required = static_cast<SearchTerm::Criteria>(pMatch->Required & ~SearchTerm::Criteria::EA);
            pMatch->Required |= DiscriminateEA(pMatch->EAName);
        }

        if (pMatch->Required & SearchTerm::Criteria::NAME_EXACT)
            m_ExcludeNameTerms.emplace(pMatch->FileName, pMatch);
        else if (pMatch->Required & SearchTerm::Criteria::PATH_EXACT)
            m_ExcludePathTerms.emplace(pMatch->Path, pMatch);
        else if (pMatch->Required & SearchTerm::Criteria::SIZE_EQ)
            m_ExcludeSizeTerms.emplace(pMatch->SizeEQ, pMatch);
        else
            m_ExcludeTerms.push_back(pMatch);
    }

    return S_OK;
}
FileFind::SearchTerm::Criteria
FileFind::ExactName(const std::shared_ptr<FileFind::SearchTerm>& aTerm, const PFILE_NAME pFileName) const
{
    if (pFileName == nullptr)
        return SearchTerm::Criteria::NONE;
    if (aTerm->Required & SearchTerm::Criteria::NAME_EXACT)
    {
        if (aTerm->FileName.size() != pFileName->FileNameLength)
            return SearchTerm::Criteria::NONE;
        if (!_wcsnicmp(aTerm->FileName.c_str(), pFileName->FileName, aTerm->FileName.size()))
            return SearchTerm::Criteria::NAME_EXACT;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::MatchName(const std::shared_ptr<FileFind::SearchTerm>& aTerm, const PFILE_NAME pFileName) const
{
    if (aTerm->Required & SearchTerm::Criteria::NAME_MATCH)
    {
        if (pFileName == nullptr)
            return SearchTerm::Criteria::NONE;
        WCHAR szName[MAX_PATH];
        szName[0] = L'\0';  // avoid false positive warning C6054
        wcsncpy_s(szName, pFileName->FileName, pFileName->FileNameLength);
        if (aTerm->FileName.empty())
            return SearchTerm::Criteria::NONE;
        if (PathMatchSpec(szName, aTerm->FileName.c_str()))
            return SearchTerm::Criteria::NAME_MATCH;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::RegexName(const std::shared_ptr<FileFind::SearchTerm>& aTerm, const PFILE_NAME pFileName) const
{
    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;
    if (aTerm->Required & SearchTerm::Criteria::NAME_REGEX)
    {
        if (regex_match(pFileName->FileName, pFileName->FileName + pFileName->FileNameLength, aTerm->FileNameRegEx))
            matchedSpec |= SearchTerm::Criteria::NAME_REGEX;

        return matchedSpec;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria FileFind::MatchName(
    const std::shared_ptr<SearchTerm>& aTerm,
    SearchTerm::Criteria requiredSpec,
    std::shared_ptr<Match>& aFileMatch,
    const PFILE_NAME pFileName) const
{
    DBG_UNREFERENCED_PARAMETER(aFileMatch);
    DBG_UNREFERENCED_PARAMETER(requiredSpec);

    SearchTerm::Criteria retval = SearchTerm::Criteria::NONE;

    if (!m_InLocationBuilder(pFileName))
        return retval;

    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;
    if (aTerm->Required & SearchTerm::Criteria::NAME_EXACT)
    {
        SearchTerm::Criteria aSpec = ExactName(aTerm, pFileName);
        if (aSpec == SearchTerm::Criteria::NONE)
            return retval;
        matchedSpec |= aSpec;
    }
    if (aTerm->Required & SearchTerm::Criteria::NAME_MATCH)
    {
        SearchTerm::Criteria aSpec = MatchName(aTerm, pFileName);
        if (aSpec == SearchTerm::Criteria::NONE)
            return retval;
        matchedSpec |= aSpec;
    }
    if (aTerm->Required & SearchTerm::Criteria::NAME_REGEX)
    {
        SearchTerm::Criteria aSpec = RegexName(aTerm, pFileName);
        if (aSpec == SearchTerm::Criteria::NONE)
            return retval;
        matchedSpec |= aSpec;
    }

    return matchedSpec;
}

FileFind::SearchTerm::Criteria FileFind::MatchName(
    const std::shared_ptr<SearchTerm>& aTerm,
    SearchTerm::Criteria required,
    const Match::NameMatch& pNameMatch) const
{
    DBG_UNREFERENCED_PARAMETER(required);
    SearchTerm::Criteria retval = SearchTerm::Criteria::NONE;

    PFILE_NAME pFileName = pNameMatch.FILENAME();

    if (!m_InLocationBuilder(pFileName))
        return retval;

    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;
    if (aTerm->Required & SearchTerm::Criteria::NAME_EXACT)
    {
        SearchTerm::Criteria aSpec = ExactName(aTerm, pFileName);
        if (aSpec == SearchTerm::Criteria::NONE)
            return retval;
        matchedSpec |= aSpec;
    }
    if (aTerm->Required & SearchTerm::Criteria::NAME_MATCH)
    {
        SearchTerm::Criteria aSpec = MatchName(aTerm, pFileName);
        if (aSpec == SearchTerm::Criteria::NONE)
            return retval;
        matchedSpec |= aSpec;
    }
    if (aTerm->Required & SearchTerm::Criteria::NAME_REGEX)
    {
        SearchTerm::Criteria aSpec = RegexName(aTerm, pFileName);
        if (aSpec == SearchTerm::Criteria::NONE)
            return retval;
        matchedSpec |= aSpec;
    }

    return matchedSpec;
}

FileFind::SearchTerm::Criteria FileFind::AddMatchingName(
    const std::shared_ptr<SearchTerm>& aTerm,
    SearchTerm::Criteria requiredSpec,
    std::shared_ptr<Match>& aFileMatch,
    MFTRecord* pElt) const
{
    SearchTerm::Criteria retval = SearchTerm::Criteria::NONE;

    for (auto name_iter = begin(pElt->GetFileNames()); name_iter != end(pElt->GetFileNames()); ++name_iter)
    {
        SearchTerm::Criteria matchedSpec = MatchName(aTerm, requiredSpec, aFileMatch, *name_iter);

        if (requiredSpec == matchedSpec)
        {
            if (aFileMatch == nullptr)
                aFileMatch = std::make_shared<Match>(
                    m_pVolReader, aTerm, pElt->GetFileReferenceNumber(), !pElt->IsRecordInUse());

            aFileMatch->AddFileNameMatch(m_FullNameBuilder, *name_iter);

            retval = matchedSpec;
        }
    }
    return retval;
}

FileFind::SearchTerm::Criteria FileFind::ExcludeMatchingName(
    const std::shared_ptr<SearchTerm>& aTerm,
    SearchTerm::Criteria requiredSpec,
    const std::shared_ptr<Match>& aMatch) const
{
    SearchTerm::Criteria retval = SearchTerm::Criteria::NONE;

    if (aMatch->MatchingNames.empty())
        return SearchTerm::Criteria::NONE;

    auto found = std::find_if(
        begin(aMatch->MatchingNames),
        end(aMatch->MatchingNames),
        [this, aTerm, requiredSpec, &retval](const Match::NameMatch& aNameMatch) -> bool {
        SearchTerm::Criteria matchedSpec = MatchName(aTerm, requiredSpec, aNameMatch);

        if (requiredSpec == matchedSpec)
            return true;
        return false;
    });

    if (found != end(aMatch->MatchingNames))
    {
        return requiredSpec;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::ExactPath(const std::shared_ptr<FileFind::SearchTerm>& aTerm, const WCHAR* szFullName) const
{
    if (szFullName == nullptr)
        return SearchTerm::Criteria::NONE;

    if (aTerm->Required & SearchTerm::Criteria::PATH_EXACT)
    {
        if (wcslen(szFullName) < sizeof("x:\\"))
            return SearchTerm::Criteria::NONE;

        if (szFullName[1] == L':'
            && ((szFullName[0] > L'A' && szFullName[0] < L'Z') || (szFullName[0] > L'a' && szFullName[0] < L'z')))
            szFullName += 2;
        if (szFullName[0] != L'\\')
            return SearchTerm::Criteria::NONE;
        if (!_wcsicmp(aTerm->Path.c_str(), szFullName))
            return SearchTerm::Criteria::PATH_EXACT;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::MatchPath(const std::shared_ptr<FileFind::SearchTerm>& aTerm, const WCHAR* szFullName) const
{
    if (szFullName == nullptr)
        return SearchTerm::Criteria::NONE;

    if (aTerm->Required & SearchTerm::Criteria::PATH_MATCH)
    {
        if (wcslen(szFullName) < sizeof("x:\\"))
            return SearchTerm::Criteria::NONE;

        if (szFullName[1] == L':'
            && ((szFullName[0] > L'A' && szFullName[0] < L'Z') || (szFullName[0] > L'a' && szFullName[0] < L'z')))
            szFullName += 2;
        if (szFullName[0] != L'\\')
            return SearchTerm::Criteria::NONE;

        if (aTerm->Path.empty())
            return SearchTerm::Criteria::NONE;
        if (PathMatchSpec(szFullName, aTerm->Path.c_str()))
            return SearchTerm::Criteria::PATH_MATCH;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::RegexPath(const std::shared_ptr<FileFind::SearchTerm>& aTerm, const WCHAR* szFullName) const
{
    if (szFullName == nullptr)
        return SearchTerm::Criteria::NONE;

    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;
    if (aTerm->Required & SearchTerm::Criteria::PATH_REGEX)
    {
        if (wcslen(szFullName) < sizeof("x:\\"))
            return SearchTerm::Criteria::NONE;

        if (szFullName[1] == L':'
            && ((szFullName[0] > L'A' && szFullName[0] < L'Z') || (szFullName[0] > L'a' && szFullName[0] < L'z')))
            szFullName += 2;
        if (szFullName[0] != L'\\')
            return SearchTerm::Criteria::NONE;

        if (regex_match(szFullName, szFullName + wcslen(szFullName), aTerm->PathRegEx))
            matchedSpec |= SearchTerm::Criteria::PATH_REGEX;

        return matchedSpec;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria FileFind::MatchPath(
    const std::shared_ptr<SearchTerm>& aTerm,
    SearchTerm::Criteria requiredSpec,
    std::shared_ptr<Match>& aFileMatch,
    const PFILE_NAME pFileName) const
{
    DBG_UNREFERENCED_PARAMETER(aFileMatch);
    DBG_UNREFERENCED_PARAMETER(requiredSpec);

    if (!m_InLocationBuilder(pFileName))
        return SearchTerm::Criteria::NONE;

    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;

    LPCWSTR szFullName = nullptr;
    if (m_FullNameBuilder)
        szFullName = m_FullNameBuilder(pFileName, nullptr);
    if (szFullName)
    {
        if (aTerm->Required & SearchTerm::Criteria::PATH_EXACT)
        {
            SearchTerm::Criteria aSpec = ExactPath(aTerm, szFullName);
            if (aSpec == SearchTerm::Criteria::NONE)
                return SearchTerm::Criteria::NONE;
            matchedSpec |= aSpec;
        }
        if (aTerm->Required & SearchTerm::Criteria::PATH_MATCH)
        {
            SearchTerm::Criteria aSpec = MatchPath(aTerm, szFullName);
            if (aSpec == SearchTerm::Criteria::NONE)
                return SearchTerm::Criteria::NONE;
            matchedSpec |= aSpec;
        }
        if (aTerm->Required & SearchTerm::Criteria::PATH_REGEX)
        {
            SearchTerm::Criteria aSpec = RegexPath(aTerm, szFullName);
            if (aSpec == SearchTerm::Criteria::NONE)
                return SearchTerm::Criteria::NONE;
            matchedSpec |= aSpec;
        }
    }
    return matchedSpec;
}

FileFind::SearchTerm::Criteria FileFind::MatchPath(
    const std::shared_ptr<SearchTerm>& aTerm,
    SearchTerm::Criteria requiredSpec,
    const Match::NameMatch& aFileMatch) const
{
    DBG_UNREFERENCED_PARAMETER(requiredSpec);
    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;

    if (!aFileMatch.FullPathName.empty())
    {
        if (aTerm->Required & SearchTerm::Criteria::PATH_EXACT)
        {
            SearchTerm::Criteria aSpec = ExactPath(aTerm, aFileMatch.FullPathName.c_str());
            if (aSpec == SearchTerm::Criteria::NONE)
                return SearchTerm::Criteria::NONE;
            matchedSpec |= aSpec;
        }
        if (aTerm->Required & SearchTerm::Criteria::PATH_MATCH)
        {
            SearchTerm::Criteria aSpec = MatchPath(aTerm, aFileMatch.FullPathName.c_str());
            if (aSpec == SearchTerm::Criteria::NONE)
                return SearchTerm::Criteria::NONE;
            matchedSpec |= aSpec;
        }
        if (aTerm->Required & SearchTerm::Criteria::PATH_REGEX)
        {
            SearchTerm::Criteria aSpec = RegexPath(aTerm, aFileMatch.FullPathName.c_str());
            if (aSpec == SearchTerm::Criteria::NONE)
                return SearchTerm::Criteria::NONE;
            matchedSpec |= aSpec;
        }
    }
    return matchedSpec;
}

FileFind::SearchTerm::Criteria FileFind::AddMatchingPath(
    const std::shared_ptr<SearchTerm>& aTerm,
    SearchTerm::Criteria requiredSpec,
    std::shared_ptr<Match>& aFileMatch,
    MFTRecord* pElt) const
{
    SearchTerm::Criteria retval = SearchTerm::Criteria::NONE;

    for (auto name_iter = begin(pElt->GetFileNames()); name_iter != end(pElt->GetFileNames()); ++name_iter)
    {
        SearchTerm::Criteria matchedSpec = MatchPath(aTerm, requiredSpec, aFileMatch, *name_iter);

        if (matchedSpec == requiredSpec)
        {
            if (aFileMatch == nullptr)
                aFileMatch = std::make_shared<Match>(
                    m_pVolReader, aTerm, pElt->GetFileReferenceNumber(), !pElt->IsRecordInUse());

            if (aFileMatch == nullptr)
                return SearchTerm::Criteria::NONE;

            LPCWSTR szFullName = L"";
            if (m_FullNameBuilder)
                szFullName = m_FullNameBuilder(*name_iter, nullptr);

            aFileMatch->AddFileNameMatch(*name_iter, szFullName);

            retval = requiredSpec;
        }
    }
    return retval;
}

FileFind::SearchTerm::Criteria FileFind::ExcludeMatchingPath(
    const std::shared_ptr<SearchTerm>& aTerm,
    SearchTerm::Criteria requiredSpec,
    const std::shared_ptr<Match>& aMatch) const
{
    SearchTerm::Criteria retval = SearchTerm::Criteria::NONE;

    auto found = std::find_if(
        begin(aMatch->MatchingNames),
        end(aMatch->MatchingNames),
        [this, aTerm, requiredSpec, &retval](const Match::NameMatch& aNameMatch) -> bool {
        SearchTerm::Criteria matchedSpec = MatchPath(aTerm, requiredSpec, aNameMatch);

        if (requiredSpec == matchedSpec)
            return true;
        return false;
    });
    if (found != end(aMatch->MatchingNames))
    {
        return requiredSpec;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria FileFind::ExactEA(
    const std::shared_ptr<FileFind::SearchTerm>& aTerm,
    MFTRecord* pElt,
    const std::shared_ptr<MftRecordAttribute>& pAttr) const
{
    if (aTerm->Required & SearchTerm::Criteria::EA_EXACT)
    {
        if (!pElt->HasExtendedAttr())
            return SearchTerm::Criteria::NONE;

        if (pAttr->TypeCode() == $EA)
        {
            auto ea_attr = std::dynamic_pointer_cast<ExtendedAttribute, MftRecordAttribute>(pAttr);

            if (FAILED(ea_attr->Parse(m_pVolReader)))
                return SearchTerm::Criteria::NONE;

            auto found = std::find_if(
                begin(ea_attr->Items()), end(ea_attr->Items()), [aTerm](const ExtendedAttribute::Item& item) {
                return !_wcsicmp(item.first.c_str(), aTerm->EAName.c_str());
            });
            if (found != end(ea_attr->Items()))
                return SearchTerm::Criteria::EA_EXACT;
        }
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria FileFind::MatchEA(
    const std::shared_ptr<FileFind::SearchTerm>& aTerm,
    MFTRecord* pElt,
    const std::shared_ptr<MftRecordAttribute>& pAttr) const
{
    if (aTerm->Required & SearchTerm::Criteria::EA_MATCH)
    {
        if (!pElt->HasExtendedAttr())
            return SearchTerm::Criteria::NONE;

        if (pAttr->TypeCode() == $EA)
        {
            auto ea_attr = std::dynamic_pointer_cast<ExtendedAttribute, MftRecordAttribute>(pAttr);
            if (!ea_attr)
            {
                return SearchTerm::Criteria::NONE;
            }

            if (FAILED(ea_attr->Parse(m_pVolReader)))
                return SearchTerm::Criteria::NONE;

            auto found = std::find_if(
                begin(ea_attr->Items()), end(ea_attr->Items()), [aTerm](const ExtendedAttribute::Item& item) {
                return PathMatchSpec(item.first.c_str(), aTerm->EAName.c_str());
            });
            if (found != end(ea_attr->Items()))
                return SearchTerm::Criteria::EA_MATCH;
        }
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria FileFind::RegexEA(
    const std::shared_ptr<FileFind::SearchTerm>& aTerm,
    MFTRecord* pElt,
    const std::shared_ptr<MftRecordAttribute>& pAttr) const
{
    if (aTerm->Required & SearchTerm::Criteria::EA_REGEX)
    {
        if (!pElt->HasExtendedAttr())
            return SearchTerm::Criteria::NONE;

        if (pAttr->TypeCode() == $EA)
        {
            auto ea_attr = std::dynamic_pointer_cast<ExtendedAttribute, MftRecordAttribute>(pAttr);
            if (!ea_attr)
                return SearchTerm::Criteria::NONE;

            if (FAILED(ea_attr->Parse(m_pVolReader)))
                return SearchTerm::Criteria::NONE;

            auto found = std::find_if(
                begin(ea_attr->Items()), end(ea_attr->Items()), [aTerm](const ExtendedAttribute::Item& item) {
                return regex_match(item.first, aTerm->EANameRegEx);
            });
            if (found != end(ea_attr->Items()))
                return SearchTerm::Criteria::EA_REGEX;
        }
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::ExactAttr(const std::shared_ptr<SearchTerm>& aTerm, LPCWSTR szAttrName, size_t AttrNameLen) const
{
    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;

    if(Orc::equalCaseInsensitive(aTerm->AttrName, std::wstring_view(szAttrName, AttrNameLen)))
    {
        return matchedSpec | SearchTerm::Criteria::ATTR_NAME_EXACT;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::MatchAttr(const std::shared_ptr<SearchTerm>& aTerm, LPCWSTR szAttrName, size_t AttrNameLen) const
{
    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;

    WCHAR szLocalAttrName[MAX_PATH];
    szLocalAttrName[0] = L'\0';  // avoid false positive warning C6054
    wcsncpy_s(szLocalAttrName, szAttrName, AttrNameLen);

    if (PathMatchSpec(szLocalAttrName, aTerm->AttrName.c_str()))
        return matchedSpec | SearchTerm::Criteria::ATTR_NAME_MATCH;
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::RegExAttr(const std::shared_ptr<SearchTerm>& aTerm, LPCWSTR szAttrName, size_t AttrNameLen) const
{
    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;

    WCHAR szLocalAttrName[MAX_PATH];
    szLocalAttrName[0] = L'\0';  // avoid false positive warning C6054
    wcsncpy_s(szLocalAttrName, szAttrName, AttrNameLen);

    if (regex_match(szLocalAttrName, aTerm->AttrNameRegEx))
        return matchedSpec | SearchTerm::Criteria::ATTR_NAME_REGEX;
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::AttrType(const std::shared_ptr<SearchTerm>& aTerm, const ATTRIBUTE_TYPE_CODE attrType) const
{
    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;
    if (attrType == aTerm->dwAttrType)
    {
        return matchedSpec | SearchTerm::Criteria::ATTR_TYPE;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria FileFind::MatchAttributes(
    const std::shared_ptr<SearchTerm>& aTerm,
    SearchTerm::Criteria requiredSpec,
    std::shared_ptr<Match>& aFileMatch,
    MFTRecord* pElt) const
{
    SearchTerm::Criteria retval = SearchTerm::Criteria::NONE;

    for (auto attr_iter = begin(pElt->GetAttributeList()); attr_iter != end(pElt->GetAttributeList()); ++attr_iter)
    {
        SearchTerm::Criteria matchedAttributesSpecs = SearchTerm::Criteria::NONE;
        const auto& pAttr = attr_iter->Attribute();

        if (pAttr == nullptr)
            continue;

        if (requiredSpec & SearchTerm::Criteria::ATTR_TYPE)
        {
            if (pAttr->Header() != nullptr)
            {
                SearchTerm::Criteria aSpec = AttrType(aTerm, pAttr->Header()->TypeCode);
                if (aSpec == SearchTerm::Criteria::NONE)
                {
                    continue;
                }
                matchedAttributesSpecs = requiredSpec | aSpec;
            }
            else
                continue;
        }
        if (requiredSpec & SearchTerm::Criteria::EA_EXACT)
        {
            SearchTerm::Criteria aSpec = ExactEA(aTerm, pElt, pAttr);
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                continue;
            }
            matchedAttributesSpecs = requiredSpec | aSpec;
        }
        if (requiredSpec & SearchTerm::Criteria::EA_MATCH)
        {
            SearchTerm::Criteria aSpec = MatchEA(aTerm, pElt, pAttr);
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                continue;
            }
            matchedAttributesSpecs = requiredSpec | aSpec;
        }
        if (requiredSpec & SearchTerm::Criteria::EA_REGEX)
        {
            SearchTerm::Criteria aSpec = RegexEA(aTerm, pElt, pAttr);
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                continue;
            }
            matchedAttributesSpecs = requiredSpec | aSpec;
        }
        if (requiredSpec & SearchTerm::Criteria::ATTR_NAME_EXACT)
        {
            SearchTerm::Criteria aSpec = ExactAttr(aTerm, pAttr->NamePtr(), pAttr->NameLength());
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                continue;
            }
            matchedAttributesSpecs = requiredSpec | aSpec;
        }
        if (requiredSpec & SearchTerm::Criteria::ATTR_NAME_MATCH)
        {
            SearchTerm::Criteria aSpec = MatchAttr(aTerm, pAttr->NamePtr(), pAttr->NameLength());
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                continue;
            }
            matchedAttributesSpecs = requiredSpec | aSpec;
        }
        if (requiredSpec & SearchTerm::Criteria::ATTR_NAME_REGEX)
        {
            SearchTerm::Criteria aSpec = RegExAttr(aTerm, pAttr->NamePtr(), pAttr->NameLength());
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                continue;
            }
            matchedAttributesSpecs = requiredSpec | aSpec;
        }
        if (matchedAttributesSpecs == requiredSpec)
        {
            if (aFileMatch == nullptr)
                aFileMatch = std::make_shared<Match>(
                    m_pVolReader, aTerm, pElt->GetFileReferenceNumber(), !pElt->IsRecordInUse());

            if (m_bProvideStream)
                aFileMatch->AddAttributeMatch(_L_, m_pVolReader, pAttr);
            else
                aFileMatch->AddAttributeMatch(pAttr);

            retval = requiredSpec;
        }
    }
    return retval;
}

FileFind::SearchTerm::Criteria FileFind::ExcludeMatchingAttributes(
    const std::shared_ptr<SearchTerm>& aTerm,
    SearchTerm::Criteria requiredSpec,
    const std::shared_ptr<Match>& aFileMatch) const
{
    auto found = std::find_if(
        begin(aFileMatch->MatchingAttributes),
        end(aFileMatch->MatchingAttributes),
        [this, aTerm, requiredSpec](const Match::AttributeMatch& attrMatch) -> bool {
        SearchTerm::Criteria matchedAttributesSpecs = SearchTerm::Criteria::NONE;

        if (requiredSpec & SearchTerm::Criteria::ATTR_TYPE)
        {
            SearchTerm::Criteria aSpec = AttrType(aTerm, attrMatch.Type);
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                return false;
            }
            matchedAttributesSpecs = requiredSpec | aSpec;
        }
        //
        /// EA content exclusion is not supported!
        //
        if (requiredSpec & SearchTerm::Criteria::ATTR_NAME_EXACT)
        {
            SearchTerm::Criteria aSpec = ExactAttr(aTerm, attrMatch.AttrName.c_str(), attrMatch.AttrName.size());
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                return false;
            }
            matchedAttributesSpecs = requiredSpec | aSpec;
        }
        if (requiredSpec & SearchTerm::Criteria::ATTR_NAME_MATCH)
        {
            SearchTerm::Criteria aSpec = MatchAttr(aTerm, attrMatch.AttrName.c_str(), attrMatch.AttrName.size());
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                return false;
            }
            matchedAttributesSpecs = requiredSpec | aSpec;
        }
        if (requiredSpec & SearchTerm::Criteria::ATTR_NAME_REGEX)
        {
            SearchTerm::Criteria aSpec = RegExAttr(aTerm, attrMatch.AttrName.c_str(), attrMatch.AttrName.size());
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                return false;
            }
            matchedAttributesSpecs = requiredSpec | aSpec;
        }

        if (matchedAttributesSpecs == requiredSpec)
            return true;
        return false;
    });
    if (found != end(aFileMatch->MatchingAttributes))
        return requiredSpec;
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::ExactADS(const std::shared_ptr<FileFind::SearchTerm>& aTerm, LPCWSTR szAttrName, size_t AttrNameLen) const
{
    if (aTerm->Required & SearchTerm::Criteria::ADS_EXACT)
    {
        if (szAttrName == nullptr)
            return SearchTerm::Criteria::NONE;
        if (Orc::equalCaseInsensitive(aTerm->ADSName, std::wstring_view(szAttrName, AttrNameLen)))
            return SearchTerm::Criteria::ADS_EXACT;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::MatchADS(const std::shared_ptr<FileFind::SearchTerm>& aTerm, LPCWSTR szAttrName, size_t AttrNameLen) const
{
    if (aTerm->Required & SearchTerm::Criteria::ADS_MATCH)
    {
        WCHAR szADSName[MAX_PATH];
        szADSName[0] = L'\0';  // avoid false positive warning C6054
        wcsncpy_s(szADSName, szAttrName, AttrNameLen);
        if (PathMatchSpec(szADSName, aTerm->ADSName.c_str()))
            return SearchTerm::Criteria::ADS_MATCH;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::RegexADS(const std::shared_ptr<FileFind::SearchTerm>& aTerm, LPCWSTR szAttrName, size_t AttrNameLen) const
{
    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;
    if (aTerm->Required & SearchTerm::Criteria::ADS_REGEX)
    {
        if (regex_match(szAttrName, szAttrName + AttrNameLen, aTerm->ADSNameRegEx))
            matchedSpec = static_cast<FileFind::SearchTerm::Criteria>(matchedSpec | SearchTerm::Criteria::ADS_REGEX);

        return matchedSpec;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::SizeMatch(const std::shared_ptr<FileFind::SearchTerm>& aTerm, ULONGLONG ullDataSize) const
{
    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;
    if (aTerm->Required & SearchTerm::Criteria::SIZE_EQ)
    {
        if (ullDataSize == aTerm->SizeEQ)
            matchedSpec |= SearchTerm::Criteria::SIZE_EQ;
    }
    else
    {
        if (aTerm->Required & SearchTerm::Criteria::SIZE_GT)
        {
            if (ullDataSize > aTerm->SizeG)
                matchedSpec |= SearchTerm::Criteria::SIZE_GT;
        }
        else if (aTerm->Required & SearchTerm::Criteria::SIZE_GE)
        {
            if (ullDataSize >= aTerm->SizeG)
                matchedSpec |= SearchTerm::Criteria::SIZE_GE;
        }

        if (aTerm->Required & SearchTerm::Criteria::SIZE_LT)
        {
            if (ullDataSize < aTerm->SizeL)
                matchedSpec |= SearchTerm::Criteria::SIZE_LT;
        }
        else if (aTerm->Required & SearchTerm::Criteria::SIZE_LE)
        {
            if (ullDataSize <= aTerm->SizeL)
                matchedSpec |= SearchTerm::Criteria::SIZE_LE;
        }
    }
    return matchedSpec;
}

FileFind::SearchTerm::Criteria FileFind::AddMatchingDataNameAndSize(
    const std::shared_ptr<SearchTerm>& aTerm,
    SearchTerm::Criteria requiredSpec,
    std::shared_ptr<Match>& aFileMatch,
    MFTRecord* pElt) const
{
    SearchTerm::Criteria retval = SearchTerm::Criteria::NONE;

    for (auto data_iter = begin(pElt->GetDataAttributes()); data_iter != end(pElt->GetDataAttributes()); ++data_iter)
    {
        SearchTerm::Criteria matchedDataNameOrSizeSpecs = SearchTerm::Criteria::NONE;
        if (requiredSpec & SearchTerm::Criteria::SIZE_EQ || requiredSpec & SearchTerm::Criteria::SIZE_GT
            || requiredSpec & SearchTerm::Criteria::SIZE_GE || requiredSpec & SearchTerm::Criteria::SIZE_LE
            || requiredSpec & SearchTerm::Criteria::SIZE_LT)
        {
            ULONGLONG ullDataSize = 0LL;

            if (FAILED((*data_iter)->DataSize(m_pVolReader, ullDataSize)))
            {
                continue;
            }

            SearchTerm::Criteria aSpec = SizeMatch(aTerm, ullDataSize);
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                continue;
            }
            matchedDataNameOrSizeSpecs = matchedDataNameOrSizeSpecs | aSpec;
        }
        if (requiredSpec & SearchTerm::Criteria::ADS_EXACT)
        {
            if (pElt->HasNamedDataAttr() && *data_iter != nullptr)
            {
                SearchTerm::Criteria aSpec = ExactADS(aTerm, (*data_iter)->NamePtr(), (*data_iter)->NameLength());
                if (aSpec == SearchTerm::Criteria::NONE)
                {
                    continue;
                }
                matchedDataNameOrSizeSpecs = matchedDataNameOrSizeSpecs | aSpec;
            }
        }
        if (requiredSpec & SearchTerm::Criteria::ADS_MATCH)
        {
            if (pElt->HasNamedDataAttr() && *data_iter != nullptr)
            {
                SearchTerm::Criteria aSpec = MatchADS(aTerm, (*data_iter)->NamePtr(), (*data_iter)->NameLength());
                if (aSpec == SearchTerm::Criteria::NONE)
                {
                    continue;
                }
                matchedDataNameOrSizeSpecs = matchedDataNameOrSizeSpecs | aSpec;
            }
        }
        if (requiredSpec & SearchTerm::Criteria::ADS_REGEX)
        {
            if (pElt->HasNamedDataAttr() && *data_iter != nullptr)
            {
                SearchTerm::Criteria aSpec = RegexADS(aTerm, (*data_iter)->NamePtr(), (*data_iter)->NameLength());
                if (aSpec == SearchTerm::Criteria::NONE)
                {
                    continue;
                }
                matchedDataNameOrSizeSpecs = matchedDataNameOrSizeSpecs | aSpec;
            }
        }
        if (matchedDataNameOrSizeSpecs == requiredSpec)
        {
            if (aFileMatch == nullptr)
                aFileMatch = std::make_shared<Match>(
                    m_pVolReader, aTerm, pElt->GetFileReferenceNumber(), !pElt->IsRecordInUse());

            if (m_bProvideStream)
                aFileMatch->AddAttributeMatch(_L_, m_pVolReader, *data_iter);
            else
                aFileMatch->AddAttributeMatch(*data_iter);

            retval = requiredSpec;
        }
    }

    return retval;
}

FileFind::SearchTerm::Criteria FileFind::ExcludeMatchingDataNameAndSize(
    const std::shared_ptr<SearchTerm>& aTerm,
    SearchTerm::Criteria requiredSpec,
    const std::shared_ptr<Match>& aFileMatch) const
{
    auto found = std::find_if(
        begin(aFileMatch->MatchingAttributes),
        end(aFileMatch->MatchingAttributes),
        [this, aTerm, requiredSpec](const Match::AttributeMatch& aMatch) -> bool {
        if (aMatch.Type != $DATA)
            return false;

        SearchTerm::Criteria matchedDataNameOrSizeSpecs = SearchTerm::Criteria::NONE;
        if (requiredSpec & SearchTerm::Criteria::SIZE_EQ || requiredSpec & SearchTerm::Criteria::SIZE_GT
            || requiredSpec & SearchTerm::Criteria::SIZE_GE || requiredSpec & SearchTerm::Criteria::SIZE_LE
            || requiredSpec & SearchTerm::Criteria::SIZE_LT)
        {
            SearchTerm::Criteria aSpec = SizeMatch(aTerm, aMatch.DataSize);
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                return false;
            }
            matchedDataNameOrSizeSpecs |= aSpec;
        }
        if (requiredSpec & SearchTerm::Criteria::ADS_EXACT)
        {
            SearchTerm::Criteria aSpec = ExactADS(aTerm, aMatch.AttrName.c_str(), aMatch.AttrName.size());
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                return false;
            }
            matchedDataNameOrSizeSpecs |= aSpec;
        }
        if (requiredSpec & SearchTerm::Criteria::ADS_MATCH)
        {
            SearchTerm::Criteria aSpec = MatchADS(aTerm, aMatch.AttrName.c_str(), aMatch.AttrName.size());
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                return false;
            }
            matchedDataNameOrSizeSpecs |= aSpec;
        }
        if (requiredSpec & SearchTerm::Criteria::ADS_REGEX)
        {
            SearchTerm::Criteria aSpec = RegexADS(aTerm, aMatch.AttrName.c_str(), aMatch.AttrName.size());
            if (aSpec == SearchTerm::Criteria::NONE)
            {
                return false;
            }
            matchedDataNameOrSizeSpecs |= aSpec;
        }

        if (matchedDataNameOrSizeSpecs == requiredSpec)
            return true;
        return false;
    });
    if (found != end(aFileMatch->MatchingAttributes))
    {
        return requiredSpec;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria FileFind::MatchHash(
    const std::shared_ptr<FileFind::SearchTerm>& aTerm,
    const std::shared_ptr<DataAttribute>& pDataAttr) const
{
    HRESULT hr = E_FAIL;
    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;

    if (aTerm->Required & SearchTerm::Criteria::DATA_MD5 || aTerm->Required & SearchTerm::Criteria::DATA_SHA1
        || aTerm->Required & SearchTerm::Criteria::DATA_SHA256)
    {
        if (pDataAttr == nullptr)
            return SearchTerm::Criteria::NONE;

        if (FAILED(hr = pDataAttr->GetHashInformation(_L_, m_pVolReader, m_NeededHash)))
        {
            log::Error(_L_, hr, L"Failed to compute hash for data attribute\r\n");
            return SearchTerm::Criteria::NONE;
        }

        if (aTerm->Required & SearchTerm::Criteria::DATA_MD5)
        {
            CBinaryBuffer& md5 = pDataAttr->GetDetails()->MD5();
            if (md5 == aTerm->MD5)
                matchedSpec |= SearchTerm::Criteria::DATA_MD5;
            else
                return SearchTerm::Criteria::NONE;
        }
        if (aTerm->Required & SearchTerm::Criteria::DATA_SHA1)
        {
            CBinaryBuffer& sha1 = pDataAttr->GetDetails()->SHA1();
            if (sha1 == aTerm->SHA1)
                matchedSpec |= SearchTerm::Criteria::DATA_SHA1;
            else
                return SearchTerm::Criteria::NONE;
        }
        if (aTerm->Required & SearchTerm::Criteria::DATA_SHA256)
        {
            CBinaryBuffer& sha256 = pDataAttr->GetDetails()->SHA256();
            if (sha256 == aTerm->SHA256)
                matchedSpec |= SearchTerm::Criteria::DATA_SHA256;
            else
                return SearchTerm::Criteria::NONE;
        }
    }
    return matchedSpec;
}

FileFind::SearchTerm::Criteria FileFind::MatchContains(
    const std::shared_ptr<FileFind::SearchTerm>& aTerm,
    const std::shared_ptr<DataAttribute>& pDataAttr) const
{
    HRESULT hr = E_FAIL;
    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;

    if (aTerm->Required & SearchTerm::Criteria::CONTAINS)
    {
        auto pDataStream = pDataAttr->GetDataStream(_L_, m_pVolReader);
        if (pDataStream == nullptr)
            return SearchTerm::Criteria::NONE;

        if (FAILED(hr = pDataStream->SetFilePointer(0LL, SEEK_SET, nullptr)))
        {
            log::Verbose(_L_, L"Failed to seek pointer to 0 for data attribute (hr=0x%lx)\r\n", hr);
            return SearchTerm::Criteria::NONE;
        }
        boost::algorithm::boyer_moore<BYTE*> boyermoore(aTerm->Contains.begin(), aTerm->Contains.end());

        CBinaryBuffer buffer;
        if (!buffer.SetCount((4 * 1024 * 1024) + aTerm->Contains.GetCount()))
            return SearchTerm::Criteria::NONE;

        ULONGLONG ullBytesRead = 0;
        ULONGLONG ullAccumulatedBytes = 0;
        ULONGLONG ullBytesToRead = pDataStream->GetSize();
        size_t carry = 0L;

        do
        {
            ullBytesRead = 0;
            CBinaryBuffer readContent(buffer.GetData() + carry, buffer.GetCount() - carry);
            if (FAILED(hr = pDataStream->Read(readContent.GetData(), readContent.GetCount(), &ullBytesRead)))
                break;
            ullAccumulatedBytes += ullBytesRead;

            CBinaryBuffer searchContent(buffer.GetData(), (size_t)ullBytesRead + carry);

            auto it = boyermoore(begin(searchContent), end(searchContent));
            auto nothing_found = std::make_pair(end(searchContent), end(searchContent));

            if (it != nothing_found)
            {
                matchedSpec = static_cast<FileFind::SearchTerm::Criteria>(
                    matchedSpec | (SearchTerm::Criteria::CONTAINS & aTerm->Required));
                return matchedSpec;
            }

            // ensure we don't miss matches where the 'needle' is on a boundary of the 4M buffers
            if (ullAccumulatedBytes < ullBytesToRead && ullBytesRead > aTerm->Contains.GetCount())
            {
                CopyMemory(
                    buffer.GetData(),
                    readContent.GetData() + ullBytesRead - aTerm->Contains.GetCount(),
                    aTerm->Contains.GetCount());
                carry = aTerm->Contains.GetCount();
            }

        } while (ullAccumulatedBytes < ullBytesToRead);

        if (FAILED(hr = pDataStream->SetFilePointer(0LL, SEEK_SET, nullptr)))
        {
            log::Verbose(_L_, L"Failed to seek pointer to 0 for data attribute (hr=0x%lx)\r\n", hr);
            return SearchTerm::Criteria::NONE;
        }
    }
    return matchedSpec;
}

std::pair<Orc::FileFind::SearchTerm::Criteria, std::optional<MatchingRuleCollection>> Orc::FileFind::MatchYara(
    const std::shared_ptr<SearchTerm>& aTerm,
    const std::shared_ptr<DataAttribute>& pDataAttr) const
{
    HRESULT hr = E_FAIL;
    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;

    if (!m_YaraScan)
    {
        log::Warning(_L_, HRESULT_FROM_WIN32(ERROR_INVALID_STATE), L"Yara not initialized & yara rules selected\r\n");
        return { SearchTerm::Criteria::NONE, std::nullopt };
    }

    if (aTerm->Required & SearchTerm::Criteria::YARA)
    {
        auto pDataStream = pDataAttr->GetDataStream(_L_, m_pVolReader);
        if (pDataStream == nullptr)
            return { SearchTerm::Criteria::NONE, std::nullopt };

        if (FAILED(hr = pDataStream->SetFilePointer(0LL, SEEK_SET, nullptr)))
        {
            log::Verbose(_L_, L"Failed to seek pointer to 0 for data attribute (hr=0x%lx)\r\n", hr);
            return { SearchTerm::Criteria::NONE, std::nullopt };
        }

        auto [hr, matchingRules] = m_YaraScan->Scan(pDataStream);
        if (FAILED(hr))
        {
            log::Verbose(_L_, L"Failed to yara scan data attribute (hr=0x%lx)\r\n", hr);
            return { SearchTerm::Criteria::NONE, std::nullopt };
        }
        if (!matchingRules.empty())
        {
            if (!aTerm->YaraRules.empty())
            {
                for (const auto& termRule : aTerm->YaraRules)
                {
                    for (const auto& matchingRule : matchingRules)
                    {
                        if (PathMatchSpecA(matchingRule.c_str(), termRule.c_str()))
                        {
                            return { SearchTerm::Criteria::YARA,
                                    matchingRules };  // With the first matchingRule in the rules spec, we have a winner
                        }
                    }
                }
                return { SearchTerm::Criteria::NONE,
                        std::nullopt };  // the stream matched more than one rule but not the specified one
            }
            else
                return { SearchTerm::Criteria::YARA, std::nullopt };
        }
    }
    return { matchedSpec, std::nullopt };
}

FileFind::SearchTerm::Criteria FileFind::MatchHeader(
    const std::shared_ptr<FileFind::SearchTerm>& aTerm,
    const std::shared_ptr<DataAttribute>& pDataAttr) const
{
    HRESULT hr = E_FAIL;
    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;

    if (aTerm->Required & SearchTerm::Criteria::HEADER)
    {
        auto pDataStream = pDataAttr->GetDataStream(_L_, m_pVolReader);
        if (pDataStream == nullptr)
            return SearchTerm::Criteria::NONE;

        if (FAILED(hr = pDataStream->SetFilePointer(0LL, SEEK_SET, nullptr)))
        {
            log::Verbose(_L_, L"Failed to seek pointer to 0 for data attribute (hr=0x%lx)\r\n", hr);
            return SearchTerm::Criteria::NONE;
        }

        CBinaryBuffer buffer;
        buffer.SetCount(aTerm->HeaderLen);
        ULONGLONG ullBytesRead = 0;
        ULONGLONG ullBytesToRead = aTerm->HeaderLen;

        if (FAILED(hr = pDataStream->Read(buffer.GetData(), ullBytesToRead, &ullBytesRead)))
            return SearchTerm::Criteria::NONE;

        if (FAILED(hr = pDataStream->SetFilePointer(0LL, SEEK_SET, nullptr)))
        {
            log::Verbose(_L_, L"Failed to seek pointer to 0 for data attribute (hr=0x%lx)\r\n", hr);
            return SearchTerm::Criteria::NONE;
        }

        // Match the header here
        if (ullBytesRead < ullBytesToRead)
            return SearchTerm::Criteria::NONE;
        if (!memcmp(buffer.GetData(), aTerm->Header.GetData(), aTerm->HeaderLen))
            return matchedSpec |= SearchTerm::Criteria::HEADER;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::RegExHeader(const std::shared_ptr<SearchTerm>& aTerm, const std::shared_ptr<DataAttribute>& pDataAttr) const
{
    HRESULT hr = E_FAIL;
    SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;

    if (aTerm->Required & SearchTerm::Criteria::HEADER_REGEX)
    {
        auto pDataStream = pDataAttr->GetDataStream(_L_, m_pVolReader);
        if (pDataStream == nullptr)
            return SearchTerm::Criteria::NONE;

        if (FAILED(hr = pDataStream->SetFilePointer(0LL, SEEK_SET, nullptr)))
        {
            log::Verbose(_L_, L"Failed to seek pointer to 0 for data attribute (hr=0x%lx)\r\n", hr);
            return SearchTerm::Criteria::NONE;
        }

        CBinaryBuffer buffer;
        buffer.SetCount(aTerm->HeaderLen);
        ULONGLONG ullBytesRead = 0;
        ULONGLONG ullBytesToRead = aTerm->HeaderLen;

        if (FAILED(hr = pDataStream->Read(buffer.GetData(), ullBytesToRead, &ullBytesRead)))
            return SearchTerm::Criteria::NONE;

        // Match the header here
        if (regex_match(
            (LPSTR)buffer.GetData(),
            ((LPSTR)buffer.GetData()) + (buffer.GetCount() / sizeof(CHAR)),
            aTerm->HeaderRegEx))
            return matchedSpec |= SearchTerm::Criteria::HEADER_REGEX;

        if (FAILED(hr = pDataStream->SetFilePointer(0LL, SEEK_SET, nullptr)))
        {
            log::Verbose(_L_, L"Failed to seek pointer to 0 for data attribute (hr=0x%lx)\r\n", hr);
            return SearchTerm::Criteria::NONE;
        }
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria
FileFind::HexHeader(const std::shared_ptr<SearchTerm>& aTerm, const std::shared_ptr<DataAttribute>& pDataAttr) const
{
    HRESULT hr = E_FAIL;

    if (aTerm->Required & SearchTerm::Criteria::HEADER_HEX)
    {
        SearchTerm::Criteria matchedSpec = SearchTerm::Criteria::NONE;

        auto pDataStream = pDataAttr->GetDataStream(_L_, m_pVolReader);
        if (pDataStream == nullptr)
            return SearchTerm::Criteria::NONE;

        if (FAILED(hr = pDataStream->SetFilePointer(0LL, SEEK_SET, nullptr)))
        {
            log::Verbose(_L_, L"Failed to seek pointer to 0 for data attribute (hr=0x%lx)\r\n", hr);
            return SearchTerm::Criteria::NONE;
        }

        CBinaryBuffer buffer;
        buffer.SetCount(aTerm->HeaderLen);
        ULONGLONG ullBytesRead = 0;
        ULONGLONG ullBytesToRead = aTerm->HeaderLen;

        if (FAILED(hr = pDataStream->Read(buffer.GetData(), ullBytesToRead, &ullBytesRead)))
            return SearchTerm::Criteria::NONE;

        // Match the header here
        if (ullBytesRead < ullBytesToRead)
            return SearchTerm::Criteria::NONE;

        if (!memcmp(buffer.GetData(), aTerm->Header.GetData(), aTerm->HeaderLen))
            return matchedSpec |= SearchTerm::Criteria::HEADER_HEX;

        if (FAILED(hr = pDataStream->SetFilePointer(0LL, SEEK_SET, nullptr)))
        {
            log::Verbose(_L_, L"Failed to seek pointer to 0 for data attribute (hr=0x%lx)\r\n", hr);
            return SearchTerm::Criteria::NONE;
        }
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria FileFind::AddMatchingData(
    const std::shared_ptr<SearchTerm>& aTerm,
    SearchTerm::Criteria requiredSpec,
    std::shared_ptr<Match>& aFileMatch,
    MFTRecord* pElt) const
{
    SearchTerm::Criteria requiredDataSpecs =
        static_cast<SearchTerm::Criteria>(aTerm->Required & SearchTerm::DataMask());
    SearchTerm::Criteria retval = SearchTerm::Criteria::NONE;
    for (const auto& data_attr : pElt->GetDataAttributes())
    {
        auto matchedDataSpecs = SearchTerm::Criteria::NONE;
        MatchingRuleCollection matchedRules;

        auto dataStream = data_attr->GetDataStream(_L_, m_pVolReader);
        if (dataStream == nullptr)
            continue;

        if (requiredDataSpecs & SearchTerm::Criteria::HEADER)
        {
            SearchTerm::Criteria aSpec = MatchHeader(aTerm, data_attr);
            if (aSpec == SearchTerm::Criteria::NONE)
                continue;
            matchedDataSpecs |= aSpec;
        }
        if (requiredDataSpecs & SearchTerm::Criteria::HEADER_HEX)
        {
            SearchTerm::Criteria aSpec = HexHeader(aTerm, data_attr);
            if (aSpec == SearchTerm::Criteria::NONE)
                continue;
            matchedDataSpecs |= aSpec;
        }
        if (requiredDataSpecs & SearchTerm::Criteria::HEADER_REGEX)
        {
            SearchTerm::Criteria aSpec = RegExHeader(aTerm, data_attr);
            if (aSpec == SearchTerm::Criteria::NONE)
                continue;
            matchedDataSpecs |= aSpec;
        }
        if (requiredDataSpecs & SearchTerm::Criteria::DATA_MD5 || requiredDataSpecs & SearchTerm::Criteria::DATA_SHA1
            || requiredDataSpecs & SearchTerm::Criteria::DATA_SHA256)
        {
            SearchTerm::Criteria aSpec = MatchHash(aTerm, data_attr);
            if (aSpec == SearchTerm::Criteria::NONE)
                continue;
            matchedDataSpecs |= aSpec;
        }
        if (requiredDataSpecs & SearchTerm::Criteria::CONTAINS)
        {
            SearchTerm::Criteria aSpec = MatchContains(aTerm, data_attr);
            if (aSpec == SearchTerm::Criteria::NONE)
                continue;
            matchedDataSpecs |= aSpec;
        }
        if (requiredDataSpecs & SearchTerm::Criteria::YARA)
        {
            auto [aSpec, matched] = MatchYara(aTerm, data_attr);
            if (matched.has_value())
                std::swap(matchedRules, matched.value());
            if (aSpec == SearchTerm::Criteria::NONE)
                continue;
            matchedDataSpecs |= aSpec;
        }
        if (matchedDataSpecs == requiredSpec)
        {
            if (aFileMatch == nullptr)
                aFileMatch = std::make_shared<Match>(
                    m_pVolReader, aTerm, pElt->GetFileReferenceNumber(), !pElt->IsRecordInUse());

            data_attr->GetHashInformation(_L_, m_pVolReader, m_MatchHash);

            if (m_bProvideStream)
                aFileMatch->AddAttributeMatch(_L_, m_pVolReader, data_attr, std::move(matchedRules));
            else
                aFileMatch->AddAttributeMatch(data_attr, std::move(matchedRules));

            retval = requiredSpec;
        }
    }
    return retval;
}

FileFind::SearchTerm::Criteria FileFind::ExcludeMatchingData(
    const std::shared_ptr<SearchTerm>& aTerm,
    SearchTerm::Criteria requiredSpec,
    const std::shared_ptr<Match>& aFileMatch) const
{
    SearchTerm::Criteria requiredDataSpecs = aTerm->Required & SearchTerm::DataMask();

    auto found = std::find_if(
        begin(aFileMatch->MatchingAttributes),
        end(aFileMatch->MatchingAttributes),
        [this, aTerm, requiredDataSpecs](const Match::AttributeMatch& attrMatch) -> bool {
        SearchTerm::Criteria matchedDataSpecs = SearchTerm::Criteria::NONE;

        if (attrMatch.Type != $DATA)
            return false;

        auto data_attr = attrMatch.DataAttr.lock();
        if (!data_attr)
            return false;

        if (requiredDataSpecs & SearchTerm::Criteria::HEADER)
        {
            SearchTerm::Criteria aSpec = MatchHeader(aTerm, data_attr);
            if (aSpec == SearchTerm::Criteria::NONE)
                return false;
            matchedDataSpecs |= aSpec;
        }
        if (requiredDataSpecs & SearchTerm::Criteria::HEADER_HEX)
        {
            SearchTerm::Criteria aSpec = HexHeader(aTerm, data_attr);
            if (aSpec == SearchTerm::Criteria::NONE)
                return false;
            matchedDataSpecs |= aSpec;
        }
        if (requiredDataSpecs & SearchTerm::Criteria::HEADER_REGEX)
        {
            SearchTerm::Criteria aSpec = RegExHeader(aTerm, data_attr);
            if (aSpec == SearchTerm::Criteria::NONE)
                return false;
            matchedDataSpecs |= aSpec;
        }
        if (requiredDataSpecs & SearchTerm::Criteria::DATA_MD5
            || requiredDataSpecs & SearchTerm::Criteria::DATA_SHA1
            || requiredDataSpecs & SearchTerm::Criteria::DATA_SHA256)
        {
            SearchTerm::Criteria aSpec = MatchHash(aTerm, data_attr);
            if (aSpec == SearchTerm::Criteria::NONE)
                return false;
            matchedDataSpecs |= aSpec;
        }
        if (requiredDataSpecs & SearchTerm::Criteria::CONTAINS)
        {
            SearchTerm::Criteria aSpec = MatchContains(aTerm, data_attr);
            if (aSpec == SearchTerm::Criteria::NONE)
                return false;
            matchedDataSpecs |= aSpec;
        }
        if (matchedDataSpecs == requiredDataSpecs)
            return true;
        return false;
    });
    if (found != end(aFileMatch->MatchingAttributes))
    {
        return requiredSpec;
    }
    return SearchTerm::Criteria::NONE;
}

FileFind::Match::NameMatch::NameMatch(const PFILE_NAME pFileName, const LPCWSTR szFullPath)
{
    if (auto pFileBytes = std::make_unique<BYTE[]>(sizeof(FILE_NAME) + pFileName->FileNameLength * sizeof(WCHAR)))
    {
        CopyMemory(pFileBytes.get(), pFileName, sizeof(FILE_NAME) + pFileName->FileNameLength * sizeof(WCHAR));
        std::swap(FileName, pFileBytes);
        _ASSERT(szFullPath);
        FullPathName.assign(szFullPath);
    }
}

FileFind::Match::NameMatch::NameMatch(const MFTWalker::FullNameBuilder pBuilder, PFILE_NAME pFileName)
{
    if (auto pFileBytes = std::make_unique<BYTE[]>(sizeof(FILE_NAME) + pFileName->FileNameLength * sizeof(WCHAR)))
    {
        CopyMemory(pFileBytes.get(), pFileName, sizeof(FILE_NAME) + pFileName->FileNameLength * sizeof(WCHAR));
        std::swap(FileName, pFileBytes);
        if (pBuilder != nullptr)
            FullPathName.assign((LPCWSTR)pBuilder(pFileName, nullptr));
    }
}

FileFind::Match::AttributeMatch::AttributeMatch(const std::shared_ptr<MftRecordAttribute>& pAttr)
    : DataSize(0)
{
    AttrName.assign(pAttr->NamePtr(), pAttr->NameLength());
    Type = pAttr->TypeCode();
    InstanceID = pAttr->Header()->Instance;

    if (pAttr->GetDetails())
    {
        MD5 = pAttr->GetDetails()->MD5();
        SHA1 = pAttr->GetDetails()->SHA1();
        SHA256 = pAttr->GetDetails()->SHA256();
    }
    DataAttr = std::dynamic_pointer_cast<DataAttribute>(pAttr);
    DataStream = pAttr->GetDetails()->GetDataStream();
    RawStream = pAttr->GetDetails()->GetRawStream();
}

FileFind::SearchTerm::Criteria FileFind::LookupTermInRecordAddMatching(
    const std::shared_ptr<SearchTerm>& aTerm,
    const SearchTerm::Criteria matched,
    std::shared_ptr<Match>& aFileMatch,
    MFTRecord* pElt) const
{
    SearchTerm::Criteria requiredSpecs = aTerm->Required;
    SearchTerm::Criteria matchedSpecs = matched;

    if (aTerm->DependsOnName())
    {
        SearchTerm::Criteria requiredNameSpecs =
            static_cast<SearchTerm::Criteria>(aTerm->Required & SearchTerm::NameMask());
        SearchTerm::Criteria matchedNameSpecs = AddMatchingName(aTerm, requiredNameSpecs, aFileMatch, pElt);
        if (requiredNameSpecs == matchedNameSpecs)
            matchedSpecs |= matchedNameSpecs;
        else
            return SearchTerm::Criteria::NONE;
    }
    if (aTerm->DependsOnPath())
    {
        SearchTerm::Criteria requiredPathSpecs =
            static_cast<SearchTerm::Criteria>(aTerm->Required & SearchTerm::PathMask());
        SearchTerm::Criteria matchedPathSpecs = AddMatchingPath(aTerm, requiredPathSpecs, aFileMatch, pElt);
        if (requiredPathSpecs == matchedPathSpecs)
            matchedSpecs |= matchedPathSpecs;
        else
            return SearchTerm::Criteria::NONE;
    }

    if (aTerm->DependsOnDataNameOrSize())
    {
        SearchTerm::Criteria requiredNameOrSizeSpecs =
            static_cast<SearchTerm::Criteria>(aTerm->Required & SearchTerm::DataNameOrSizeMask());
        SearchTerm::Criteria matchedDataNameOrSizeSpecs =
            AddMatchingDataNameAndSize(aTerm, requiredNameOrSizeSpecs, aFileMatch, pElt);
        if (matchedDataNameOrSizeSpecs == requiredNameOrSizeSpecs)
            matchedSpecs |= matchedDataNameOrSizeSpecs;
        else
            return SearchTerm::Criteria::NONE;
    }

    // before evaluating if more expensive attributes match, we check we are in location
    auto it = std::find_if(begin(pElt->GetFileNames()), end(pElt->GetFileNames()), [pElt, this](PFILE_NAME aName) {
        return m_InLocationBuilder(aName);
    });
    if (it == end(pElt->GetFileNames()))
    {
        // none of this record file name is in location
        // unappropriate to continue...
        return SearchTerm::Criteria::NONE;
    }

    if (aTerm->DependsOnAttribute())
    {
        SearchTerm::Criteria requiredAttributeSpecs =
            static_cast<SearchTerm::Criteria>(aTerm->Required & SearchTerm::AttributeMask());
        SearchTerm::Criteria matchedAttributeSpecs = MatchAttributes(aTerm, requiredAttributeSpecs, aFileMatch, pElt);
        if (requiredAttributeSpecs == matchedAttributeSpecs)
            matchedSpecs |= matchedAttributeSpecs;
        else
            return SearchTerm::Criteria::NONE;
    }

    if (aTerm->DependsOnData())
    {
        SearchTerm::Criteria requiredDataSpecs =
            static_cast<SearchTerm::Criteria>(aTerm->Required & SearchTerm::DataMask());
        SearchTerm::Criteria matchedDataSpecs = AddMatchingData(aTerm, requiredDataSpecs, aFileMatch, pElt);
        if (requiredDataSpecs == matchedDataSpecs)
            matchedSpecs |= matchedDataSpecs;
        else
            return SearchTerm::Criteria::NONE;
    }

    if (matchedSpecs == aTerm->Required)
    {
        // We do have a positive match. Fill in the blanks
        if (aFileMatch == nullptr)
            aFileMatch =
            std::make_shared<Match>(m_pVolReader, aTerm, pElt->GetFileReferenceNumber(), !pElt->IsRecordInUse());

        aFileMatch->Term = aTerm;
        aFileMatch->DeletedRecord = !pElt->IsRecordInUse();

        if (aFileMatch->MatchingNames.empty())
        {
            // No file name matched --> Easy! use default name
            PFILE_NAME pFileName = pElt->GetDefaultFileName();

            if (pFileName == nullptr)
            {
                LARGE_INTEGER* pLI = (LARGE_INTEGER*)&pElt->GetFileReferenceNumber();
                log::Error(
                    _L_,
                    E_POINTER,
                    L"Failed to find a default file name for record %I64X matching %s\r\n",
                    pLI->QuadPart,
                    aTerm->GetDescription().c_str());
            }
            else
            {
                if (!m_InLocationBuilder(pFileName))
                {
                    // the selected file name is not in location, try to find another one
                    auto it2 = std::find_if(
                        begin(pElt->GetFileNames()), end(pElt->GetFileNames()), [pElt, this](PFILE_NAME aName) -> bool {
                        return m_InLocationBuilder(aName);
                    });
                    if (it2 == end(pElt->GetFileNames()))
                        return SearchTerm::Criteria::NONE;  // we have been unable to find a name in location
                    else
                        pFileName = *it2;
                }
                aFileMatch->AddFileNameMatch(m_FullNameBuilder, pFileName);
            }
        }

        auto pSI = pElt->GetStandardInformation();
        if (pSI != nullptr)
        {
            aFileMatch->StandardInformation = std::unique_ptr<STANDARD_INFORMATION>(new STANDARD_INFORMATION);
            memcpy_s(
                aFileMatch->StandardInformation.get(), sizeof(STANDARD_INFORMATION), pSI, sizeof(STANDARD_INFORMATION));
        }

        if (aFileMatch->MatchingAttributes.empty())
        {
            // No data associated? Easy! assume default $data stream
            auto first = std::find_if(
                begin(pElt->GetDataAttributes()),
                end(pElt->GetDataAttributes()),
                [](const std::shared_ptr<DataAttribute>& pDataAttr) { return pDataAttr->NameLength() == 0L; });
            if (first != end(pElt->GetDataAttributes()))
            {
                if (m_bProvideStream)
                    aFileMatch->AddAttributeMatch(_L_, m_pVolReader, *first);
                else
                    aFileMatch->AddAttributeMatch(*first);
            }
        }

        if (NtfsFullSegmentNumber(&aFileMatch->FRN) == 0LL)
        {
            aFileMatch->FRN = pElt->GetFileReferenceNumber();
        }
    }
    if (matchedSpecs == requiredSpecs)
        return matchedSpecs;
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria FileFind::LookupTermIn$I30AddMatching(
    const std::shared_ptr<SearchTerm>& aTerm,
    const SearchTerm::Criteria matched,
    std::shared_ptr<Match>& aFileMatch,
    const PFILE_NAME pFileName) const
{
    SearchTerm::Criteria requiredSpecs = aTerm->Required;
    SearchTerm::Criteria matchedSpecs = matched;

    if (aTerm->DependsOnName())
    {
        SearchTerm::Criteria requiredNameSpecs =
            static_cast<SearchTerm::Criteria>(aTerm->Required & SearchTerm::NameMask());
        SearchTerm::Criteria matchedNameSpecs = MatchName(aTerm, requiredNameSpecs, aFileMatch, pFileName);
        if (requiredNameSpecs == matchedNameSpecs)
            matchedSpecs |= matchedNameSpecs;
        else
            return SearchTerm::Criteria::NONE;
    }
    if (aTerm->DependsOnPath())
    {
        SearchTerm::Criteria requiredPathSpecs =
            static_cast<SearchTerm::Criteria>(aTerm->Required & SearchTerm::PathMask());
        SearchTerm::Criteria matchedPathSpecs = MatchPath(aTerm, requiredPathSpecs, aFileMatch, pFileName);
        if (requiredPathSpecs == matchedPathSpecs)
            matchedSpecs |= matchedPathSpecs;
        else
            return SearchTerm::Criteria::NONE;
    }

    if (matchedSpecs == aTerm->Required)
    {
        // We do have a positive match. Fill in the blanks
        if (aFileMatch == nullptr)
            aFileMatch = std::make_shared<Match>(m_pVolReader, aTerm);

        aFileMatch->Term = aTerm;

        if (aFileMatch->MatchingNames.empty())
        {
            if (!m_InLocationBuilder(pFileName))
            {
                return SearchTerm::Criteria::NONE;  // we have been unable to find a name in location
            }
            aFileMatch->AddFileNameMatch(m_FullNameBuilder, pFileName);
        }
    }
    if (matchedSpecs == requiredSpecs)
        return matchedSpecs;
    return SearchTerm::Criteria::NONE;
}

FileFind::SearchTerm::Criteria FileFind::LookupTermInMatchExcludeMatching(
    const std::shared_ptr<SearchTerm>& aTerm,
    const SearchTerm::Criteria matched,
    const std::shared_ptr<Match>& aFileMatch) const
{
    SearchTerm::Criteria requiredSpecs = aTerm->Required;
    SearchTerm::Criteria matchedSpecs = matched;

    if (aTerm->DependsOnName())
    {
        SearchTerm::Criteria requiredNameSpecs =
            static_cast<SearchTerm::Criteria>(aTerm->Required & SearchTerm::NameMask());
        SearchTerm::Criteria matchedNameSpecs = ExcludeMatchingName(aTerm, requiredNameSpecs, aFileMatch);
        if (requiredNameSpecs == matchedNameSpecs)
            matchedSpecs |= matchedNameSpecs;
        else
            return SearchTerm::Criteria::NONE;
    }
    if (aTerm->DependsOnPath())
    {
        SearchTerm::Criteria requiredPathSpecs =
            static_cast<SearchTerm::Criteria>(aTerm->Required & SearchTerm::PathMask());
        SearchTerm::Criteria matchedPathSpecs = ExcludeMatchingPath(aTerm, requiredPathSpecs, aFileMatch);
        if (requiredPathSpecs == matchedPathSpecs)
            matchedSpecs |= matchedPathSpecs;
        else
            return SearchTerm::Criteria::NONE;
    }

    if (aTerm->DependsOnDataNameOrSize())
    {
        SearchTerm::Criteria requiredNameOrSizeSpecs =
            static_cast<SearchTerm::Criteria>(aTerm->Required & SearchTerm::DataNameOrSizeMask());
        SearchTerm::Criteria matchedDataNameOrSizeSpecs =
            ExcludeMatchingDataNameAndSize(aTerm, requiredNameOrSizeSpecs, aFileMatch);
        if (matchedDataNameOrSizeSpecs == requiredNameOrSizeSpecs)
            matchedSpecs |= matchedDataNameOrSizeSpecs;
        else
            return SearchTerm::Criteria::NONE;
    }

    if (aTerm->DependsOnAttribute())
    {
        SearchTerm::Criteria requiredAttributeSpecs =
            static_cast<SearchTerm::Criteria>(aTerm->Required & SearchTerm::AttributeMask());
        SearchTerm::Criteria matchedAttributeSpecs =
            ExcludeMatchingAttributes(aTerm, requiredAttributeSpecs, aFileMatch);
        if (requiredAttributeSpecs == matchedAttributeSpecs)
            matchedSpecs |= matchedAttributeSpecs;
        else
            return SearchTerm::Criteria::NONE;
    }

    if (aTerm->DependsOnData())
    {
        SearchTerm::Criteria requiredDataSpecs =
            static_cast<SearchTerm::Criteria>(aTerm->Required & SearchTerm::DataMask());
        SearchTerm::Criteria matchedDataSpecs = ExcludeMatchingData(aTerm, requiredDataSpecs, aFileMatch);
        if (requiredDataSpecs == matchedDataSpecs)
            matchedSpecs |= matchedDataSpecs;
        else
            return SearchTerm::Criteria::NONE;
    }

    if (matchedSpecs == requiredSpecs)
        return matchedSpecs;
    return SearchTerm::Criteria::NONE;
}

HRESULT FileFind::ComputeMatchHashes(const std::shared_ptr<Match>& aMatch)
{
    HRESULT hr = E_FAIL;

    for (auto& attr_match : aMatch->MatchingAttributes)
    {
        CryptoHashStream::Algorithm needed = CryptoHashStream::Algorithm::Undefined;
        if (m_MatchHash & CryptoHashStream::Algorithm::MD5 && attr_match.MD5.empty())
            needed |= CryptoHashStream::Algorithm::MD5;
        if (m_MatchHash & CryptoHashStream::Algorithm::SHA1 && attr_match.SHA1.empty())
            needed |= CryptoHashStream::Algorithm::SHA1;
        if (m_MatchHash & CryptoHashStream::Algorithm::SHA256 && attr_match.SHA256.empty())
            needed |= CryptoHashStream::Algorithm::SHA256;

        if (needed != CryptoHashStream::Algorithm::Undefined)
        {
            auto stream = attr_match.DataStream;

            if (stream == nullptr)
                return E_POINTER;

            stream->SetFilePointer(0L, FILE_BEGIN, NULL);

            auto hashstream = std::make_shared<CryptoHashStream>(_L_);
            if (hashstream == nullptr)
                return E_OUTOFMEMORY;

            if (FAILED(hr = hashstream->OpenToWrite(needed, nullptr)))
                return hr;

            ULONGLONG ullWritten = 0LL;
            if (FAILED(hr = stream->CopyTo(*hashstream, &ullWritten)))
                return hr;

            if (ullWritten > 0)
            {
                if (needed & CryptoHashStream::Algorithm::MD5
                    && FAILED(hr = hashstream->GetHash(CryptoHashStream::Algorithm::MD5, attr_match.MD5)))
                {
                    if (hr != MK_E_UNAVAILABLE)
                        return hr;
                }
                if (needed & CryptoHashStream::Algorithm::SHA1
                    && FAILED(hr = hashstream->GetHash(CryptoHashStream::Algorithm::SHA1, attr_match.SHA1)))
                {
                    if (hr != MK_E_UNAVAILABLE)
                        return hr;
                }
                if (needed & CryptoHashStream::Algorithm::SHA256
                    && FAILED(hr = hashstream->GetHash(CryptoHashStream::Algorithm::SHA256, attr_match.SHA256)))
                {
                    if (hr != MK_E_UNAVAILABLE)
                        return hr;
                }
            }
        }
    }
    return S_OK;
}

HRESULT FileFind::EvaluateMatchCallCallback(
    FileFind::FoundMatchCallback aCallback,
    bool& bStop,
    const std::shared_ptr<Match>& aMatch)
{
    HRESULT hr = E_FAIL;

    if (FAILED(hr = ExcludeMatch(aMatch)))
        return hr;

    if (hr == S_FALSE)
    {
        if (m_MatchHash != CryptoHashStream::Algorithm::Undefined)
        {
            if (FAILED(hr = ComputeMatchHashes(aMatch)))
            {
                log::Warning(
                    _L_,
                    hr,
                    L"Failed to compute hashs for match %s\r\n",
                    aMatch->MatchingNames.front().FullPathName.c_str());
            }
        }

        // the match has not matched a excluding term
        log::Verbose(_L_, L"Adding match %s\r\n", aMatch->MatchingNames.front().FullPathName.c_str());
        m_Matches.push_back(aMatch);
        if (aCallback)
            aCallback(aMatch, bStop);
    }
    else
    {
        log::Verbose(_L_, L"Match has been excluded\r\n");
    }
    return S_OK;
}

HRESULT FileFind::FindMatch(MFTRecord* pElt, bool& bStop, FileFind::FoundMatchCallback aCallback)
{
    HRESULT hr = E_FAIL;
    shared_ptr<FileFind::Match> retval;

    if (!m_ExactNameTerms.empty() || (!m_ExactPathTerms.empty() && m_FullNameBuilder != nullptr))
    {
        auto& names = pElt->GetFileNames();

        for (auto it = begin(names); it != end(names); ++it)
        {
            PFILE_NAME aName = *it;

            if (!m_InLocationBuilder(aName))
                continue;

            std::wstring strName;
            std::wstring strPath;

            if (!m_ExactNameTerms.empty())
            {
                strName.assign(aName->FileName, aName->FileNameLength);
                auto name_list = m_ExactNameTerms.equal_range(strName);

                for (auto name_it = name_list.first; name_it != name_list.second; ++name_it)
                {
                    auto matched =
                        LookupTermInRecordAddMatching(name_it->second, SearchTerm::Criteria::NAME_EXACT, retval, pElt);
                    if (matched != SearchTerm::Criteria::NONE)
                    {
                        // we do have a match!
                        if (FAILED(hr = EvaluateMatchCallCallback(aCallback, bStop, retval)))
                            return hr;
                        retval.reset();
                    }
                    else if (retval != nullptr)
                        retval->Reset();
                }
            }
            if (!m_ExactPathTerms.empty() && m_FullNameBuilder != nullptr)
            {
                strPath.assign(m_FullNameBuilder(aName, nullptr));

                auto path_list = m_ExactPathTerms.equal_range(strPath);

                for (auto path_it = path_list.first; path_it != path_list.second; ++path_it)
                {
                    auto matched =
                        LookupTermInRecordAddMatching(path_it->second, SearchTerm::Criteria::PATH_EXACT, retval, pElt);
                    if (matched != SearchTerm::Criteria::NONE)
                    {
                        // we do have a match!
                        if (FAILED(hr = EvaluateMatchCallCallback(aCallback, bStop, retval)))
                            return hr;
                        retval.reset();
                    }
                    else if (retval != nullptr)
                        retval->Reset();
                }
            }
        }
    }

    if (!m_SizeTerms.empty())
    {
        auto& data_attrs = pElt->GetDataAttributes();

        for (auto& data_attr : data_attrs)
        {
            ULONGLONG ullDataSize = 0LL;

            if (FAILED(data_attr->DataSize(m_pVolReader, ullDataSize)))
            {
                continue;
            }

            auto attr_list = m_SizeTerms.equal_range(ullDataSize);

            for (auto attr_it = attr_list.first; attr_it != attr_list.second; ++attr_it)
            {
                auto matched =
                    LookupTermInRecordAddMatching(attr_it->second, SearchTerm::Criteria::SIZE_EQ, retval, pElt);
                if (matched != SearchTerm::Criteria::NONE)
                {
                    // we do have a match!
                    if (FAILED(hr = EvaluateMatchCallCallback(aCallback, bStop, retval)))
                        return hr;
                    retval.reset();
                }
                else if (retval != nullptr)
                    retval->Reset();
            }
        }
    }

    for (auto term_it : m_Terms)
    {
        auto matched = LookupTermInRecordAddMatching(term_it, SearchTerm::Criteria::NONE, retval, pElt);
        if (matched != SearchTerm::Criteria::NONE)
        {
            // we do have a match!
            if (FAILED(hr = EvaluateMatchCallCallback(aCallback, bStop, retval)))
                return hr;
            retval.reset();
        }
        else if (retval != nullptr)
            retval->Reset();
    }

    return S_OK;
}

HRESULT FileFind::FindI30Match(const PFILE_NAME pFileName, bool& bStop, FileFind::FoundMatchCallback aCallback)
{
    HRESULT hr = E_FAIL;
    shared_ptr<FileFind::Match> retval;

    std::wstring strName;
    std::wstring strPath;

    if (!m_ExactNameTerms.empty())
    {
        strName.assign(pFileName->FileName, pFileName->FileNameLength);
        auto name_list = m_ExactNameTerms.equal_range(strName);

        for (auto name_it = name_list.first; name_it != name_list.second; ++name_it)
        {
            // we do have a match!
            auto matched =
                LookupTermIn$I30AddMatching(name_it->second, SearchTerm::Criteria::NAME_EXACT, retval, pFileName);

            if (matched != SearchTerm::Criteria::NONE)
            {
                if (FAILED(hr = EvaluateMatchCallCallback(aCallback, bStop, retval)))
                    return hr;
                retval.reset();
            }
            else if (retval != nullptr)
                retval->Reset();
        }
    }
    if (!m_ExactPathTerms.empty() && m_FullNameBuilder != nullptr)
    {
        strPath.assign(m_FullNameBuilder(pFileName, nullptr));

        auto path_list = m_ExactPathTerms.equal_range(strPath);

        for (auto path_it = path_list.first; path_it != path_list.second; ++path_it)
        {
            auto matched =
                LookupTermIn$I30AddMatching(path_it->second, SearchTerm::Criteria::PATH_EXACT, retval, pFileName);
            if (matched != SearchTerm::Criteria::NONE)
            {
                // we do have a match!
                if (FAILED(hr = EvaluateMatchCallCallback(aCallback, bStop, retval)))
                    return hr;
                retval.reset();
            }
            else if (retval != nullptr)
                retval->Reset();
        }
    }
    for (auto term_it = begin(m_Terms); term_it != end(m_Terms); ++term_it)
    {
        auto matched = LookupTermIn$I30AddMatching(*term_it, SearchTerm::Criteria::NONE, retval, pFileName);
        if (matched != SearchTerm::Criteria::NONE)
        {
            // we do have a match!
            if (FAILED(hr = EvaluateMatchCallCallback(aCallback, bStop, retval)))
                return hr;
            retval.reset();
        }
        else if (retval != nullptr)
            retval->Reset();
    }

    return S_OK;
}

CryptoHashStream::Algorithm FileFind::GetNeededHashAlgorithms()
{
    auto getNeededHash = [](const std::shared_ptr<FileFind::SearchTerm>& term) -> CryptoHashStream::Algorithm {
        CryptoHashStream::Algorithm retval = CryptoHashStream::Algorithm::Undefined;

        if (term->Required & SearchTerm::Criteria::DATA_MD5)
        {
            retval |= CryptoHashStream::Algorithm::MD5;
        }
        if (term->Required & SearchTerm::Criteria::DATA_SHA1)
        {
            retval |= CryptoHashStream::Algorithm::SHA1;
        }
        if (term->Required & SearchTerm::Criteria::DATA_SHA256)
        {
            retval |= CryptoHashStream::Algorithm::SHA256;
        }
        return retval;
    };

    CryptoHashStream::Algorithm needed = CryptoHashStream::Algorithm::Undefined;

    for (const auto& term : m_ExactNameTerms)
    {
        needed |= getNeededHash(term.second);
    }

    for (const auto& term : m_ExactPathTerms)
    {
        needed |= getNeededHash(term.second);
    }

    for (const auto& term : m_SizeTerms)
    {
        needed |= getNeededHash(term.second);
    }

    for (const auto& term : m_Terms)
    {
        needed |= getNeededHash(term);
    }

    for (const auto& term : m_ExcludeNameTerms)
    {
        needed |= getNeededHash(term.second);
    }

    for (const auto& term : m_ExcludePathTerms)
    {
        needed |= getNeededHash(term.second);
    }

    for (const auto& term : m_ExcludeSizeTerms)
    {
        needed |= getNeededHash(term.second);
    }

    for (const auto& term : m_ExcludeTerms)
    {
        needed |= getNeededHash(term);
    }

    return needed;
}

HRESULT FileFind::ExcludeMatch(const std::shared_ptr<Match>& aMatch)
{
    if (!m_ExcludeNameTerms.empty() || !m_ExcludePathTerms.empty())
    {
        auto found = std::find_if(
            begin(aMatch->MatchingNames),
            end(aMatch->MatchingNames),
            [this, aMatch](const Match::NameMatch& nameMatch) -> bool {
            if (!m_ExcludeNameTerms.empty())
            {
                wstring strName;
                strName.assign(nameMatch.FILENAME()->FileName, nameMatch.FILENAME()->FileNameLength);
                auto name_list = m_ExcludeNameTerms.equal_range(strName);

                for (auto name_it = name_list.first; name_it != name_list.second; ++name_it)
                {
                    auto matched =
                        LookupTermInMatchExcludeMatching(name_it->second, SearchTerm::Criteria::NAME_EXACT, aMatch);
                    if (matched != SearchTerm::Criteria::NONE)
                    {
                        // we do have a match!
                        return true;
                    }
                }
            }
            if (!m_ExcludePathTerms.empty())
            {
                auto path_list = m_ExcludePathTerms.equal_range(nameMatch.FullPathName);
                for (auto path_it = path_list.first; path_it != path_list.second; ++path_it)
                {
                    auto matched =
                        LookupTermInMatchExcludeMatching(path_it->second, SearchTerm::Criteria::PATH_EXACT, aMatch);
                    if (matched != SearchTerm::Criteria::NONE)
                    {
                        // we do have a Match
                        return true;
                    }
                }
            }
            return false;
        });

        if (found != end(aMatch->MatchingNames))
            return S_OK;
    }

    if (!m_ExcludeSizeTerms.empty())
    {
        for (auto& match_attr : aMatch->MatchingAttributes)
        {
            auto attr_list = m_ExcludeSizeTerms.equal_range(match_attr.DataSize);

            for (auto attr_it = attr_list.first; attr_it != attr_list.second; ++attr_it)
            {
                auto matched = LookupTermInMatchExcludeMatching(attr_it->second, SearchTerm::Criteria::SIZE_EQ, aMatch);
                if (matched != SearchTerm::Criteria::NONE)
                {
                    // we do have a Match
                    return S_OK;
                }
            }
        }
    }

    for (auto term_it = begin(m_ExcludeTerms); term_it != end(m_ExcludeTerms); ++term_it)
    {
        auto matched = LookupTermInMatchExcludeMatching(*term_it, SearchTerm::Criteria::NONE, aMatch);
        if (matched != SearchTerm::Criteria::NONE)
        {
            return S_OK;
        }
    }

    return S_FALSE;
}

HRESULT FileFind::Find(const LocationSet& locations, FileFind::FoundMatchCallback aCallback, bool bParseI30Data)
{
    HRESULT hr = E_FAIL;

    if (m_ExactNameTerms.empty() && m_ExactPathTerms.empty() && m_Terms.empty() && m_SizeTerms.empty()
        && m_I30ExactNameTerms.empty() && m_I30ExactPathTerms.empty() && m_I30Terms.empty())
        return S_OK;

    const auto& lowest_locs = locations.GetAltitudeLocations();
    std::vector<std::shared_ptr<Location>> locs;

    // keep only the locations we're parsing
    std::copy_if(
        begin(lowest_locs), end(lowest_locs), back_inserter(locs), [](const shared_ptr<Location>& item) -> bool {
        return item->GetParse();
    });

    m_NeededHash = GetNeededHashAlgorithms();

    if (FAILED(hr = InitializeYara()))
        return hr;

    for (const auto& aLoc : locs)
    {
        HRESULT hr = E_FAIL;
        MFTWalker walk(_L_);

        m_FullNameBuilder = walk.GetFullNameBuilder();
        m_InLocationBuilder = walk.GetInLocationBuilder();

        m_pVolReader = aLoc->GetReader();

        if (FAILED(hr = walk.Initialize(aLoc, false)))
        {
            if (hr == HRESULT_FROM_WIN32(ERROR_FILE_SYSTEM_LIMITATION))
            {
                log::Verbose(
                    _L_, L"\tFile system not eligible for volume %s (%lx)\r\n\r\n", aLoc->GetLocation().c_str(), hr);
            }
            else
            {
                log::Verbose(
                    _L_, L"\tFailed to init walk for volume %s (%lx)\r\n\r\n", aLoc->GetLocation().c_str(), hr);
            }
        }
        else
        {
            MFTWalker::Callbacks cbs;
            auto pCB = aCallback;

            bool bStop = false;

            cbs.ElementCallback =
                [this, aCallback, &bStop, &hr](const std::shared_ptr<VolumeReader>& volreader, MFTRecord* pElt) {
                DBG_UNREFERENCED_PARAMETER(volreader);
                try
                {
                    if (pElt)
                    {
                        if (FAILED(hr = FindMatch(pElt, bStop, aCallback)))
                        {
                            log::Error(_L_, hr, L"FindMatch failed\r\n");
                            pElt->CleanCachedData();
                            return;
                        }
                        pElt->CleanCachedData();
                    }
                }
                catch (WCHAR * e)
                {
                    log::Error(_L_, E_ABORT, L"\r\nCould not parse record for %s : %s\r\n", nullptr, e);
                }
                return;
            };

            cbs.ProgressCallback = [&bStop](ULONG ulProgress) -> HRESULT {
                if (bStop)
                {
                    return HRESULT_FROM_WIN32(ERROR_NO_MORE_FILES);
                }
                return S_OK;
            };

            if (bParseI30Data && (!m_I30ExactNameTerms.empty() || !m_I30ExactPathTerms.empty() || !m_I30Terms.empty()))
            {
                cbs.I30Callback = [this, aCallback, &bStop, &hr](
                    const std::shared_ptr<VolumeReader>& volreader,
                    MFTRecord* pElt,
                    const PINDEX_ENTRY pEntry,
                    const PFILE_NAME pFileName,
                    bool bCarvedEntry) {
                    DBG_UNREFERENCED_PARAMETER(volreader);
                    DBG_UNREFERENCED_PARAMETER(bCarvedEntry);
                    DBG_UNREFERENCED_PARAMETER(pEntry);
                    DBG_UNREFERENCED_PARAMETER(pElt);
                    try
                    {
                        if (FAILED(hr = FindI30Match(pFileName, bStop, aCallback)))
                        {
                            log::Error(_L_, hr, L"FindI30Match failed\r\n");
                            return;
                        }
                    }
                    catch (WCHAR * e)
                    {
                        log::Error(_L_, E_ABORT, L"\r\nCould not parse record : %s\r\n", e);
                    }
                    return;
                };
            }

            if (FAILED(hr = walk.Walk(cbs)))
            {
                log::Verbose(_L_, L"Failed to walk volume %s (%lx)\r\n", aLoc->GetLocation().c_str(), hr);
            }
            else
            {
                log::Verbose(_L_, L"Done!\r\n");
                walk.Statistics(L"Done");
            }
        }
    }

    return S_OK;
}

void FileFind::PrintSpecs() const
{
    std::for_each(
        begin(m_ExactNameTerms),
        end(m_ExactNameTerms),
        [this](const pair<std::wstring, std::shared_ptr<SearchTerm>>& aPair) {
        log::Info(_L_, L"\t%s\r\n", aPair.second->GetDescription().c_str());
    });

    std::for_each(
        begin(m_ExactPathTerms),
        end(m_ExactPathTerms),
        [this](const pair<std::wstring, std::shared_ptr<SearchTerm>>& aPair) {
        log::Info(_L_, L"\t%s\r\n", aPair.second->GetDescription().c_str());
    });

    std::for_each(begin(m_Terms), end(m_Terms), [this](const std::shared_ptr<SearchTerm>& aTerm) {
        log::Info(_L_, L"\t%s\r\n", aTerm->GetDescription().c_str());
    });

    return;
}

FileFind::~FileFind(void) {}
