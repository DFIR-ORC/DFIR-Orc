//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright © 2011-2019 ANSSI. All Rights Reserved.
//
// Author(s): Jean Gautier (ANSSI)
//

#include "stdafx.h"

#include "DiskExtent.h"

#include "LogFileWriter.h"

#include "Kernel32Extension.h"

#include <crtdbg.h>

using namespace Orc;

CDiskExtent::CDiskExtent(
    logger pLog,
    const std::wstring& name,
    ULONGLONG ullStart,
    ULONGLONG ullLength,
    ULONG ulSectorSize)
{
    m_Name = name;
    m_hFile = INVALID_HANDLE_VALUE;

    m_Start = ullStart;
    m_Length = ullLength;
    m_LogicalSectorSize = m_PhysicalSectorSize = ulSectorSize;
    m_liCurrentPos.QuadPart = 0LL;
}

CDiskExtent::CDiskExtent(logger pLog, const std::wstring& name)
    : _L_(std::move(pLog))
{
    m_Name = name;
    m_hFile = INVALID_HANDLE_VALUE;

    m_Start = 0;
    m_Length = 0;
    m_LogicalSectorSize = m_PhysicalSectorSize = 0;
    m_liCurrentPos.QuadPart = 0LL;
}

CDiskExtent::CDiskExtent(CDiskExtent&& Other) noexcept
{
    std::swap(_L_, Other._L_);
    m_hFile = Other.m_hFile;
    Other.m_hFile = INVALID_HANDLE_VALUE;
    m_Length = Other.m_Length;
    Other.m_Length = 0;
    m_LogicalSectorSize = Other.m_LogicalSectorSize;
    Other.m_LogicalSectorSize = 0;
    m_PhysicalSectorSize = Other.m_PhysicalSectorSize;
    Other.m_PhysicalSectorSize = 0;
    m_liCurrentPos = Other.m_liCurrentPos;
    m_Start = Other.m_Start;
    m_Name = std::move(Other.m_Name);
}

CDiskExtent::CDiskExtent(const CDiskExtent& Other) noexcept
    : _L_(Other._L_)
    , m_Name(Other.m_Name)
{
    m_hFile = INVALID_HANDLE_VALUE;
    m_Length = Other.m_Length;
    m_LogicalSectorSize = Other.m_LogicalSectorSize;
    m_PhysicalSectorSize = Other.m_PhysicalSectorSize;
    m_liCurrentPos = Other.m_liCurrentPos;
    m_Start = Other.m_Start;
}

CDiskExtent& CDiskExtent::operator=(const CDiskExtent& other)
{
    _L_ = other._L_;
    m_hFile = INVALID_HANDLE_VALUE;
    m_Length = other.m_Length;
    m_liCurrentPos.QuadPart = 0LL;
    m_LogicalSectorSize = other.m_LogicalSectorSize;
    m_Name = other.m_Name;
    m_PhysicalSectorSize = other.m_PhysicalSectorSize;
    m_Start = other.m_Start;
    return *this;
}

HRESULT CDiskExtent::Open(DWORD dwShareMode, DWORD dwCreationDisposition, DWORD dwFlags)
{
    auto fullname = m_Name;
    if (fullname.empty())
    {
        return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    }
    if (fullname[fullname.length() - 1] != L'\\')
    {
        fullname += L'\\';
    }

    switch (GetDriveTypeW(fullname.c_str()))
    {
        case DRIVE_UNKNOWN:
        case DRIVE_NO_ROOT_DIR:
        {
            log::Error(
                _L_,
                HRESULT_FROM_WIN32(ERROR_BAD_DEVICE),
                L"Cannot open location %s: Unrecognised, No root dir or unknown device",
                fullname.c_str());
        }
        case DRIVE_CDROM:
            return HRESULT_FROM_WIN32(ERROR_BAD_DEVICE);
        case DRIVE_REMOTE:
        default:
            break;
    }

    m_hFile = CreateFile(m_Name.c_str(), GENERIC_READ, dwShareMode, NULL, dwCreationDisposition, dwFlags, NULL);

    if (INVALID_HANDLE_VALUE == m_hFile)
    {
        log::Verbose(
            _L_,
            L"Failed to CreateFile(FILE_FLAG_SEQUENTIAL_SCAN) - \"%ws\" (hr=0x%lx).\r\n",
            m_Name.c_str(),
            HRESULT_FROM_WIN32(GetLastError()));
        return HRESULT_FROM_WIN32(GetLastError());
    }

    ULARGE_INTEGER liLength = {0};
    DWORD dwOutBytes = 0;
    DWORD ioctlLastError, lastError;

    // Get disk size
    GET_LENGTH_INFORMATION lenInfo;
    if (DeviceIoControl(
            m_hFile, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &lenInfo, sizeof(GET_LENGTH_INFORMATION), &dwOutBytes, NULL))
    {
        m_Length = lenInfo.Length.QuadPart;
    }
    else  // try to get file size with GetFileSize
    {
        ioctlLastError = GetLastError();

        log::Warning(
            _L_,
            ioctlLastError,
            L"[CDiskExtent] Unable to determine disk size with IOCTL (Name: '%s', IOCTL_DISK_GET_LENGTH_INFO=0x%lx)\r\n",
            m_Name.c_str(),
            ioctlLastError);

        liLength.LowPart = GetFileSize(m_hFile, &liLength.HighPart);
        m_Length = liLength.QuadPart;
        if (liLength.LowPart == INVALID_FILE_SIZE && ((lastError = GetLastError()) != NO_ERROR))
        {
            log::Warning(
                _L_,
                HRESULT_FROM_WIN32(lastError),
                L"[CDiskExtent] Unable to determine disk size with GetFileSize (Name: '%s', hr=0x%lx)\r\n",
                m_Name.c_str(),
                HRESULT_FROM_WIN32(lastError));
            m_Length = 0;
        }
    }

    // Get sector size
    DISK_GEOMETRY Geometry;
    if (DeviceIoControl(
            m_hFile, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &Geometry, sizeof(DISK_GEOMETRY), &dwOutBytes, NULL))
    {
        // Length calculated this way can be wrong (seen less than the real size)
        // m_Length = Geometry.Cylinders.QuadPart * Geometry.TracksPerCylinder * Geometry.SectorsPerTrack *
        // Geometry.BytesPerSector;
        m_LogicalSectorSize = Geometry.BytesPerSector;
    }
    else
    {
        ioctlLastError = GetLastError();
        log::Debug(
            _L_,
            L"[CDiskExtent] Unable to determine sector size (IOCTL_DISK_GET_DRIVE_GEOMETRY error=0x%lx), fallback to "
            L"a size of 512.\r\n",
            ioctlLastError);
        m_LogicalSectorSize = 512;
    }

    if (m_Start > 0)
    {
        LARGE_INTEGER offset = {0};
        return Seek(offset, NULL, FILE_BEGIN);
    }

    return S_OK;
}

