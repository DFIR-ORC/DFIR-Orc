//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright © 2011-2019 ANSSI. All Rights Reserved.
//
// Author(s): Jean Gautier (ANSSI)
//

#include "StdAfx.h"

#include "MFTWalker.h"

#include "LogFileWriter.h"

#include "MemoryStream.h"
#include "MountedVolumeReader.h"
#include "OfflineMFTReader.h"

#include "MFTOnline.h"
#include "MFTOffline.h"

#include "OrcException.h"

#include <boost/scope_exit.hpp>

using namespace std;
using namespace Orc;

// Number of items in the VirtualStore
constexpr auto SEGMENT_MAX_NUMBER = (0x10000);

HCRYPTPROV MFTRecord::g_hProv = NULL;

MFTWalker::MFTFileNameWrapper::MFTFileNameWrapper(const PFILE_NAME pFileName)
{
    _ASSERT(pFileName != NULL);
    const size_t size = sizeof(FILE_NAME) + (pFileName->FileNameLength * sizeof(WCHAR));
    m_pFileName = (PFILE_NAME)malloc(size);
    if (m_pFileName == NULL)
        throw std::exception("Out of memory");
    CopyMemory(m_pFileName, pFileName, size);
    m_InLocation = boost::indeterminate;
}

HRESULT MFTWalker::Initialize(const shared_ptr<Location>& loc, bool bIncludeNoInUse)
{
    HRESULT hr = E_FAIL;

    m_bIncludeNotInUse = bIncludeNoInUse;
    m_pVolReader = loc->GetReader();

    if (m_pVolReader == nullptr)
        return E_OUTOFMEMORY;

    if (FAILED(hr = m_pVolReader->LoadDiskProperties()))
    {
        log::Error(_L_, hr, L"Failed to load disk properties for location %s\r\n", loc->GetLocation().c_str());
        return hr;
    }

    if (Location::Type::OfflineMFT == loc->GetType())
    {
        auto pOfflineReader = std::dynamic_pointer_cast<OfflineMFTReader>(m_pVolReader);
        if (pOfflineReader)
            m_pMFT = std::make_unique<MFTOffline>(_L_, pOfflineReader);
        else
            return hr;
    }
    else
    {
        m_pMFT = std::make_unique<MFTOnline>(_L_, m_pVolReader);
    }

    if (FAILED(m_pMFT->Initialize()))
        return hr;

    if (!loc->GetSubDirs().empty())
    {
        auto& SpecificLocations = loc->GetSubDirs();
        std::for_each(begin(SpecificLocations), end(SpecificLocations), [this](const std::wstring& location) {
            DWORD dwLen = ExpandEnvironmentStringsW(location.c_str(), NULL, 0L);
            if (dwLen > MAX_PATH || dwLen <= 0)
                return;

            WCHAR* szExpandedLocation = (WCHAR*)calloc(dwLen + 1, sizeof(WCHAR));
            if (szExpandedLocation == nullptr)
                return;

            BOOST_SCOPE_EXIT(&szExpandedLocation) { free(szExpandedLocation); }
            BOOST_SCOPE_EXIT_END;

            dwLen = ExpandEnvironmentStringsW(location.c_str(), szExpandedLocation, dwLen + 1);
            if (dwLen == 0L)
                return;

            wstring loc(szExpandedLocation, dwLen - 1 /*we shall not include the trailing \0 */);

            if (loc.back() != L'\\')
                loc.append(L"\\");

            m_Locations.insert(std::move(loc));
        });

        if (m_Locations.size() == 1 && m_Locations.find(L"\\") != end(m_Locations))
        {
            // if subdirs is only \, then remove subdirs
            m_Locations.clear();
        }
    }

    if (FAILED(hr = m_SegmentStore.InitializeStore(0L, sizeof(MFTRecord) + m_pVolReader->GetBytesPerFRS())))
    {
        return hr;
    }
    return S_OK;
}

HRESULT MFTWalker::ExtendNameBuffer(WCHAR** pCurrent)
{
    WCHAR* pNewBuf = NULL;

    pNewBuf = (WCHAR*)HeapAlloc(GetProcessHeap(), 0L, m_dwFullNameBufferLen + m_pVolReader->MaxComponentLength());
    if (!pNewBuf)
        return E_OUTOFMEMORY;

    if (m_pFullNameBuffer)
    {
#ifdef DEBUG
        wmemset(pNewBuf, L'#', (m_dwFullNameBufferLen + m_pVolReader->MaxComponentLength()) / sizeof(WCHAR));
        pNewBuf[(m_dwFullNameBufferLen + m_pVolReader->MaxComponentLength()) / sizeof(WCHAR) - 1] = 0;
#endif
        memcpy_s(
            pNewBuf + (m_pVolReader->MaxComponentLength() / sizeof(WCHAR)) - 1,
            m_dwFullNameBufferLen + m_pVolReader->MaxComponentLength(),
            m_pFullNameBuffer,
            m_dwFullNameBufferLen);
    }

    if (pCurrent)
    {
        DWORD dwIndex = (DWORD)(*pCurrent - m_pFullNameBuffer);

        *pCurrent = pNewBuf + (m_pVolReader->MaxComponentLength() / sizeof(WCHAR)) - 1;
        *pCurrent += dwIndex;
        if (*pCurrent >= (pNewBuf + (m_dwFullNameBufferLen + m_pVolReader->MaxComponentLength()) / sizeof(WCHAR)))
        {
            HeapFree(GetProcessHeap(), 0L, pNewBuf);
            return E_FAIL;
        }
    }

    if (m_pFullNameBuffer)
        HeapFree(GetProcessHeap(), 0L, m_pFullNameBuffer);

    m_pFullNameBuffer = pNewBuf;
    m_dwFullNameBufferLen += m_pVolReader->MaxComponentLength();

    return S_OK;
}

HRESULT MFTWalker::UpdateAttributeList(MFTRecord* pRecord)
{
    HRESULT hr = E_FAIL;
    if (pRecord->m_pAttributeList->IsPresent())
    {
        for (auto& attr : pRecord->m_pAttributeList->m_AttList)
        {
            if (attr.m_Attribute == NULL)
            {
                log::Debug(
                    _L_,
                    L"Record %.16I64X: Incomplete due to null attribute\r\n",
                    NtfsFullSegmentNumber(&pRecord->GetFileReferenceNumber()));

                if (attr.m_pListEntry)
                {
                    auto it = m_MFTMap.find(NtfsFullSegmentNumber(&attr.m_pListEntry->SegmentReference));

                    if (it != end(m_MFTMap) && it->second)
                    {
                        for (const auto& other : it->second->GetAttributeList())
                        {
                            if (other.TypeCode() == attr.m_pListEntry->AttributeTypeCode
                                && other.LowestVCN() == attr.m_pListEntry->LowestVcn
                                && other.AttributeNameLength() == attr.m_pListEntry->AttributeNameLength
                                && !wcsncmp(
                                    other.AttributeName(),
                                    attr.m_pListEntry->AttributeName,
                                    attr.m_pListEntry->AttributeNameLength))
                            {
                                attr.m_Attribute = other.m_Attribute;
                                break;
                            }
                        }
                        if (attr.m_Attribute == nullptr)
                        {
                            log::Debug(
                                _L_,
                                L"Record %.16I64X: attribute remains unknwon due to missing attribute even if record "
                                L"is loaded....\r\n",
                                NtfsFullSegmentNumber(&pRecord->GetFileReferenceNumber()));
                        }
                    }
                    else
                    {
                        log::Debug(
                            _L_,
                            L"Record %.16I64X: attribute remains unknwon due to missing record %.16I64X\r\n",
                            NtfsFullSegmentNumber(&pRecord->GetFileReferenceNumber()),
                            NtfsFullSegmentNumber(&attr.m_pListEntry->SegmentReference));
                    }
                }
            }
            if (attr.m_Attribute != nullptr)
            {
                if (attr.m_Attribute->TypeCode() == $INDEX_ROOT && attr.m_Attribute->NameEquals(L"$I30"))
                {
                    if (!pRecord->m_bIsDirectory)
                    {
                        pRecord->m_bIsDirectory = true;
                        if (FAILED(hr = AddDirectoryName(pRecord)))
                        {
                            log::Warning(_L_, hr, L"Failed to update directory map\r\n");
                        }
                    }
                }
            }
        }
    }
    if (pRecord->IsBaseRecord() && pRecord->IsDirectory())
    {
        auto it = m_DirectoryNames.find(NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber));
        if (it == end(m_DirectoryNames))
        {
            if (FAILED(hr = AddDirectoryName(pRecord)))
            {
                log::Warning(_L_, hr, L"Failed to update directory map\r\n");
            }
        }
    }

    return S_OK;
}

