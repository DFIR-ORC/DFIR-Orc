//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright © 2011-2019 ANSSI. All Rights Reserved.
//
// Author(s): Jean Gautier (ANSSI)
//
#include "StdAfx.h"

#include <regex>
#include <algorithm>
#include <sstream>

#include <agents.h>

#include "CommandExecute.h"

#include "JobObject.h"
#include "ProcessRedirect.h"
#include "ParameterCheck.h"
#include "LogFileWriter.h"

#include "Temporary.h"

#include "Robustness.h"

using namespace std;

using namespace Orc;

static const auto MAX_CMDLINE = 32768;

HRESULT OnComplete::OnCompleteTerminationHandler::operator()()
{
    switch (m_object)
    {
        case OnComplete::File:
            if (!DeleteFile(m_FullPath.c_str()))
            {
                if (GetLastError() != ERROR_FILE_NOT_FOUND)
                    return HRESULT_FROM_WIN32(GetLastError());
            }
            break;
        case OnComplete::Directory:
            if (!RemoveDirectory(m_FullPath.c_str()))
            {
                if (GetLastError() != ERROR_FILE_NOT_FOUND)
                    return HRESULT_FROM_WIN32(GetLastError());
            }
            break;
        case OnComplete::Stream:
            m_Stream->Close();
            break;
        default:
            break;
    }

    return S_OK;
}

OnComplete::OnComplete(Action action, ArchiveMessage::ITarget* pArchive)
    : m_pArchive(pArchive)
{
    if (action != FlushArchiveQueue)
        m_action = Void;
    else
        m_action = action;
    m_object = OnComplete::ArchiveQueue;
    m_pTerminationHandler = nullptr;
}

OnComplete::OnComplete(
    Action action,
    const std::wstring& name,
    const std::wstring& fullpath,
    ArchiveMessage::ITarget* pCab)
    : m_action(action)
    , m_object(File)
    , m_Name(name)
    , m_FullPath(fullpath)
    , m_pArchive(pCab)
    , m_pTerminationHandler(nullptr)
{
    if (action == OnComplete::Delete || action == OnComplete::ArchiveAndDelete)
    {
        m_pTerminationHandler = std::make_shared<OnCompleteTerminationHandler>(
            L"File delete upon abnormal termination", m_object, fullpath);
        Robustness::AddTerminationHandler(m_pTerminationHandler);
    }
}

OnComplete::OnComplete(
    Action action,
    const std::wstring& name,
    const std::wstring& fullpath,
    const std::wstring& matchPattern,
    ArchiveMessage::ITarget* pArchive)
    : m_action(action)
    , m_Name(name)
    , m_object(Directory)
    , m_FullPath(fullpath)
    , m_Pattern(matchPattern)
    , m_pArchive(pArchive)
    , m_pTerminationHandler(nullptr)
{
    if (action == OnComplete::Delete || action == OnComplete::ArchiveAndDelete)
    {
        m_pTerminationHandler = std::make_shared<OnCompleteTerminationHandler>(
            L"Directory delete upon abnormal termination", m_object, fullpath);
        Robustness::AddTerminationHandler(m_pTerminationHandler);
    }
}

OnComplete::OnComplete(
    Action action,
    const std::wstring& name,
    const std::shared_ptr<ByteStream>& stream,
    ArchiveMessage::ITarget* pArchive)
    : m_action(action)
    , m_Name(name)
    , m_object(Stream)
    , m_stream(stream)
    , m_pArchive(pArchive)
    , m_pTerminationHandler(nullptr)
{
    if (action == OnComplete::Delete || action == OnComplete::ArchiveAndDelete)
    {
        m_pTerminationHandler = std::make_shared<OnCompleteTerminationHandler>(
            L"Stream closure upon abnormal termination", m_object, stream);
        Robustness::AddTerminationHandler(m_pTerminationHandler);
    }
}

OnComplete::~OnComplete()
{
    CancelTerminationHandler();
}

void OnComplete::CancelTerminationHandler()
{
    std::shared_ptr<TerminationHandler> pTerminationHandler = nullptr;
    {
        concurrency::critical_section::scoped_lock sl(m_cs);
        pTerminationHandler = m_pTerminationHandler;
        m_pTerminationHandler = nullptr;
    }

    if (pTerminationHandler != nullptr)
    {
        Robustness::RemoveTerminationHandler(pTerminationHandler);
        pTerminationHandler.reset();
    }
}