HRESULT CDiskExtent::Read(__in_bcount(dwCount) PVOID lpBuf, DWORD dwCount, PDWORD pdwBytesRead)
{
    HRESULT hr = S_OK;
    _ASSERT(INVALID_HANDLE_VALUE != m_hFile);
    _ASSERT(pdwBytesRead != nullptr);

    DWORD dwBytesRead = 0;
    log::Verbose(_L_, L"CDiskExtent Reading 0x%lx bytes\r\n", dwCount);
    if (!ReadFile(m_hFile, lpBuf, dwCount, &dwBytesRead, NULL))
    {
        log::Warning(
            _L_, hr = HRESULT_FROM_WIN32(GetLastError()), L"Failed to read %d bytes from disk extent\r\n", dwCount);
        *pdwBytesRead = 0;
        return hr;
    }

    m_liCurrentPos.QuadPart += dwBytesRead;
    *pdwBytesRead = dwBytesRead;
    return S_OK;
}

HRESULT CDiskExtent::Seek(LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER pliNewFilePointer, DWORD dwFrom)
{
    _ASSERT(INVALID_HANDLE_VALUE != m_hFile);

    if (dwFrom == FILE_BEGIN)
        liDistanceToMove.QuadPart += m_Start;

    log::Verbose(_L_, L"Moving from %#.16I64X to %#.16I64X\r\n", m_liCurrentPos.QuadPart, liDistanceToMove.QuadPart);

    if (!SetFilePointerEx(m_hFile, liDistanceToMove, &m_liCurrentPos, dwFrom))
    {
        auto lastError = GetLastError();
        log::Error(_L_, HRESULT_FROM_WIN32(lastError), L"Failed to set file pointer on file\r\n");
        return HRESULT_FROM_WIN32(lastError);
    }
    if (pliNewFilePointer != NULL)
        *pliNewFilePointer = m_liCurrentPos;

    return S_OK;
}

void CDiskExtent::Close()
{
    if (INVALID_HANDLE_VALUE != m_hFile)
    {
        CloseHandle(m_hFile);
        m_hFile = INVALID_HANDLE_VALUE;
    }
}

CDiskExtent CDiskExtent::ReOpen(DWORD dwDesiredAccess, DWORD dwShareMode, DWORD dwFlags) const
{
    CDiskExtent ext(_L_);
    ext.m_hFile = INVALID_HANDLE_VALUE;
    ext.m_Length = m_Length;
    ext.m_liCurrentPos.QuadPart = 0LL;
    ext.m_LogicalSectorSize = m_LogicalSectorSize;
    ext.m_Name = m_Name;
    ext.m_PhysicalSectorSize = m_PhysicalSectorSize;
    ext.m_Start = m_Start;

    const auto k32 = ExtensionLibrary::GetLibrary<Kernel32Extension>(_L_, true);

    if (k32 != nullptr)
    {
        ext.m_hFile = k32->ReOpenFile(m_hFile, dwDesiredAccess, dwShareMode, dwFlags);
    }

    if (ext.m_hFile == INVALID_HANDLE_VALUE)
    {
        if (!DuplicateHandle(
                GetCurrentProcess(), m_hFile, GetCurrentProcess(), &ext.m_hFile, dwDesiredAccess, FALSE, 0L))
        {
            log::Error(_L_, HRESULT_FROM_WIN32(GetLastError()), L"Failed to duplicate disk extent's handle\r\n");
            ext.m_hFile = INVALID_HANDLE_VALUE;
        }
    }
    return ext;
}

CDiskExtent::~CDiskExtent()
{
    Close();
}