bool MFTWalker::IsInLocation(PFILE_NAME pFileName)
{
    if (m_Locations.empty())
        return true;

    boost::logic::tribool bInLocation = boost::indeterminate;

    MFTUtils::SafeMFTSegmentNumber ulLastSegmentNumber = NtfsFullSegmentNumber(&(pFileName->ParentDirectory));
    auto it = m_DirectoryNames.find(ulLastSegmentNumber);

    if (it == end(m_DirectoryNames))
    {
        // parent directory not found :'( we return not in location
        return false;
    }
    else
    {
        if (it->second.m_InLocation)
        {
            // Direct parent is in location, return true!
            return true;
        }
        else if (!it->second.m_InLocation)
        {
            // if direct parent is determined and false then, Record is not in location!
            return false;
        }
        else
        {
            // direct parent is indeterminate... need to determinate!
            bool bNameInLocation = false;
            GetFullNameAndIfInLocation(pFileName, nullptr, nullptr, &bNameInLocation);
            it->second.m_InLocation = bNameInLocation;  // result saved for future queries
            return bNameInLocation;
        }
    }
}

const WCHAR* MFTWalker::GetFullNameAndIfInLocation(
    PFILE_NAME pFileName,
    const std::shared_ptr<DataAttribute>& pDataAttr,
    DWORD* pdwLen,
    bool* pbInSpecificLocation)
{
    WCHAR* pCurrent = NULL;

    if (m_Locations.empty() && pbInSpecificLocation != nullptr)
        *pbInSpecificLocation = true;

    if (m_pFullNameBuffer == NULL)
    {
        m_dwFullNameBufferLen = msl::utilities::SafeInt(m_pVolReader->MaxComponentLength()) * 2;
        m_pFullNameBuffer = (WCHAR*)HeapAlloc(GetProcessHeap(), 0L, m_dwFullNameBufferLen);
        if (!m_pFullNameBuffer)
            return NULL;
    }
#ifdef DEBUG
    wmemset(m_pFullNameBuffer, L'_', m_dwFullNameBufferLen / sizeof(WCHAR));
    m_pFullNameBuffer[(m_pVolReader->MaxComponentLength() / sizeof(WCHAR)) - 1] = 0;
#endif

    DWORD dwCount = 0;
    DWORD dwBaseNameCount = 0;

    // Doing trailing \0
    if (dwCount > m_dwFullNameBufferLen)
    {
        if (FAILED(ExtendNameBuffer(&pCurrent)))
            return NULL;
    }
    _ASSERT(dwCount < m_dwFullNameBufferLen);
    {
        dwCount += sizeof(WCHAR);

        if (dwCount > m_dwFullNameBufferLen)
        {
            if (FAILED(ExtendNameBuffer(&pCurrent)))
                return NULL;
        }
        _ASSERT(dwCount <= m_dwFullNameBufferLen);

        pCurrent = m_pFullNameBuffer + (m_dwFullNameBufferLen / sizeof(WCHAR)) - 1;
        _ASSERT(pCurrent >= m_pFullNameBuffer);
        *pCurrent = 0;
    }

    // Doing Stream name
    if (pDataAttr != NULL)
    {
        PATTRIBUTE_RECORD_HEADER pHeader = pDataAttr->Header();

        if (pHeader->NameLength)
        {
            // Stream has a name, we have to add it
            {
                dwCount += pHeader->NameLength * sizeof(WCHAR);
                dwBaseNameCount += pHeader->NameLength;

                if (dwCount > m_dwFullNameBufferLen)
                {
                    if (FAILED(ExtendNameBuffer(&pCurrent)))
                        return NULL;
                }
                _ASSERT(dwCount <= m_dwFullNameBufferLen);

                pCurrent -= pHeader->NameLength;
                _ASSERT(pCurrent >= m_pFullNameBuffer);

                memcpy_s(
                    pCurrent, dwCount, ((BYTE*)pHeader) + pHeader->NameOffset, pHeader->NameLength * sizeof(WCHAR));
            }

            // adding colon
            {
                dwCount += 1 * sizeof(WCHAR);
                dwBaseNameCount += 1;

                if (dwCount > m_dwFullNameBufferLen)
                {
                    if (FAILED(ExtendNameBuffer(&pCurrent)))
                        return NULL;
                }
                _ASSERT(dwCount <= m_dwFullNameBufferLen);

                pCurrent -= 1;
                _ASSERT(pCurrent >= m_pFullNameBuffer);

                *pCurrent = L':';
            }
        }
    }

    // Doing base file name
    PFILE_NAME pCurFileName = pFileName;
    if (pCurFileName != nullptr)
    {
        dwCount += pCurFileName->FileNameLength * sizeof(WCHAR);
        dwBaseNameCount += pCurFileName->FileNameLength;

        if (dwCount > m_dwFullNameBufferLen)
        {
            if (FAILED(ExtendNameBuffer(&pCurrent)))
                return NULL;
        }
        _ASSERT(dwCount <= m_dwFullNameBufferLen);

        pCurrent -= pCurFileName->FileNameLength;
        _ASSERT(pCurrent >= m_pFullNameBuffer);
        memcpy_s(pCurrent, dwCount, pCurFileName->FileName, pCurFileName->FileNameLength * sizeof(WCHAR));
    }
    else
    {
        const WCHAR szNullFileName[] = L"<NoName>";
        const DWORD dwNullFileName = (DWORD)wcslen(szNullFileName);

        dwCount += dwNullFileName * sizeof(WCHAR);
        dwBaseNameCount += dwNullFileName;

        if (dwCount > m_dwFullNameBufferLen)
        {
            if (FAILED(ExtendNameBuffer(&pCurrent)))
                return NULL;
        }
        _ASSERT(dwCount <= m_dwFullNameBufferLen);

        pCurrent -= dwNullFileName;
        _ASSERT(pCurrent >= m_pFullNameBuffer);
        memcpy_s(pCurrent, dwCount, szNullFileName, dwNullFileName * sizeof(WCHAR));

        // Entries with lost parents are
        if (pbInSpecificLocation != nullptr)
            *pbInSpecificLocation = m_Locations.empty() ? true : false;

        if (pdwLen)
            *pdwLen = dwCount;
        return pCurrent;
    }

    MFTUtils::SafeMFTSegmentNumber ulLastSegmentNumber = NtfsFullSegmentNumber(&(pCurFileName->ParentDirectory));

    auto pParentPair = m_DirectoryNames.find(NtfsFullSegmentNumber(&(pCurFileName->ParentDirectory)));
    auto pDirectParent = pParentPair;

    while (pParentPair != end(m_DirectoryNames))
    {
        PFILE_NAME pParentName = pParentPair->second.FileName();

        if (pParentName == NULL)
        {
            log::Verbose(
                (_L_),
                L"Could not determine main parent file name for %.*s\r\n",
                pCurFileName->FileNameLength,
                pCurFileName->FileName);
            break;
        }

        if (!(pParentName->FileNameLength == 1 && *pParentName->FileName == L'.'))
        {
            // Adding \\ (i.e. one backslash)
            {
                dwCount += sizeof(WCHAR);
                if (dwCount > m_dwFullNameBufferLen)
                {
                    if (FAILED(ExtendNameBuffer(&pCurrent)))
                        return NULL;
                }
                _ASSERT(dwCount <= m_dwFullNameBufferLen);

                pCurrent--;
                _ASSERT(pCurrent >= m_pFullNameBuffer);

                *pCurrent = L'\\';
            }

            // Adding parent file name
            {
                dwCount += pParentName->FileNameLength * sizeof(WCHAR);
                if (dwCount > m_dwFullNameBufferLen)
                {
                    if (FAILED(ExtendNameBuffer(&pCurrent)))
                        return NULL;
                }
                _ASSERT(dwCount <= m_dwFullNameBufferLen);

                pCurrent -= pParentName->FileNameLength;
                _ASSERT(pCurrent >= m_pFullNameBuffer);

                memcpy_s(pCurrent, dwCount, pParentName->FileName, pParentName->FileNameLength * sizeof(WCHAR));
            }
        }
        ulLastSegmentNumber = NtfsFullSegmentNumber(&(pParentName->ParentDirectory));
        pParentPair = m_DirectoryNames.find(NtfsFullSegmentNumber(&(pParentName->ParentDirectory)));

        if (ulLastSegmentNumber == m_pMFT->GetUSNRoot())
            break;
    }

    if (ulLastSegmentNumber == m_pMFT->GetUSNRoot())
    {
        // Adding \\ (i.e. one backslash)
        {
            dwCount += sizeof(WCHAR);
            if (dwCount > m_dwFullNameBufferLen)
            {
                if (FAILED(ExtendNameBuffer(&pCurrent)))
                    return NULL;
            }
            _ASSERT(dwCount <= m_dwFullNameBufferLen);

            pCurrent--;
            _ASSERT(pCurrent >= m_pFullNameBuffer);

            *pCurrent = L'\\';
        }

        if (pDirectParent != end(m_DirectoryNames))
        {
            // Looking for presence in specific locations
            if (!m_Locations.empty() && boost::logic::indeterminate(pDirectParent->second.m_InLocation))
            {
                pDirectParent->second.m_InLocation = std::any_of(
                    begin(m_Locations), end(m_Locations), [pCurrent, pbInSpecificLocation](const wstring& item) {
                        return !_wcsnicmp(pCurrent, item.c_str(), item.size());
                    });
            }
            if (pbInSpecificLocation != nullptr)
            {
                if (m_Locations.empty())
                    *pbInSpecificLocation = true;
                else
                {
                    if (pDirectParent->second.m_InLocation)
                        *pbInSpecificLocation = true;
                    else if (!pDirectParent->second.m_InLocation)
                        *pbInSpecificLocation = false;
                    else
                    {
                        log::Error(_L_, E_FAIL, L"Failed to determine if in location for path %s\r\n", pCurrent);
                        *pbInSpecificLocation = false;
                    }
                }
            }
        }

        // And we're done :-)
        if (pdwLen)
            *pdwLen = dwCount;
        return pCurrent;
    }
    else
    {
        // Parent folder was _not_ found, inserting "place holder"
        // Adding \\ (i.e. one backslash)

        {
            dwCount += sizeof(WCHAR);
            if (dwCount > m_dwFullNameBufferLen)
            {
                if (FAILED(ExtendNameBuffer(&pCurrent)))
                    return NULL;
            }
            _ASSERT(dwCount <= m_dwFullNameBufferLen);

            pCurrent--;
            _ASSERT(pCurrent >= m_pFullNameBuffer);
            *pCurrent = L'\\';
        }

        {
            dwCount += sizeof(WCHAR) * 20;
            if (dwCount > m_dwFullNameBufferLen)
            {
                if (FAILED(ExtendNameBuffer(&pCurrent)))
                    return NULL;
            }
            _ASSERT(dwCount <= m_dwFullNameBufferLen);

            pCurrent -= 20;
            swprintf_s(pCurrent, 21, L"__%.16I64X__", ulLastSegmentNumber);
            pCurrent[20] = L'\\';
        }

        {
            dwCount += sizeof(WCHAR);
            if (dwCount > m_dwFullNameBufferLen)
            {
                if (FAILED(ExtendNameBuffer(&pCurrent)))
                    return NULL;
            }
            _ASSERT(dwCount <= m_dwFullNameBufferLen);

            pCurrent--;
            _ASSERT(pCurrent >= m_pFullNameBuffer);
            *pCurrent = L'\\';
        }

        if (pdwLen)
            *pdwLen = dwCount;
        return pCurrent;
    }
}

