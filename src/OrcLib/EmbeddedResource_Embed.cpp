//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright © 2011-2019 ANSSI. All Rights Reserved.
//
// Author(s): Jean Gautier (ANSSI)
//

#include "stdafx.h"

#include <agents.h>

#include "EmbeddedResource.h"

#include "MemoryStream.h"
#include "FileStream.h"
#include "ArchiveCreate.h"
#include "ZipCreate.h"

#include "RunningProcesses.h"
#include "ParameterCheck.h"
#include "SystemDetails.h"
#include "Temporary.h"

using namespace std;
using namespace Orc;

HRESULT EmbeddedResource::_UpdateResource(
    HANDLE& hOutput,
    const WCHAR* szModule,
    const WCHAR* szType,
    const WCHAR* szName,
    LPVOID pData,
    DWORD cbSize)
{
    HRESULT hr = E_FAIL;
    HANDLE hOut = hOutput;

    if (hOutput == INVALID_HANDLE_VALUE)
    {
        hOut = BeginUpdateResource(szModule, FALSE);
        if (hOut == NULL)
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            Log::Debug(L"Failed to update ressource in '{}' (BeginUpdateResource) [{}]", szModule, SystemError(hr));
            return hr;
        }
    }

    if (!UpdateResource(hOut, szType, szName, MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), pData, cbSize))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        Log::Debug(L"Failed to add ressource '{}' (UpdateResource) [{}]", szName, SystemError(hr));
        return hr;
    }

    if (hOutput == INVALID_HANDLE_VALUE)
    {
        // Calling EndUpdateResource will make the handle invalid for success AND failure
        hOutput = INVALID_HANDLE_VALUE;

        if (!EndUpdateResource(hOut, FALSE))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            Log::Debug(L"Failed to update ressource in '{}' (EndUpdateResource) [{}]", szModule, SystemError(hr));
            return hr;
        }
    }
    return S_OK;
}