CommandExecute::CommandExecute(logger pLog, const std::wstring& Keyword)
    : _L_(std::move(pLog))
    , m_Keyword(Keyword)
    , m_RedirectStatus(ProcessRedirect::Initialized)
{
    m_dwExitCode = 0L;
    ZeroMemory(&m_si, sizeof(m_si));
    m_si.cb = sizeof(m_si);
    ZeroMemory(&m_pi, sizeof(m_pi));
    m_Status = Initialized;
}

HRESULT CommandExecute::AddRedirection(const shared_ptr<ProcessRedirect>& redirect)
{
    if (std::any_of(m_Redirections.begin(), m_Redirections.end(), [redirect](const shared_ptr<ProcessRedirect>& item) {
            return redirect->Selection() & item->Selection();
        }))
    {
        log::Error(_L_, E_INVALIDARG, L"a redirection for this handle is already added\r\n");
        return E_INVALIDARG;
    }
    else
    {
        m_Redirections.push_back(redirect);
    }
    return S_OK;
}

HRESULT CommandExecute::AddArgument(const std::wstring& arg, const LONG OrderID)
{
    m_Arguments.push_back(std::pair<wstring, LONG>(arg, OrderID));
    return S_OK;
}

HRESULT CommandExecute::AddExecutableToRun(const std::wstring& szImageFilePath)
{
    if (!m_ImageFilePath.empty())
    {
        log::Error(
            _L_,
            E_INVALIDARG,
            L"%s has already been set the binary to execute. %s tried to overwrite",
            m_ImageFilePath.c_str(),
            szImageFilePath.c_str());
        return E_INVALIDARG;
    }

    WCHAR inputfile[MAX_PATH] = {0};
    if (FAILED(GetInputFile(szImageFilePath.c_str(), inputfile, MAX_PATH)))
    {
        log::Error(_L_, E_INVALIDARG, L"%s is not a valid file to use\r\n", szImageFilePath.c_str());
        return E_INVALIDARG;
    }
    m_ImageFilePath.assign(inputfile);
    return S_OK;
}

HANDLE CommandExecute::GetChildHandleFor(ProcessRedirect::ProcessInOut selection)
{
    // Check for existing redirections
    auto iter = std::find_if(
        m_Redirections.begin(),
        m_Redirections.end(),
        [selection](const std::shared_ptr<ProcessRedirect>& item) -> bool {
            return item->GetChildHandleFor(selection) != INVALID_HANDLE_VALUE;
        });
    if (iter != m_Redirections.end())
        return (*iter)->GetChildHandleFor(selection);

    // No redirection found, use my own ones.
    if (selection & ProcessRedirect::StdInput)
        return GetStdHandle(STD_INPUT_HANDLE);
    if (selection & ProcessRedirect::StdOutput)
        return GetStdHandle(STD_OUTPUT_HANDLE);
    if (selection & ProcessRedirect::StdError)
        return GetStdHandle(STD_ERROR_HANDLE);

    return INVALID_HANDLE_VALUE;
}

HANDLE CommandExecute::GetParentHandleFor(ProcessRedirect::ProcessInOut selection)
{
    // Check for existing redirections
    auto iter = std::find_if(
        m_Redirections.begin(),
        m_Redirections.end(),
        [selection](const std::shared_ptr<ProcessRedirect>& item) -> bool {
            return item->GetParentHandleFor(selection) != INVALID_HANDLE_VALUE;
        });

    if (iter != m_Redirections.end())
        return (*iter)->GetParentHandleFor(selection);

    // No redirection found, use my own ones.
    if (selection & ProcessRedirect::StdInput)
        return GetStdHandle(STD_INPUT_HANDLE);
    if (selection & ProcessRedirect::StdOutput)
        return GetStdHandle(STD_OUTPUT_HANDLE);
    if (selection & ProcessRedirect::StdError)
        return GetStdHandle(STD_ERROR_HANDLE);

    return INVALID_HANDLE_VALUE;
}

HRESULT CommandExecute::AddDumpFileDirectory(const std::wstring& strDirectory)
{
    m_DumpFilePath = strDirectory;
    return S_OK;
}