bool MFTWalker::AreAttributesComplete(const MFTRecord* pBaseRecord, std::vector<MFT_SEGMENT_REFERENCE>& missingRecords)
    const
{
    bool retval = true;

    if (pBaseRecord->m_pAttributeList->IsPresent())
    {
        for (const auto& attr : pBaseRecord->m_pAttributeList->m_AttList)
        {
            if (attr.m_Attribute == NULL)
            {
                log::Debug(
                    _L_,
                    L"Record %.16I64X: Incomplete due to null attribute\r\n",
                    NtfsFullSegmentNumber(&pBaseRecord->GetFileReferenceNumber()));
                if (attr.m_pListEntry != nullptr)
                {

                    auto pAttrRecord = m_MFTMap.find(NtfsFullSegmentNumber(&attr.m_pListEntry->SegmentReference));
                    if (pAttrRecord == end(m_MFTMap))
                    {
                        missingRecords.push_back(attr.m_pListEntry->SegmentReference);
                    }
                    retval = false;
                }
            }
            else if (attr.m_Attribute->m_pHostRecord == NULL)
            {
                log::Debug(
                    _L_,
                    L"Record %.16I64X: Incomplete due to missing host record for attribute\r\n",
                    NtfsFullSegmentNumber(&pBaseRecord->GetFileReferenceNumber()));
                retval = false;
            }
            else
            {
                if (!attr.m_Attribute->m_pHostRecord->IsParsed())
                {
                    missingRecords.push_back(attr.m_Attribute->m_pHostRecord->GetFileReferenceNumber());
                    log::Debug(
                        _L_,
                        L"Record %.16I64X: Incomplete due to unavailable, parsed host record (%.16I64X) for "
                        L"attribute\r\n",
                        NtfsFullSegmentNumber(&pBaseRecord->GetFileReferenceNumber()),
                        NtfsFullSegmentNumber(&attr.m_Attribute->m_pHostRecord->GetFileReferenceNumber()));
                    retval = false;
                }
            }
        }
    }
    return retval;
}

bool MFTWalker::IsRecordComplete(
    MFTRecord* pRecord,
    std::vector<MFT_SEGMENT_REFERENCE>& missingRecords,
    bool bAndAttributesComplete,
    bool bAndAllParents) const
{
    if (pRecord->m_bIsComplete)
        return true;

    bool bIsComplete = true;

    for (const auto& child : pRecord->GetChildRecords())
    {
        const auto& pChild = m_MFTMap.find(child.first);

        if (pChild == end(m_MFTMap))
        {
            MFT_SEGMENT_REFERENCE childFRN = *((MFT_SEGMENT_REFERENCE*)&child.first);
            missingRecords.push_back(childFRN);
            bIsComplete = false;
        }
    }

    const MFTRecord* pBaseRecord = (pRecord->m_pBaseFileRecord == NULL) ? pRecord : pRecord->m_pBaseFileRecord;
    if (0 != NtfsSegmentNumber(&(pBaseRecord->m_pRecord->BaseFileRecordSegment))
        && pBaseRecord->m_pBaseFileRecord == NULL)
    {
        log::Debug(
            _L_,
            L"Record %.16I64X: Incomplete due to missing base record %.16I64X\r\n",
            NtfsFullSegmentNumber(&pRecord->GetFileReferenceNumber()),
            NtfsFullSegmentNumber(&(pBaseRecord->m_pRecord->BaseFileRecordSegment)));

        const auto& pBase = m_MFTMap.find(NtfsFullSegmentNumber(&pBaseRecord->m_pRecord->BaseFileRecordSegment));

        if (pBase == end(m_MFTMap))
        {
            missingRecords.push_back(pBaseRecord->m_pRecord->BaseFileRecordSegment);
        }
        bIsComplete = false;
    }

    if (bAndAttributesComplete && !AreAttributesComplete(pBaseRecord, missingRecords))
    {
        bIsComplete = false;
    }

    if (bAndAllParents)
    {
        for (auto nameiter = pBaseRecord->m_FileNames.cbegin(); nameiter != pBaseRecord->m_FileNames.cend(); ++nameiter)
        {
            const PFILE_NAME pFileName = *nameiter;

            if (NtfsFullSegmentNumber(&(pFileName->ParentDirectory)) == m_pMFT->GetUSNRoot())
            {
                break;
            }

            const auto& pParentPair = m_DirectoryNames.find(NtfsFullSegmentNumber(&(pFileName->ParentDirectory)));

            if (pParentPair == end(m_DirectoryNames))
            {
                log::Debug(
                    _L_,
                    L"Record %.16I64X: Incomplete due to missing file name parent record %.16I64X\r\n",
                    NtfsFullSegmentNumber(&pRecord->GetFileReferenceNumber()),
                    pFileName->ParentDirectory);

                auto pParent = m_MFTMap.find(NtfsFullSegmentNumber(&(pFileName->ParentDirectory)));
                if (pParent == end(m_MFTMap))
                {
                    missingRecords.push_back(pFileName->ParentDirectory);
                }
                bIsComplete = false;
                break;
            }

            auto pParentName = pParentPair->second.FileName();

            while (pParentName != nullptr)
            {
                MFTUtils::UnSafeMFTSegmentNumber UnSafeSegmentNumber =
                    NtfsSegmentNumber(&(pParentName->ParentDirectory));
                ULONGLONG SafeSegmentNumber = NtfsFullSegmentNumber(&(pParentName->ParentDirectory));
                if (SafeSegmentNumber == m_pMFT->GetUSNRoot() || UnSafeSegmentNumber == 0)
                    break;

                const auto& pOtherParentPair =
                    m_DirectoryNames.find(NtfsFullSegmentNumber(&(pParentName->ParentDirectory)));
                if (pOtherParentPair == end(m_DirectoryNames))
                {
                    log::Debug(
                        _L_,
                        L"Record %.16I64X: Incomplete due to missing file name parent record %.16I64X\r\n",
                        NtfsFullSegmentNumber(&pRecord->GetFileReferenceNumber()),
                        pParentName->ParentDirectory);

                    auto pParent = m_MFTMap.find(NtfsFullSegmentNumber(&(pParentName->ParentDirectory)));
                    if (pParent == end(m_MFTMap))
                    {
                        missingRecords.push_back(pParentName->ParentDirectory);
                    }
                    bIsComplete = false;
                    pParentName = nullptr;
                }
                else
                {
                    pParentName = pOtherParentPair->second.FileName();
                }
            }
        }
    }

    const_cast<MFTRecord*>(pRecord)->m_bIsComplete = bIsComplete;
    return bIsComplete;
}