HRESULT EmbeddedResource::UpdateResources(const std::wstring& strPEToUpdate, const std::vector<EmbedSpec>& ToEmbed)
{
    HRESULT hr = S_OK;

    DWORD dwMajor = 0L, dwMinor = 0L;
    SystemDetails::GetOSVersion(dwMajor, dwMinor);

    HANDLE hOutput = INVALID_HANDLE_VALUE;

    bool bAtomicUpdate = false;
    if (dwMajor <= 5)
        bAtomicUpdate = true;

    if (!bAtomicUpdate)
    {
        hOutput = BeginUpdateResource(strPEToUpdate.c_str(), FALSE);
        if (hOutput == NULL)
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            Log::Error(
                L"Failed to update ressources in '{}' (BeginUpdateResource) [{}]", strPEToUpdate, SystemError(hr));
            return hr;
        }
    }

    for (auto iter = ToEmbed.begin(); iter != ToEmbed.end(); ++iter)
    {
        const EmbeddedResource::EmbedSpec& item = *iter;

        switch (item.Type)
        {
            case EmbeddedResource::EmbedSpec::EmbedType::NameValuePair:

                if (SUCCEEDED(
                        hr = _UpdateResource(
                            hOutput,
                            strPEToUpdate.c_str(),
                            EmbeddedResource::VALUES(),
                            item.Name.c_str(),
                            (LPVOID)item.Value.c_str(),
                            (DWORD)item.Value.size() * sizeof(WCHAR))))
                {
                    Log::Info(L"Successfully added {}={}", item.Name, item.Value);
                }
                break;
            case EmbeddedResource::EmbedSpec::EmbedType::ValuesDeletion:
                if (SUCCEEDED(
                        hr = _UpdateResource(
                            hOutput, strPEToUpdate.c_str(), EmbeddedResource::VALUES(), item.Name.c_str(), NULL, 0L)))
                {
                    Log::Info(L"Successfully delete resource at position '{}' [{}]", item.Name, SystemError(hr));
                }
                break;
            case EmbeddedResource::EmbedSpec::EmbedType::BinaryDeletion:
                if (SUCCEEDED(
                        hr = _UpdateResource(
                            hOutput, strPEToUpdate.c_str(), EmbeddedResource::BINARY(), item.Name.c_str(), NULL, 0L)))
                {
                    Log::Info(L"Successfully delete resource at position '{}' [{}]", item.Name, SystemError(hr));
                }
                break;
            default:
                hr = S_OK;
                break;
        }
    }

    if (hr != S_OK)
    {
        return hr;
    }

    if (!bAtomicUpdate)
    {
        if (!EndUpdateResource(hOutput, FALSE))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            Log::Error(L"Failed to update ressources in '{}' (EndUpdateResource) [{}]", strPEToUpdate, SystemError(hr));
            return hr;
        }
    }

    for (auto iter = ToEmbed.begin(); iter != ToEmbed.end(); ++iter)
    {
        const EmbeddedResource::EmbedSpec& item = *iter;

        switch (item.Type)
        {
            case EmbeddedResource::EmbedSpec::EmbedType::File: {
                auto filestream = make_shared<FileStream>();

                if (FAILED(hr = filestream->ReadFrom(item.Value.c_str())))
                {
                    Log::Error(L"Failed to update ressources in '{}' (read failure) [{}]", item.Value, SystemError(hr));
                    return hr;
                }

                auto memstream = std::make_shared<MemoryStream>();

                if (FAILED(hr = memstream->OpenForReadWrite()))
                {
                    Log::Error(L"Failed to open mem resource in '{}' [{}]", item.Value, SystemError(hr));
                    return hr;
                }
                ULONGLONG ullFileSize = filestream->GetSize();

                if (FAILED(hr = memstream->SetSize((DWORD)ullFileSize)))
                {
                    Log::Error(L"Failed set size of mem resource in '{}' [{}]", item.Value, SystemError(hr));
                    return hr;
                }

                ULONGLONG ullBytesWritten = 0LL;
                if (FAILED(hr = filestream->CopyTo(memstream, &ullBytesWritten)))
                {
                    Log::Error(L"Failed to copy '{}' to a memory stream [{}]", item.Value, SystemError(hr));
                    return hr;
                }

                CBinaryBuffer data;
                memstream->GrabBuffer(data);

                if (SUCCEEDED(
                        hr = _UpdateResource(
                            INVALID_HANDLE_VALUE,
                            strPEToUpdate.c_str(),
                            EmbeddedResource::BINARY(),
                            item.Name.c_str(),
                            data.GetData(),
                            (DWORD)data.GetCount())))
                {
                    Log::Info(L"Successfully added '{}' at position '{}'", item.Value, item.Name);
                }
                else
                {
                    return hr;
                }
            }
            break;
            case EmbeddedResource::EmbedSpec::EmbedType::Archive: {
                ArchiveFormat fmt = ArchiveFormat::Unknown;

                if (item.ArchiveFormat.empty())
                    fmt = ArchiveFormat::SevenZip;
                else
                    fmt = ArchiveCreate::GetArchiveFormat(item.ArchiveFormat);

                if (fmt == ArchiveFormat::Unknown)
                {
                    Log::Error(L"Failed to use archive format '{}'", item.ArchiveFormat);
                    return E_INVALIDARG;
                }
                auto creator = ArchiveCreate::MakeCreate(fmt, false);

                auto memstream = std::make_shared<MemoryStream>();

                if (FAILED(hr = memstream->OpenForReadWrite()))
                {
                    Log::Error("Failed to initialize memory stream [{}]", SystemError(hr));
                    return hr;
                }

                if (FAILED(hr = creator->InitArchive(memstream)))
                {
                    Log::Error(L"Failed to initialize archive stream [{}]", SystemError(hr));
                    return hr;
                }

                if (!item.ArchiveCompression.empty())
                {
                    if (FAILED(hr = creator->SetCompressionLevel(item.ArchiveCompression)))
                    {
                        Log::Error(L"Invalid compression level '{}' [{}]", item.ArchiveCompression, SystemError(hr));
                        return hr;
                    }
                }

                hr = S_OK;

                for (auto arch_item : item.ArchiveItems)
                {
                    hr = creator->AddFile(arch_item.Name.c_str(), arch_item.Path.c_str(), false);
                    if (FAILED(hr))
                    {
                        Log::Error(L"Failed to add file '{}' to archive", arch_item.Path);
                        return hr;
                    }
                    else
                    {
                        Log::Info(L"Successfully added '{}' to archive", arch_item.Path);
                    }
                }

                if (FAILED(hr = creator->Complete()))
                {
                    Log::Error("Failed to complete archive [{}]", SystemError(hr));
                    return hr;
                }

                CBinaryBuffer data;
                memstream->GrabBuffer(data);

                if (SUCCEEDED(
                        hr = _UpdateResource(
                            INVALID_HANDLE_VALUE,
                            strPEToUpdate.c_str(),
                            EmbeddedResource::BINARY(),
                            item.Name.c_str(),
                            data.GetData(),
                            (DWORD)data.GetCount())))
                {
                    Log::Info(L"Successfully added archive '{}'", item.Name);
                }
                else
                {
                    return hr;
                }
            }
            break;
            default:
                break;
        }
    }
    return hr;
}