HRESULT CommandExecute::Execute(const JobObject& job, bool bBreakAway)
{
    HRESULT hr = E_FAIL;
    wstring cmdLineBuilder;

    cmdLineBuilder = L"\"";
    cmdLineBuilder += m_ImageFilePath;
    cmdLineBuilder += L"\"";

    std::sort(
        m_Arguments.begin(),
        m_Arguments.end(),
        [](const std::pair<wstring, LONG>& left, const std::pair<wstring, LONG>& rigth) -> bool {
            return left.second < rigth.second;
        });

    std::for_each(m_Arguments.begin(), m_Arguments.end(), [&cmdLineBuilder](const std::pair<wstring, LONG>& anArg) {
        cmdLineBuilder += L" ";
        cmdLineBuilder += anArg.first;
    });

    if (cmdLineBuilder.size() > MAX_CMDLINE)
    {
        log::Error(
            _L_,
            E_INVALIDARG,
            L"Command line too long (length=%d): \t%s\r\n",
            cmdLineBuilder.size(),
            cmdLineBuilder.c_str());
        return E_INVALIDARG;
    }

    // Set up the start up info struct.
    ZeroMemory(&m_si, sizeof(STARTUPINFO));
    m_si.cb = sizeof(STARTUPINFO);

    if (bBreakAway && job.IsValid())
    {
        log::Verbose(_L_, L"INFO: Launching process is in a job, we need to check if break away is OK\r\n");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION LimitInfo;
        DWORD dwReturnedBytes = 0L;
        if (!QueryInformationJobObject(
                job.GetHandle(),
                JobObjectExtendedLimitInformation,
                &LimitInfo,
                sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION),
                &dwReturnedBytes))
        {
            log::Error(_L_, HRESULT_FROM_WIN32(GetLastError()), L"Failed to QueryInformationJobObject on job\r\n");
            return hr;
        }
        if (LimitInfo.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_BREAKAWAY_OK
            || LimitInfo.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)
        {
            log::Verbose(_L_, L"Job verification is OK with breakaway\r\n");
        }
        else
        {
            log::Error(
                _L_,
                hr = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED),
                L"Job limit configuration is NOT OK with the command engine (breakaway is not allowed)\r\n");
            return hr;
        }
    }

    DWORD dwCreationFlags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;

    if (bBreakAway && job.IsValid())
    {
        dwCreationFlags |= CREATE_BREAKAWAY_FROM_JOB;
    }

    if (!m_Redirections.empty())
    {
        m_si.hStdOutput = GetChildHandleFor(ProcessRedirect::StdOutput);
        m_si.hStdInput = GetChildHandleFor(ProcessRedirect::StdInput);
        m_si.hStdError = GetChildHandleFor(ProcessRedirect::StdError);
        m_si.wShowWindow = SW_HIDE;
        m_si.dwFlags |= STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES | FILE_FLAG_OVERLAPPED;

        dwCreationFlags |= CREATE_NO_WINDOW;
    }
    else
    {
        m_si.wShowWindow = SW_HIDE;
        m_si.dwFlags |= STARTF_USESHOWWINDOW;
    }

    {
        std::vector<WCHAR> szCommandLine(MAX_CMDLINE);

        wcsncpy_s(szCommandLine.data(), MAX_CMDLINE, cmdLineBuilder.c_str(), cmdLineBuilder.size());

        if (!CreateProcess(
                m_ImageFilePath.c_str(),
                szCommandLine.data(),
                NULL,
                NULL,
                TRUE,
                dwCreationFlags,
                NULL,
                NULL,
                &m_si,
                &m_pi))
        {
            log::Error(
                _L_,
                hr = HRESULT_FROM_WIN32(GetLastError()),
                L"Could not start \"%s\" with command line \"%s\"\r\n",
                m_ImageFilePath.c_str(),
                szCommandLine.data());
            return hr;
        }
    }

    if (!m_DumpFilePath.empty())
    {
        wstring dumpKeyword(m_Keyword.c_str());
        dumpKeyword.append(L".dmp");
        log::Verbose(_L_, L"Attaching debugger to %s\r\n", m_Keyword.c_str());
        m_pDebugger = DebugAgent::DebugProcess(_L_, m_pi.dwProcessId, m_DumpFilePath, dumpKeyword);
        if (m_pDebugger)
        {
            log::Verbose(_L_, L"Debugger attached to %s\r\n", m_Keyword.c_str());
        }
    }

    if (job.IsValid())
    {
        if (!AssignProcessToJobObject(job.GetHandle(), m_pi.hProcess))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            log::Error(_L_, hr, L"Could not assign process \"%s\" to job object \r\n", m_Keyword.c_str());
            TerminateProcess(m_pi.hProcess, (UINT)-1);
            return hr;
        }
    }

    if (ResumeThread(m_pi.hThread) == -1)
    {
        HRESULT hr2 = HRESULT_FROM_WIN32(GetLastError());
        log::Error(_L_, hr2, L"Failed to resume process \"%s\"\r\n", m_Keyword.c_str());
        TerminateProcess(m_pi.hProcess, (UINT)-1);
        return hr2;
    }

    WaitForInputIdle(m_pi.hProcess, 1000);

    for_each(m_Redirections.begin(), m_Redirections.end(), [](const shared_ptr<ProcessRedirect>& item) {
        item->ChildConnected();
    });

    EvaluateRedirectionsStatus();

    SetStatus(Started);
    return S_OK;
}