HRESULT MFTWalker::SetCallbacks(const Callbacks& pCallbacks)
{
    m_pCallbackCall = [](MFTWalker* pThis, MFTRecord* pRecord, bool& bFreeRecord) {
        return pThis->SimpleCallCallbackForRecord(pRecord, bFreeRecord);
    };

    m_Callbacks = pCallbacks;

    if (m_Callbacks.ElementCallback == nullptr)
    {
        m_Callbacks.ElementCallback = [](const std::shared_ptr<VolumeReader>&, MFTRecord*) {};
    }

    if (m_Callbacks.AttributeCallback != nullptr || m_Callbacks.DataCallback != nullptr
        || m_Callbacks.FileNameAndDataCallback != nullptr || m_Callbacks.DirectoryCallback != nullptr
        || m_Callbacks.FileNameCallback != nullptr)
    {
        m_pCallbackCall = [](MFTWalker* pThis, MFTRecord* pRecord, bool& bFreeRecord) {
            return pThis->FullCallCallbackForRecord(pRecord, bFreeRecord);
        };
    }

    if (m_Callbacks.KeepAliveCallback == nullptr)
    {
        m_Callbacks.KeepAliveCallback = [](const std::shared_ptr<VolumeReader>&, MFTRecord*) -> bool { return false; };
    }

    if (m_Callbacks.ProgressCallback == nullptr)
    {
        m_Callbacks.ProgressCallback = [](const ULONG) -> HRESULT { return S_OK; };
    }

    return S_OK;
}

HRESULT MFTWalker::ParseI30AndCallback(MFTRecord* pRecord)
{
    HRESULT hr = E_FAIL;

    if (pRecord == nullptr)
        return E_POINTER;
    if (!pRecord->IsDirectory())
        return S_OK;

    std::shared_ptr<IndexAllocationAttribute> pIA;
    std::shared_ptr<IndexRootAttribute> pIR;
    std::shared_ptr<BitmapAttribute> pBM;

    if (FAILED(hr = pRecord->GetIndexAttributes(m_pVolReader, L"$I30", pIR, pIA, pBM)))
    {
        log::Error(_L_, hr, L"Failed to find $I30 attributes\r\n");
        return hr;
    }

    if (pIR != nullptr)
    {
        PINDEX_ENTRY entry = pIR->FirstIndexEntry();

        while (!(entry->Flags & INDEX_ENTRY_END))
        {
            PFILE_NAME pFileName = (PFILE_NAME)((PBYTE)entry + sizeof(INDEX_ENTRY));

            if (m_Callbacks.I30Callback)
                m_Callbacks.I30Callback(m_pVolReader, pRecord, entry, pFileName, false);

            entry = NtfsNextIndexEntry(entry);
        }
    }

    if (pIA != nullptr && m_Callbacks.I30Callback != nullptr)
    {
        ULONGLONG ToRead = 0ULL;
        if (FAILED(hr = pIA->DataSize(m_pVolReader, ToRead)))
        {
            log::Error(_L_, hr, L"Failed to determine $INDEX_ALLOCATION size\r\n");
            return hr;
        }

        UINT i = 0L;

        if (FAILED(
                hr = pRecord->EnumData(
                    m_pVolReader,
                    pIA,
                    0ULL,
                    ToRead,
                    pIR->SizePerIndex(),
                    [this, pBM, pIR, &hr, pRecord, &i](ULONGLONG ullBufferStartOffset, CBinaryBuffer& Data) -> HRESULT {
                        DBG_UNREFERENCED_PARAMETER(ullBufferStartOffset);
                        PINDEX_ALLOCATION_BUFFER pIABuff = (PINDEX_ALLOCATION_BUFFER)Data.GetData();

                        if ((*pBM)[i])
                        {
                            if (FAILED(hr = MFTUtils::MultiSectorFixup(pIABuff, pIR->SizePerIndex(), m_pVolReader)))
                            {
                                if (HRESULT_FROM_NT(NTE_BAD_SIGNATURE) != hr)
                                {
                                    log::Error(_L_, hr, L"Failed to fixup $INDEX_ALLOCATION header\r\n");
                                    return hr;
                                }
                            }
                            else
                            {
                                PINDEX_HEADER pHeader = &(pIABuff->IndexHeader);
                                PINDEX_ENTRY pEntry = (PINDEX_ENTRY)NtfsFirstIndexEntry(pHeader);
                                while (!(pEntry->Flags & INDEX_ENTRY_END))
                                {
                                    PFILE_NAME pFileName = (PFILE_NAME)((PBYTE)pEntry + sizeof(INDEX_ENTRY));

                                    m_Callbacks.I30Callback(m_pVolReader, pRecord, pEntry, pFileName, false);

                                    pEntry = NtfsNextIndexEntry(pEntry);
                                }

                                LPBYTE pFirstFreeByte =
                                    (((LPBYTE)NtfsFirstIndexEntry(pHeader)) + pHeader->FirstFreeByte);

                                while (pFirstFreeByte + sizeof(FILE_NAME) < Data.GetData() + Data.GetCount())
                                {
                                    PFILE_NAME pCarvedFileName = (PFILE_NAME)pFirstFreeByte;

                                    if (NtfsFullSegmentNumber(&pCarvedFileName->ParentDirectory)
                                        == pRecord->GetSafeMFTSegmentNumber())
                                    {
                                        PINDEX_ENTRY pEntry =
                                            (PINDEX_ENTRY)((LPBYTE)pCarvedFileName - sizeof(INDEX_ENTRY));
                                        m_Callbacks.I30Callback(m_pVolReader, pRecord, pEntry, pCarvedFileName, true);
                                    }
                                    pFirstFreeByte++;
                                }
                            }
                        }
                        else
                        {
                            log::Verbose(
                                _L_,
                                L"Index %d of $INDEX_ALLOCATION is not in use (FRN=0x%.16I64X) only carving...\r\n",
                                i,
                                NtfsFullSegmentNumber(&pRecord->GetFileReferenceNumber()));

                            if (FAILED(hr = MFTUtils::MultiSectorFixup(pIABuff, pIR->SizePerIndex(), m_pVolReader)))
                            {
                                log::Verbose(_L_, L"Failed to fixup $INDEX_ALLOCATION (carved)\r\n");
                                return S_OK;
                            }
                            else
                            {
                                LPBYTE pFirstFreeByte = (LPBYTE)pIABuff;

                                while (pFirstFreeByte + sizeof(FILE_NAME) < Data.GetData() + Data.GetCount())
                                {
                                    PFILE_NAME pCarvedFileName = (PFILE_NAME)pFirstFreeByte;

                                    if (NtfsFullSegmentNumber(&pCarvedFileName->ParentDirectory)
                                        == pRecord->GetSafeMFTSegmentNumber())
                                    {
                                        PINDEX_ENTRY pEntry =
                                            (PINDEX_ENTRY)((LPBYTE)pCarvedFileName - sizeof(INDEX_ENTRY));
                                        m_Callbacks.I30Callback(m_pVolReader, pRecord, pEntry, pCarvedFileName, true);
                                    }
                                    pFirstFreeByte++;
                                }
                            }
                        }
                        i++;
                        return S_OK;
                    })))
        {
            log::Error(_L_, hr, L"Failed to read from $INDEX_ALLOCATION\r\n");
            return hr;
        }
    }
    return S_OK;
}

HRESULT MFTWalker::Parse$SecureAndCallback(MFTRecord* pRecord)
{
    HRESULT hr = E_FAIL;

    std::shared_ptr<DataAttribute> pSDSAttr;
    pSDSAttr = pRecord->GetDataAttribute(L"$SDS");
    if (pSDSAttr == nullptr)
    {
        log::Error(
            _L_,
            hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
            L"Failed to find $SDS data stream, nothing to parse...\r\n");
        return hr;
    }

    auto pSDSStream = pSDSAttr->GetDataStream(_L_, m_pVolReader);
    if (!pSDSStream)
    {
        log::Error(
            _L_,
            hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
            L"Failed to get $SDS data stream, nothing to parse...\r\n");
        return hr;
    }
    auto pMemStream = std::make_shared<MemoryStream>(_L_);

    if (FAILED(hr = pMemStream->OpenForReadWrite(static_cast<DWORD>(pSDSStream->GetSize()))))
    {
        log::Error(
            _L_,
            hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
            L"Failed to open mem stream to store $SDS data stream\r\n");
        return hr;
    }

    ULONGLONG ullCopied = 0LL;
    if (FAILED(hr = pSDSStream->CopyTo(pMemStream, &ullCopied)))
    {
        log::Error(_L_, hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA), L"Failed to load $SDS data stream\r\n");
        return hr;
    }

    CBinaryBuffer SDS;
    pMemStream->GrabBuffer(SDS);

    pMemStream->Close();
    pMemStream.reset();

    std::shared_ptr<IndexAllocationAttribute> pIA;
    std::shared_ptr<IndexRootAttribute> pIR;
    std::shared_ptr<BitmapAttribute> pBM;
    if (FAILED(hr = pRecord->GetIndexAttributes(m_pVolReader, L"$SII", pIR, pIA, pBM)))
    {
        log::Error(_L_, hr, L"Failed to find $SII attributes\r\n");
        return hr;
    }

    if (pIR != nullptr)
    {
        PINDEX_ENTRY entry = pIR->FirstIndexEntry();

        while (!(entry->Flags & INDEX_ENTRY_END))
        {
            // PBYTE pFileName = (PBYTE) ((PBYTE) entry + sizeof(INDEX_ENTRY));

            entry = NtfsNextIndexEntry(entry);
        }
    }
    if (pIA != nullptr)
    {
        ULONGLONG ToRead = 0ULL;
        if (FAILED(hr = pIA->DataSize(m_pVolReader, ToRead)))
        {
            log::Error(_L_, hr, L"Failed to determine $INDEX_ALLOCATION size\r\n");
            return hr;
        }

        UINT i = 0L;

        if (FAILED(
                hr = pRecord->EnumData(
                    m_pVolReader,
                    pIA,
                    0ULL,
                    ToRead,
                    pIR->SizePerIndex(),
                    [this, pBM, pIR, &hr, pRecord, &i, &SDS](
                        ULONGLONG ullBufferStartOffset, CBinaryBuffer& Data) -> HRESULT {
                        DBG_UNREFERENCED_PARAMETER(ullBufferStartOffset);
                        PINDEX_ALLOCATION_BUFFER pIABuff = (PINDEX_ALLOCATION_BUFFER)Data.GetData();

                        if ((*pBM)[i])
                        {
                            if (FAILED(hr = MFTUtils::MultiSectorFixup(pIABuff, pIR->SizePerIndex(), m_pVolReader)))
                            {
                                log::Error(_L_, hr, L"Failed to fixup $INDEX_ALLOCATION header\r\n");
                                return hr;
                            }
                            else
                            {
                                PINDEX_HEADER pHeader = &(pIABuff->IndexHeader);
                                PSECURITY_DESCRIPTOR_INDEX_ENTRY pEntry = NtfsFirstSecDescIndexEntry(pHeader);

                                while (!(pEntry->Flags & INDEX_ENTRY_END))
                                {
                                    PSECURITY_DESCRIPTOR_ENTRY pSDSEntry =
                                        (PSECURITY_DESCRIPTOR_ENTRY)&SDS.Get<BYTE>(pEntry->SecurityDescriptorOffset);

                                    if (m_Callbacks.SecDescCallback != nullptr)
                                        m_Callbacks.SecDescCallback(m_pVolReader, pSDSEntry);

                                    pEntry = NtfsNextSecDescIndexEntry(pEntry);
                                }
                            }
                        }
                        return S_OK;
                    })))
        {
            log::Error(_L_, hr, L"Failed to read from $INDEX_ALLOCATION\r\n");
            return hr;
        }
    }
    return S_OK;
}

HRESULT MFTWalker::SimpleCallCallbackForRecord(MFTRecord* pRecord, bool& bFreeRecord)
{
    if (NtfsSegmentNumber(&pRecord->m_pRecord->BaseFileRecordSegment) > 0)
        return S_OK;  // we don't call the callbacks on child records...

    HRESULT hr = S_OK;

    if (!pRecord->HasCallbackBeenCalled())
    {
        m_dwWalkedItems++;

        m_Callbacks.ElementCallback(m_pVolReader, pRecord);

        if (m_Callbacks.I30Callback && pRecord->IsDirectory())
        {
            HRESULT hr = E_FAIL;
            if (FAILED(hr = ParseI30AndCallback(pRecord)))
            {
                log::Error(
                    _L_,
                    hr,
                    L"Failed to parse $I30 for record 0x%.16I64X\r\n",
                    NtfsFullSegmentNumber(&pRecord->GetFileReferenceNumber()));
            }
        }

        bFreeRecord = !m_Callbacks.KeepAliveCallback(m_pVolReader, pRecord);

        hr = m_Callbacks.ProgressCallback((DWORD)((m_dwWalkedItems * 100) / m_ulMFTRecordCount));

        pRecord->CallbackCalled();
    }

    pRecord->CleanCachedData();
    return hr;
}

HRESULT MFTWalker::FullCallCallbackForRecord(MFTRecord* pRecord, bool& bFreeRecord)
{
    if (NtfsSegmentNumber(&pRecord->m_pRecord->BaseFileRecordSegment) > 0)
        return S_OK;  // we don't call the callbacks on child records...

    HRESULT hr = S_OK;

    if (!pRecord->HasCallbackBeenCalled())
    {
        m_dwWalkedItems++;

        if (m_Callbacks.ElementCallback)
            m_Callbacks.ElementCallback(m_pVolReader, pRecord);

        if (m_Callbacks.AttributeCallback)
        {
            for (const auto& iter : pRecord->GetAttributeList())
            {
                if (m_Callbacks.AttributeCallback)
                    m_Callbacks.AttributeCallback(m_pVolReader, pRecord, iter);
            }
        }

        if (pRecord->m_FileNames.empty() && m_Callbacks.DataCallback)
        {
            if (!pRecord->GetDataAttributes().empty())
            {
                // This record has Data attributes (but no $FILENAME).
                for (const auto& iter2 : pRecord->m_DataAttrList)
                {
                    shared_ptr<DataAttribute> pDataAttr = iter2;
                    bool bInSpecificLocation = false;
                    const WCHAR* pFullName = GetFullNameAndIfInLocation(NULL, pDataAttr, NULL, &bInSpecificLocation);
                    if (pFullName && bInSpecificLocation)
                    {
                        m_Callbacks.DataCallback(m_pVolReader, pRecord, pDataAttr);
                    }
                }
            }
        }
        else if (
            m_Callbacks.FileNameCallback || m_Callbacks.DirectoryCallback || m_Callbacks.FileNameAndDataCallback
            || m_Callbacks.DataCallback)
        {
            // "standard" case
            for (const auto& name : pRecord->m_FileNames)
            {
                bool bInSpecificLocation = false;
                const WCHAR* pFullName = GetFullNameAndIfInLocation(name, NULL, NULL, &bInSpecificLocation);

                if (m_Callbacks.FileNameCallback && bInSpecificLocation)
                    m_Callbacks.FileNameCallback(m_pVolReader, pRecord, name);

                if (bInSpecificLocation
                    && (m_Callbacks.FileNameAndDataCallback || m_Callbacks.DirectoryCallback || m_Callbacks.I30Callback)
                    && pRecord->IsDirectory())
                {
                    if (m_Callbacks.DirectoryCallback)
                        m_Callbacks.DirectoryCallback(
                            m_pVolReader, pRecord, name, pRecord->GetIndexAllocationAttribute(L"$I30"));

                    // This record is a directory and has Data attributes (i.e. ADS).
                    if (m_Callbacks.FileNameAndDataCallback)
                    {
                        for (const auto& data : pRecord->m_DataAttrList)
                        {
                            if (pFullName && m_Callbacks.FileNameAndDataCallback)
                                m_Callbacks.FileNameAndDataCallback(m_pVolReader, pRecord, name, data);
                        }
                    }

                    if (m_Callbacks.I30Callback)
                    {
                        HRESULT hr = E_FAIL;
                        if (FAILED(hr = ParseI30AndCallback(pRecord)))
                        {
                            log::Error(
                                _L_,
                                hr,
                                L"Failed to parse $I30 for record 0x%.16I64X\r\n",
                                NtfsFullSegmentNumber(&pRecord->GetFileReferenceNumber()));
                        }
                    }
                }
                else if (bInSpecificLocation && pRecord->m_DataAttrList.size() && m_Callbacks.FileNameAndDataCallback)
                {
                    // This record has Data attributes.
                    for (const auto& data : pRecord->m_DataAttrList)
                    {
                        if (pFullName && m_Callbacks.FileNameAndDataCallback)
                            m_Callbacks.FileNameAndDataCallback(m_pVolReader, pRecord, name, data);
                    }
                }
            }
            if (pRecord->m_DataAttrList.size() && m_Callbacks.DataCallback)
            {
                // This record has Data attributes.
                for (auto data_iter = pRecord->m_DataAttrList.begin(); data_iter != pRecord->m_DataAttrList.end();
                     ++data_iter)
                {
                    if (m_Callbacks.DataCallback)
                        m_Callbacks.DataCallback(m_pVolReader, pRecord, *data_iter);
                }
            }
        }

        bFreeRecord = !m_Callbacks.KeepAliveCallback(m_pVolReader, pRecord);

        hr = m_Callbacks.ProgressCallback((DWORD)((m_dwWalkedItems * 100) / m_ulMFTRecordCount));

        pRecord->CallbackCalled();
    }

    pRecord->CleanCachedData();

    return hr;
}