HRESULT CommandExecute::WaitCompletion(DWORD dwTimeOut)
{
    if (m_pi.hProcess == 0L || m_pi.hProcess == INVALID_HANDLE_VALUE)
        return S_FALSE;

    bool bCompleted = false;
    DWORD dwCount = 0;

    while (!bCompleted && dwCount < 20)
    {
        DWORD retval = WaitForSingleObjectEx(m_pi.hProcess, (dwTimeOut == INFINITE ? 500 : dwTimeOut / 20), TRUE);

        switch (retval)
        {
            case WAIT_TIMEOUT:
                if (dwTimeOut != INFINITE)
                    dwCount++;
                break;
            case WAIT_OBJECT_0:
                bCompleted = true;
                if (!m_Redirections.empty())
                {
                    for_each(
                        m_Redirections.begin(),
                        m_Redirections.end(),
                        [&bCompleted](const shared_ptr<ProcessRedirect>& item) {
                            if (item->Status() > ProcessRedirect::PipeCreated
                                && item->Status() < ProcessRedirect::Complete)
                                bCompleted = false;
                        });
                }
                break;
            default:
                break;
        }
    }
    EvaluateRedirectionsStatus();
    if (bCompleted)
        SetStatus(Complete);
    if (!GetExitCodeProcess(m_pi.hProcess, &m_dwExitCode))
        return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

ProcessRedirect::RedirectStatus CommandExecute::EvaluateRedirectionsStatus()
{
    ProcessRedirect::RedirectStatus status = ProcessRedirect::Closed;

    std::for_each(
        m_Redirections.cbegin(), m_Redirections.cend(), [&status](const std::shared_ptr<ProcessRedirect>& item) {
            if (item)
                status = min(status, item->Status());
        });

    return m_RedirectStatus = status;
}

bool CommandExecute::HasCompleted()
{
    if (m_pi.hProcess == 0L || m_pi.hProcess == INVALID_HANDLE_VALUE)
        return false;

    if (m_RedirectStatus >= ProcessRedirect::Complete && m_Status >= CommandExecute::Complete)
        return true;

    if (m_Status >= CommandExecute::Complete)
    {
        EvaluateRedirectionsStatus();

        if (m_RedirectStatus > ProcessRedirect::Complete)
            return true;
    }
    return false;
}

HRESULT CommandExecute::CompleteExecution(ArchiveMessage::ITarget* pCab)
{
    // This must be executed only once...
    if (m_Status != Complete)
        return S_OK;

    for_each(
        m_Redirections.begin(), m_Redirections.end(), [](const shared_ptr<ProcessRedirect>& item) { item->Close(); });
    EvaluateRedirectionsStatus();

    if (m_pDebugger && pCab != nullptr)
    {
        std::for_each(
            m_pDebugger->GetDumpList().cbegin(), m_pDebugger->GetDumpList().cend(), [this, pCab](const wstring& dump) {
                HRESULT hr = E_FAIL;
                WCHAR szDumpFileName[MAX_PATH];

                if (FAILED(hr = GetFileNameForFile(dump.c_str(), szDumpFileName, MAX_PATH)))
                {
                    log::Error(_L_, hr, L"Could not deduce file name from path %s\r\n", dump.c_str());
                }
                else
                {
                    m_OnCompleteActions.push_back(
                        make_shared<OnComplete>(OnComplete::ArchiveAndDelete, szDumpFileName, dump, pCab));
                }
            });
    }

    std::for_each(m_OnCompleteActions.begin(), m_OnCompleteActions.end(), [this](const shared_ptr<OnComplete>& action) {
        switch (action->GetObjectType())
        {
            case OnComplete::Directory:
            {
                switch (action->GetAction())
                {
                    case OnComplete::Archive:
                    case OnComplete::ArchiveAndDelete:

                        if (action->ArchiveTarget() != nullptr)
                        {
                            if (SUCCEEDED(VerifyDirectoryExists(action->Fullpath().c_str())))
                            {
                                auto cab_request = ArchiveMessage::MakeAddDirectoryRequest(
                                    action->Name(),
                                    action->Fullpath(),
                                    action->MatchPattern(),
                                    true,
                                    0L,
                                    action->DeleteWhenDone());
                                Concurrency::send(action->ArchiveTarget(), cab_request);
                            }
                            else
                            {
                                log::Error(
                                    _L_,
                                    HRESULT_FROM_WIN32(GetLastError()),
                                    L"no directory to cab for path %s, ignored\r\n",
                                    action->Fullpath().c_str());
                            }
                        }
                        break;
                    case OnComplete::Delete:
                    {
                        if (!action->Fullpath().empty())
                        {
                            if (!RemoveDirectory(action->Fullpath().c_str()))
                            {
                                log::Error(
                                    _L_,
                                    HRESULT_FROM_WIN32(GetLastError()),
                                    L"Failed to delete directory %s\r\n",
                                    action->Fullpath().c_str());
                            }
                            else
                            {
                                log::Verbose(_L_, L"Successfully deleted file %s\r\n", action->Fullpath().c_str());
                            }
                        }
                    }
                    break;
                    default:
                        break;
                }
            }
            break;
            case OnComplete::File:
            {
                switch (action->GetAction())
                {
                    case OnComplete::Archive:
                    case OnComplete::ArchiveAndDelete:

                        if (action->ArchiveTarget() != nullptr)
                        {
                            if (SUCCEEDED(VerifyFileExists(action->Fullpath().c_str())))
                            {
                                auto cab_request = ArchiveMessage::MakeAddFileRequest(
                                    action->Name(), action->Fullpath(), true, 0L, action->DeleteWhenDone());
                                Concurrency::send(action->ArchiveTarget(), cab_request);
                            }
                            else
                            {
                                log::Error(
                                    _L_,
                                    HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
                                    L"no file to cab for path %s, ignored\r\n",
                                    action->Fullpath().c_str());
                            }
                        }
                        break;
                    case OnComplete::Delete:
                    {
                        if (!action->Fullpath().empty())
                        {
                            if (!DeleteFile(action->Fullpath().c_str()))
                            {
                                log::Error(
                                    _L_,
                                    HRESULT_FROM_WIN32(GetLastError()),
                                    L"Failed to delete file %s\r\n",
                                    action->Fullpath().c_str());
                            }
                            else
                            {
                                log::Verbose(_L_, L"Successfully deleted file %s\r\n", action->Fullpath().c_str());
                            }
                        }
                    }
                    break;
                    default:
                        break;
                }
            }
            break;
            case OnComplete::Stream:
            {
                switch (action->GetAction())
                {
                    case OnComplete::Archive:
                    case OnComplete::ArchiveAndDelete:
                        if (action->ArchiveTarget() != nullptr)
                        {
                            HRESULT hr = E_FAIL;
                            if (FAILED(hr = action->GetStream()->SetFilePointer(0L, FILE_BEGIN, NULL)))
                            {
                                log::Error(_L_, hr, L"Failed to reset stream before adding it to cab!\r\n");
                            }
                            auto cab_request =
                                ArchiveMessage::MakeAddStreamRequest(action->Name(), action->GetStream(), true, 0L);
                            Concurrency::send(action->ArchiveTarget(), cab_request);
                        }
                        else
                        {
                            log::Error(
                                _L_,
                                E_INVALIDARG,
                                L"Cab action planned and no Cab agent available, no cab addition for file %s\r\n",
                                action->Fullpath().c_str());
                        }
                        break;
                    case OnComplete::Delete:
                        if (action->GetStream() != nullptr)
                        {
                            action->GetStream()->Close();
                        }
                        break;
                }
                break;
            }
            default:
            {
                switch (action->GetAction())
                {
                    case OnComplete::FlushArchiveQueue:
                    {
                        auto archive_request = ArchiveMessage::MakeFlushQueueRequest();
                        Concurrency::send(action->ArchiveTarget(), archive_request);
                    }
                    break;
                    default:
                        break;
                }
            }
        }
        action->CancelTerminationHandler();
    });

    CloseHandle(m_pi.hProcess);
    m_pi.hProcess = INVALID_HANDLE_VALUE;
    CloseHandle(m_pi.hThread);
    m_pi.hThread = INVALID_HANDLE_VALUE;

    SetStatus(Closed);

    return S_OK;
}

CommandExecute::~CommandExecute(void) {}