HRESULT MFTWalker::WalkRecords(bool bIsFinalWalk)
{
    HRESULT hr = E_FAIL;

    log::Verbose((_L_), L"Loading MFT done, now walking what is left in the map\r\n");

    for (auto iter = begin(m_MFTMap); iter != end(m_MFTMap); ++iter)
    {
        MFTUtils::SafeMFTSegmentNumber RefNumber = iter->first;
        MFTRecord* pRecord = iter->second;

        if (pRecord == nullptr)
        {
            log::Debug(_L_, L"Record %.16I64X's entry is null, skipped\r\n", RefNumber);
            continue;
        }
        if (pRecord->m_pRecord == nullptr)
        {
            log::Debug(_L_, L"Record %.16I64X's entry has a null pRecord, skipped\r\n", RefNumber);
            continue;
        }

        // skipping non base records
        if (NtfsSegmentNumber(&pRecord->m_pRecord->BaseFileRecordSegment) > 0)
        {
            log::Debug(_L_, L"Record %.16I64X is a child record, skipped\r\n", RefNumber);
            continue;
        }

        if (!pRecord->IsParsed())
        {
            log::Debug(_L_, L"Record %.16I64X is not parsed, parsing\r\n", RefNumber);

            MFTRecord* pBaseRecord = nullptr;

            if (pRecord->m_pBaseFileRecord == nullptr
                && 0 != NtfsFullSegmentNumber(&(pRecord->m_pRecord->BaseFileRecordSegment)))
            {
                auto iter = m_MFTMap.find(NtfsFullSegmentNumber(&(pRecord->m_pRecord->BaseFileRecordSegment)));

                if (iter != end(m_MFTMap))
                    pBaseRecord = iter->second;
            }

            if (FAILED(
                    hr = pRecord->ParseRecord(
                        _L_, m_pVolReader, pRecord->m_pRecord, m_pVolReader->GetBytesPerFRS(), pBaseRecord)))
            {
                log::Error(_L_, hr, L"Failed to parse record even if every record is now loaded...\r\n");
            }
            if (pRecord->IsParsed())
            {
                log::Debug(_L_, L"Record %.16I64X is now parsed\r\n", RefNumber);
            }
        }

        bool bFreeRecord = false;
        std::vector<MFT_SEGMENT_REFERENCE> missingRecords;

        if (!IsRecordComplete(pRecord, missingRecords, bIsFinalWalk ? false : true, bIsFinalWalk ? false : true))
        {
            log::Debug(_L_, L"Record %.16I64X is still incomplete, skipped\r\n", RefNumber);
            bFreeRecord = false;
        }
        else
        {
            log::Debug(_L_, L"Calling callback for record %.16I64X\r\n", RefNumber);

            if (m_Callbacks.SecDescCallback != nullptr
                && NtfsFullSegmentNumber(&pRecord->GetFileReferenceNumber()) == $SECURE_FILE_REFERENCE_NUMBER)
            {
                if (FAILED(hr = Parse$SecureAndCallback(pRecord)))
                {
                    log::Debug(_L_, L"Failed to parse $Secure %.16I64X\r\n", RefNumber);
                }
            }
            if (FAILED(hr = m_pCallbackCall(this, pRecord, bFreeRecord)))
            {
                if (hr == HRESULT_FROM_WIN32(ERROR_NO_MORE_FILES))
                {
                    log::Verbose(
                        _L_,
                        L"Callback call is asking to stop walk at record %.16I64X\r\n",
                        NtfsFullSegmentNumber(&pRecord->GetFileReferenceNumber()));
                }
            }
        }

        if (bFreeRecord)
        {
            HRESULT freeHR = E_FAIL;
            log::Debug(_L_, L"Deleting record %.16I64X...\r\n", RefNumber);
            if (FAILED(freeHR = DeleteRecord(pRecord)))
            {
                log::Debug(_L_, L"Record %.16I64X failed deletion (hr=0x%lx)\r\n", RefNumber, freeHR);
            }
            else
            {
                log::Debug(_L_, L"Record %.16I64X deleted\r\n", RefNumber);
            }
        }

        if (hr == HRESULT_FROM_WIN32(ERROR_NO_MORE_FILES))
            return hr;
    }

    return S_OK;
}

HRESULT MFTWalker::DeleteRecord(MFTRecord* pRecord)
{
    _ASSERT(pRecord != nullptr);

    MFTUtils::SafeMFTSegmentNumber ullRecordIndex = NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber);

    for (auto& aPair : pRecord->m_ChildRecords)
    {
        if (aPair.second == nullptr)
        {
            auto item = m_MFTMap.find(aPair.first);
            if (item != end(m_MFTMap))
                aPair.second = item->second;
        }
        if (aPair.second != nullptr && aPair.second != pRecord && aPair.first != ullRecordIndex)
        {
            log::Debug(_L_, L"Deleting record %.16I64X (child of %.16I64X)\r\n", aPair.first, ullRecordIndex);
            aPair.second->~MFTRecord();
            m_SegmentStore.FreeCell(aPair.second);
            m_MFTMap[aPair.first] = nullptr;
        }
    }

    log::Debug(_L_, L"Deleting record %.16I64X\r\n", ullRecordIndex);

    pRecord->~MFTRecord();
    m_SegmentStore.FreeCell(pRecord);
    m_MFTMap[ullRecordIndex] = nullptr;
    return S_OK;
}

HRESULT MFTWalker::AddDirectoryName(MFTRecord* pRecord)
{
    if (pRecord->m_pBaseFileRecord == nullptr && pRecord->IsDirectory())
    {
        // simple case, record is not a child and a directory... let's add it!
        PFILE_NAME pFileName = pRecord->GetMain_PFILE_NAME();
        if (pFileName != NULL)
            m_DirectoryNames.insert(pair<MFTUtils::SafeMFTSegmentNumber, MFTFileNameWrapper>(
                NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber), MFTFileNameWrapper(pFileName)));
        else
        {
            log::Debug(
                _L_,
                L"Record %.16I64X: FAILED to get a name for this directory : none inserted\r\n",
                NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber));
        }
    }
    else if (pRecord->m_pBaseFileRecord != nullptr && pRecord->m_pBaseFileRecord->IsDirectory())
    {
        // we need to check if master record is already in m_DirectoryNames....
        auto iter = m_DirectoryNames.find(NtfsFullSegmentNumber(&pRecord->m_pBaseFileRecord->m_FileReferenceNumber));

        if (iter == end(m_DirectoryNames))
        {
            // it's not... we need to add it!
            PFILE_NAME pFileName = pRecord->m_pBaseFileRecord->GetMain_PFILE_NAME();
            if (pFileName != NULL)
                m_DirectoryNames.insert(pair<MFTUtils::SafeMFTSegmentNumber, MFTFileNameWrapper>(
                    NtfsFullSegmentNumber(&pRecord->m_pBaseFileRecord->m_FileReferenceNumber),
                    MFTFileNameWrapper(pFileName)));
            else
            {
                log::Debug(
                    _L_,
                    L"Record %.16I64X: FAILED to get a name for this directory : none inserted\r\n",
                    NtfsFullSegmentNumber(&pRecord->m_pBaseFileRecord->m_FileReferenceNumber));
            }
        }
    }
    return S_OK;
}

HRESULT
MFTWalker::AddRecord(MFTUtils::SafeMFTSegmentNumber& ullRecordIndex, CBinaryBuffer& Data, MFTRecord*& pAddedRecord)
{
    HRESULT hr = E_FAIL;

    pAddedRecord = nullptr;

    try
    {

        PFILE_RECORD_SEGMENT_HEADER pHeader = (PFILE_RECORD_SEGMENT_HEADER)Data.GetData();

        if ((pHeader->MultiSectorHeader.Signature[0] != 'F') || (pHeader->MultiSectorHeader.Signature[1] != 'I')
            || (pHeader->MultiSectorHeader.Signature[2] != 'L') || (pHeader->MultiSectorHeader.Signature[3] != 'E'))
        {
            log::Verbose(
                _L_,
                L"Skipping... MultiSectorHeader.Signature is not FILE - \"%c%c%c%c\".\r\n",
                pHeader->MultiSectorHeader.Signature[0],
                pHeader->MultiSectorHeader.Signature[1],
                pHeader->MultiSectorHeader.Signature[2],
                pHeader->MultiSectorHeader.Signature[3]);
            return S_OK;
        }

        MFT_SEGMENT_REFERENCE SafeReference;

        if (pHeader->MultiSectorHeader.UpdateSequenceArrayOffset == 0x2A && pHeader->FirstAttributeOffset == 0x30)
        {
            log::Verbose((_L_), L"Weird case of NTFS from 2K upgraded to XP\r\n");
            ULARGE_INTEGER FRN {0};
            FRN.QuadPart = ullRecordIndex;
            SafeReference.SegmentNumberLowPart = FRN.LowPart;
            SafeReference.SegmentNumberHighPart = static_cast<USHORT>(FRN.HighPart);
            SafeReference.SequenceNumber = pHeader->SequenceNumber;
        }
        else
        {
            SafeReference.SegmentNumberHighPart = pHeader->SegmentNumberHighPart;
            SafeReference.SegmentNumberLowPart = pHeader->SegmentNumberLowPart;
            SafeReference.SequenceNumber = pHeader->SequenceNumber;
        }

        MFTUtils::SafeMFTSegmentNumber SafeFRN = NtfsFullSegmentNumber(&SafeReference);

        const auto pIter = m_MFTMap.find(SafeFRN);

        if (pIter != end(m_MFTMap) && pIter->second == nullptr)
        {
            // This record was added, treated and deleted) --> Now SKIP it!
            return S_FALSE;
        }

        MFTRecord* pRecord = nullptr;
        if (pIter == end(m_MFTMap))
        {
            if (m_SegmentStore.AllocatedCells() >= m_CellStoreLastWalk + m_CellStoreThreshold)
            {
                WalkRecords(false);
                m_CellStoreLastWalk = m_SegmentStore.AllocatedCells();
            }

            LPVOID pBuf = m_SegmentStore.GetNewCell();
            if (pBuf == nullptr)
                return E_OUTOFMEMORY;

            pRecord = new (pBuf) MFTRecord;

            if (pRecord == NULL)
            {
                // We walk through FILES for our already recorded nodes with hope this will free some space
                WalkRecords(false);
                pBuf = m_SegmentStore.GetNewCell();
                pRecord = new (pBuf) MFTRecord;
                if (pRecord == NULL)
                {
                    // We are still unable to move forward...
                    return E_OUTOFMEMORY;
                }
            }

            pRecord->m_pRecord = (PFILE_RECORD_SEGMENT_HEADER)(((BYTE*)pRecord) + sizeof(MFTRecord));

            memcpy_s(
                (LPBYTE)pRecord->m_pRecord,
                m_pVolReader->GetBytesPerFRS(),
                Data.GetData(),
                m_pVolReader->GetBytesPerFRS());

            pRecord->m_FileReferenceNumber = SafeReference;
        }
        else
        {
            pRecord = pIter->second;
        }

        log::Debug(
            _L_,
            L"AddRecordCallback: adding record %.16I64X\r\n",
            NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber));

        if (ullRecordIndex != pRecord->m_FileReferenceNumber.SegmentNumberLowPart)
        {
            log::Verbose(
                _L_,
                L"Segment number 0x%.16I64X out of sequence 0x%.16lX (correction applied: 0x%.16lX)\r\n",
                ullRecordIndex,
                pRecord->m_FileReferenceNumber.SegmentNumberLowPart,
                ullRecordIndex - pRecord->m_FileReferenceNumber.SegmentNumberLowPart);
            ullRecordIndex = pRecord->m_FileReferenceNumber.SegmentNumberLowPart;
        }

        if (m_bIncludeNotInUse || (pRecord->m_pRecord->Flags & FILE_RECORD_SEGMENT_IN_USE))
        {
            MFTRecord* pBaseRecord = NULL;

            // check if this is not a base file record segment
            if (0 != NtfsSegmentNumber(&(pRecord->m_pRecord->BaseFileRecordSegment)))
            {

                log::Debug(
                    _L_,
                    L"Record %.16I64X is child record of %.16I64X\r\n",
                    NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber),
                    NtfsFullSegmentNumber(&(pRecord->m_pRecord->BaseFileRecordSegment)));

                auto pParentPair = m_MFTMap.find(NtfsFullSegmentNumber(&(pRecord->m_pRecord->BaseFileRecordSegment)));

                if (pParentPair != m_MFTMap.end())
                {
                    pBaseRecord = pParentPair->second;
                }
            }

            hr = pRecord->ParseRecord(
                _L_, m_pVolReader, pRecord->m_pRecord, m_pVolReader->GetBytesPerFRS(), pBaseRecord);

            if (hr == S_FALSE)
            {
                log::Debug(
                    _L_,
                    L"Skipping record %.16I64X (ParseRecord returned S_FALSE)\r\n",
                    NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber));
                DeleteRecord(pRecord);
            }
            else if (hr == S_OK)
            {
                log::Debug(_L_, L"Record %.16I64X parsed\r\n", NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber));

                m_MFTMap.insert(pair<MFTUtils::SafeMFTSegmentNumber, MFTRecord*>(
                    NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber), pRecord));

                if (FAILED(hr = AddDirectoryName(pRecord)))
                {
                    log::Verbose(
                        _L_,
                        L"FAILED to add directory name for record %.16I64X (hr=0x%lx)\r\n",
                        NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber),
                        hr);
                }
                std::shared_ptr<AttributeList> pAttributeList;

                if (pRecord->m_pAttributeList == nullptr)
                    pRecord->m_pAttributeList = make_shared<AttributeList>();

                if (pRecord->m_pAttributeList->IsPresent())
                {
                    pAttributeList = pRecord->m_pAttributeList;
                }
                else if (pRecord->m_pBaseFileRecord != NULL)
                {
                    if (pRecord->m_pBaseFileRecord->m_pAttributeList && pRecord->m_pBaseFileRecord->m_pAttributeList->IsPresent())
                        pAttributeList = pRecord->m_pBaseFileRecord->m_pAttributeList;
                }

                if (pAttributeList)
                {
                    // This record has child records (or is a child record), parsing them.

                    for (auto iter = pAttributeList->m_AttList.begin(); iter != pAttributeList->m_AttList.end(); iter++)
                    {
                        AttributeListEntry& attr = *iter;

                        if (attr.m_Attribute == NULL
                            && NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber)
                                != NtfsFullSegmentNumber(&attr.m_pListEntry->SegmentReference))
                        {
                            auto pParentPair =
                                m_MFTMap.find(NtfsFullSegmentNumber(&(attr.m_pListEntry->SegmentReference)));

                            if (pParentPair != m_MFTMap.end())
                            {
                                MFTRecord* pHostRecord = pParentPair->second;

                                if (pHostRecord != nullptr)
                                {
                                    if (!pHostRecord->IsParsed())
                                    {
                                        hr = pHostRecord->ParseRecord(
                                            _L_,
                                            m_pVolReader,
                                            pHostRecord->m_pRecord,
                                            m_pVolReader->GetBytesPerFRS(),
                                            pRecord);
                                        if (hr == S_FALSE)
                                        {
                                            log::Debug(
                                                _L_,
                                                L"Skipping record 0x%lx\r\n",
                                                NtfsFullSegmentNumber(&(attr.m_pListEntry->SegmentReference)));
                                        }
                                        else if (FAILED(hr))
                                        {
                                            log::Verbose(
                                                _L_,
                                                L"Parsing child record %.16I64X failed\r\n",
                                                NtfsFullSegmentNumber(&(attr.m_pListEntry->SegmentReference)));
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                pAddedRecord = pRecord;
            }
            else
            {
                if (hr == HRESULT_FROM_WIN32(ERROR_NO_DATA))
                {
                    log::Debug(
                        _L_,
                        L"Skipping empty record %.16I64X\r\n",
                        NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber));
                }
                else if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_OPERATION))
                {
                    log::Verbose(
                        _L_,
                        L"Parsing record failed: Non Resident data is unavailable for record %.16I64X\r\n",
                        NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber));
                }
                else
                {
                    log::Error(
                        _L_,
                        hr,
                        L"Parsing record failed %.16I64X\r\n",
                        NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber));
                }
                DeleteRecord(pRecord);
            }
        }
        else
        {
            log::Debug(
                _L_,
                L"Record %.16I64X: not in use, and ignored\r\n",
                NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber));
            DeleteRecord(pRecord);
        }
    }
    catch (const Orc::Exception& e)
    {
        e.PrintMessage(_L_);
        log::Error((_L_), E_FAIL, L"\r\nError while parsing record 0x%.16I64X\r\n", ullRecordIndex);
        pAddedRecord = nullptr;
    }
	catch (const std::exception & e)
	{
        log::Error((_L_), E_FAIL, L"\r\nParsing record 0x%.16I64X threw exception \"%S\"\r\n", ullRecordIndex, e.what());
        pAddedRecord = nullptr;
	}
	catch (...)
	{
	    log::Error((_L_), E_FAIL, L"\r\nParsing record 0x%.16I64X threw an exception\r\n", ullRecordIndex);
	    pAddedRecord = nullptr;
	}
    if (hr == HRESULT_FROM_WIN32(ERROR_NO_MORE_FILES))
        return hr;  // if hr==ERROR_NO_MORE_FILES we return this result to allow the walker to stop enumeration. All
                    // other HRs are ignored

    return S_OK;
}

HRESULT MFTWalker::AddRecordCallback(MFTUtils::SafeMFTSegmentNumber& ullRecordIndex, CBinaryBuffer& Data)
{
    HRESULT hr = E_FAIL;

    try
    {

        MFTRecord* pRecord = nullptr;

        if (FAILED(hr = AddRecord(ullRecordIndex, Data, pRecord)))
        {
            log::Error(_L_, hr, L"Failed to add record %I64d\r\n", ullRecordIndex);
            return hr;
        }

        if (pRecord == nullptr)
            return S_OK;

        std::vector<MFT_SEGMENT_REFERENCE> missingRecords;

        bool bIsComplete = false;
        bool bProceed = false;

        std::set<MFT_SEGMENT_REFERENCE> fetchedRecords;

        while (!(bIsComplete = IsRecordComplete(pRecord, missingRecords)))
        {
            if (missingRecords.empty())
            {
                // incomplete but no missing records?
                break;
            }

            std::sort(
                begin(missingRecords),
                end(missingRecords),
                [](const MFT_SEGMENT_REFERENCE& left, const MFT_SEGMENT_REFERENCE& rigth) -> bool {
                    if (left.SegmentNumberHighPart != rigth.SegmentNumberHighPart)
                        return left.SegmentNumberHighPart < rigth.SegmentNumberHighPart;
                    return left.SegmentNumberLowPart < rigth.SegmentNumberLowPart;
                });

            auto new_end = std::unique(
                begin(missingRecords),
                end(missingRecords),
                [](const MFT_SEGMENT_REFERENCE& left, const MFT_SEGMENT_REFERENCE& rigth) -> bool {
                    return left.SegmentNumberHighPart == rigth.SegmentNumberHighPart
                        && left.SegmentNumberLowPart == rigth.SegmentNumberLowPart;
                });

            auto new_new_end = std::remove_if(
                begin(missingRecords), new_end, [&fetchedRecords](const MFT_SEGMENT_REFERENCE& one) -> bool {
                    return fetchedRecords.find(one) != end(fetchedRecords);
                });

            missingRecords.erase(new_new_end, end(missingRecords));

            if (missingRecords.empty())
            {
                bProceed = true;
                break;  // if we could not determine a list of records to fetch, stop being stuborn
            }

            std::vector<MFT_SEGMENT_REFERENCE> missingCopy;

            std::swap(missingCopy, missingRecords);

            fetchedRecords.insert(begin(missingCopy), end(missingCopy));

            if (FAILED(
                    hr = m_pMFT->FetchMFTRecord(
                        missingCopy,
                        [this, &missingRecords](
                            MFTUtils::SafeMFTSegmentNumber& ullRecordIndex, CBinaryBuffer& Data) -> HRESULT {
                            MFTRecord* pRecord = nullptr;
                            HRESULT hr = E_FAIL;
                            if (FAILED(hr = AddRecord(ullRecordIndex, Data, pRecord)))
                                return hr;

                            if (FAILED(hr) || pRecord == nullptr)
                            {
                                log::Verbose(_L_, L"Fetched record %I64X is incomplete\r\n", ullRecordIndex);
                                return S_OK;
                            }

                            if (hr != S_FALSE)
                            {
                                bool bIsComplete = false;
                                if (bIsComplete = IsRecordComplete(pRecord, missingRecords))
                                {
                                    log::Verbose(_L_, L"Fetched record %I64X is complete\r\n", ullRecordIndex);
                                }
                                else
                                {
                                    log::Verbose(_L_, L"Fetched record %I64X is incomplete\r\n", ullRecordIndex);
                                }
                            }
                            return S_OK;
                        })))
            {
                log::Error(_L_, hr, L"Failed to fetch records\r\n");
                break;
            }

            if (FAILED(hr = UpdateAttributeList(pRecord)))
            {
                log::Error(_L_, hr, L"Failed to update attribute list\r\n");
                break;
            }
            if (NtfsFullSegmentNumber(&pRecord->m_pRecord->BaseFileRecordSegment) != 0LL
                && pRecord->m_pBaseFileRecord == nullptr)
            {
                auto pBase = m_MFTMap.find(NtfsFullSegmentNumber(&pRecord->m_pRecord->BaseFileRecordSegment));

                if (pBase != end(m_MFTMap))
                {
                    pRecord->m_pBaseFileRecord = pBase->second;
                }
            }

            if (pRecord->m_pBaseFileRecord)
            {
                if (FAILED(hr = UpdateAttributeList(pRecord->m_pBaseFileRecord)))
                {
                    log::Error(_L_, hr, L"Failed to update master record attribute list\r\n");
                    break;
                }
            }
        }

        if (bIsComplete || bProceed)
        {
            log::Debug(
                _L_,
                L"Record %.16I64X is complete, calling callback\r\n",
                NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber));

            if (m_Callbacks.SecDescCallback != nullptr
                && NtfsFullSegmentNumber(&pRecord->GetFileReferenceNumber()) == $SECURE_FILE_REFERENCE_NUMBER)
            {
                if (FAILED(hr = Parse$SecureAndCallback(pRecord)))
                {
                    log::Verbose(
                        _L_,
                        L"Failed to parse $Secure %.16I64X\r\n",
                        NtfsFullSegmentNumber(&pRecord->GetFileReferenceNumber()));
                }
            }

            bool bFreeRecord = false;

            if (FAILED(hr = m_pCallbackCall(this, pRecord, bFreeRecord)))
            {
                if (hr == HRESULT_FROM_WIN32(ERROR_NO_MORE_FILES))
                {
                    log::Verbose(
                        _L_,
                        L"Callback call is asking to stop walk at record %.16I64X\r\n",
                        NtfsFullSegmentNumber(&pRecord->GetFileReferenceNumber()));
                }
            }

            if (bFreeRecord)
                DeleteRecord(pRecord);
        }
        else
        {
            log::Debug(
                _L_,
                L"Record %.16I64X is incomplete, missing %d records\r\n",
                NtfsFullSegmentNumber(&pRecord->m_FileReferenceNumber),
                missingRecords.size());
        }
    }
    catch (Orc::Exception& e)
    {
        e.PrintMessage(_L_);
        log::Error(_L_, E_FAIL, L"\r\nError while parsing record %.16I64X\r\n", ullRecordIndex);
    }
	catch (const std::exception & e)
	{
        log::Error((_L_), E_FAIL, L"\r\nParsing record 0x%.16I64X threw exception \"%S\"\r\n", ullRecordIndex, e.what());
	}
    catch (...)
    {
        log::Error(_L_, E_FAIL, L"\r\nParsing record %.16I64X threw an exception\r\n", ullRecordIndex);
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_NO_MORE_FILES))
        return hr;  // if hr==ERROR_NO_MORE_FILES we return this result to allow the walker to stop enumeration. All
                    // other HRs are ignored

    return S_OK;
}

HRESULT MFTWalker::Walk(const Callbacks& Callbacks)
{
    HRESULT hr = E_FAIL;

    if (FAILED(hr = SetCallbacks(Callbacks)))
        return hr;

    m_ulMFTRecordCount = GetMFTRecordCount();

    if (m_ulMFTRecordCount > 0)
    {
        hr = m_pMFT->EnumMFTRecord(
            [this](MFTUtils::SafeMFTSegmentNumber& ullRecordIndex, CBinaryBuffer& Data) -> HRESULT {
                return AddRecordCallback(ullRecordIndex, Data);
            });
    }

    if (hr == HRESULT_FROM_WIN32(ERROR_NO_MORE_FILES))
    {
        return hr;  // no more enumeration nor walking...
    }

    return WalkRecords(true);
}

ULONG MFTWalker::GetMFTRecordCount() const
{
    if (nullptr != m_pMFT)
    {
        return m_pMFT->GetMFTRecordCount();
    }

    return 0;
}

HRESULT MFTWalker::Statistics(const WCHAR* szMsg)
{
    HRESULT hr = E_FAIL;
    log::Verbose(_L_, L"\r\nMFT Walker statistics : %s\r\n", szMsg);

    log::Verbose(_L_, L"\tMap Count: %d\r\n", m_MFTMap.size());

    DWORD dwDeletedDirCount = 0;
    DWORD dwDeletedNotParsedCount = 0;
    DWORD dwDeletedIncompleteCount = 0;
    DWORD dwDeletedAvailableEntries = 0;

    DWORD dwDirCount = 0;
    DWORD dwNotParsedCount = 0;
    DWORD dwIncompleteCount = 0;
    DWORD dwAvailableEntries = 0;

    std::for_each(begin(m_MFTMap), end(m_MFTMap), [&](const pair<MFTUtils::SafeMFTSegmentNumber, MFTRecord*>& pair) {
        if (pair.second != nullptr)
        {
            dwDirCount += pair.second->IsDirectory() ? 1 : 0;
            dwNotParsedCount += !pair.second->IsParsed() ? 1 : 0;
            dwIncompleteCount += !pair.second->m_bIsComplete ? 1 : 0;
            dwAvailableEntries++;

            if (!(pair.second->m_pRecord->Flags & FILE_RECORD_SEGMENT_IN_USE))
            {
                dwDeletedDirCount += pair.second->IsDirectory() ? 1 : 0;
                dwDeletedNotParsedCount += !pair.second->IsParsed() ? 1 : 0;
                dwDeletedIncompleteCount += !pair.second->m_bIsComplete ? 1 : 0;
                dwDeletedAvailableEntries++;
            }
        }
    });
    if (m_bIncludeNotInUse)
    {
        log::Debug(
            _L_,
            L"\tDeleted -> Available: %d Directories: %d Not parsed: %d, Incomplete: %d\r\n",
            dwDeletedAvailableEntries,
            dwDeletedDirCount,
            dwDeletedNotParsedCount,
            dwDeletedIncompleteCount);
    }
    log::Debug(
        _L_,
        L"\tTotal   -> Available: %d Directories: %d Not parsed: %d, Incomplete: %d\r\n",
        dwAvailableEntries,
        dwDirCount,
        dwNotParsedCount,
        dwIncompleteCount);

    if (m_SegmentStore.AllocatedCells() > 0)
    {
        log::Info(_L_, L"\r\nWARNING: Heap still maintains %d entries\r\n", m_SegmentStore.AllocatedCells());
    }

#ifdef _DEBUG

    if (FAILED(hr = m_SegmentStore.EnumCells([this](void* pData) {
            MFTRecord* pRecord = (MFTRecord*)pData;

            log::Info(_L_, L"\tRecord: %.16I64X\r\n", pRecord->GetSafeMFTSegmentNumber());
        })))
    {
        log::Error(_L_, hr, L"\r\nFailed to enumerate segment store entries\r\n", m_SegmentStore.AllocatedCells());
    }

#endif
    return S_OK;
}

MFTWalker::~MFTWalker()
{
    for_each(begin(m_MFTMap), end(m_MFTMap), [](const pair<MFTUtils::SafeMFTSegmentNumber, MFTRecord*>& pair) {
        if (pair.second != nullptr)  //&& !IsBadReadPtr(pair.second, sizeof(MFTRecord*)))
        {
            pair.second->~MFTRecord();
        }
    });
}
