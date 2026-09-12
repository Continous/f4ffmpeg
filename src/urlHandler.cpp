#include "pch.h"
#include "urlHandler.h"

#include <cstdint>
#include <string>
#include <filesystem>

#include <REX/LOG.h>

#include <windows.h>

namespace f4ffmpeg
{
    // Escapes a string for safe embedding inside a double-quoted argument
    // of a `cmd.exe` command line. Sufficient for well-formed URLs from
    // INI values (no `|`, `;`, backticks).
    static std::string ShellQuote(const std::string& value)
    {
        std::string quoted;
        quoted.reserve(value.size() + 2);
        quoted.push_back('"');
        for (const auto c : value)
        {
            if (c == '"' || c == '\\')
                quoted.push_back('\\');
            quoted.push_back(c);
        }
        quoted.push_back('"');
        return quoted;
    }

    static HANDLE
    createPipe(HANDLE writeEnd)
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;

        HANDLE readEnd = nullptr;
        if (!::CreatePipe(&readEnd, &writeEnd, &sa, 0u))
            return nullptr;
        ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0u);
        return readEnd;
    }

    // Drains a pipe handle into a string. Only safe once the child has
    // exited (or been terminated) and closed its write side.
    static void
    drainPipe(HANDLE readEnd, std::string& out)
    {
        char buffer[8192];
        DWORD bytesRead = 0;
        while (::ReadFile(readEnd, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead != 0u)
            out.append(buffer, bytesRead);
    }

    // Spawns `cmd.exe /d /c yt-dlp ...`, applying the configured cookie
    // mode. Captures stdout and stderr separately. Returns false if the
    // process could not be spawned.
    static bool
    spawnYtDlp(
        const std::string& url,
        int cookieSource,
        const std::string& cookieData,
        std::chrono::seconds timeout,
        std::string& stdoutOut,
        std::string& stderrOut
    )
    {
        std::string command =
            "yt-dlp -g -f best --get-url " + ShellQuote(url);

        if (cookieSource == kCookieSourceFromBrowser ||
            cookieSource == kCookieSourceFile)
        {
            if (cookieData.empty())
            {
                REX::DEBUG(
                    "urlHandler - cookie source {} configured but CookieValue is empty; proceeding without cookies",
                    cookieSource
                );
            }
            else
            {
                command +=
                    (cookieSource == kCookieSourceFromBrowser
                        ? " --cookies-from-browser "
                        : " --cookies ") +
                    ShellQuote(cookieData);
            }
        }
        else if (cookieSource != kCookieSourceNone)
        {
            REX::DEBUG(
                "urlHandler - unknown cookie source {}; treating as no cookies",
                cookieSource
            );
        }

        const std::string cmdLine = "cmd.exe /d /c " + command;

        HANDLE outWrite = nullptr;
        HANDLE errWrite = nullptr;
        HANDLE outRead = createPipe(outWrite);
        if (!outRead)
            return false;
        HANDLE errRead = createPipe(errWrite);
        if (!errRead)
        {
            ::CloseHandle(outRead);
            ::CloseHandle(outWrite);
            return false;
        }

        STARTUPINFOA si{};
        si.cb = sizeof(STARTUPINFOA);
        si.hStdOutput = outWrite;
        si.hStdError = errWrite;
        si.dwFlags = STARTF_USESTDHANDLES;

        PROCESS_INFORMATION pi{};
        const BOOL created = ::CreateProcessA(
            "cmd.exe",
            const_cast<char*>(cmdLine.data()),
            nullptr,
            nullptr,
            TRUE,
            0u,
            nullptr,
            nullptr,
            &si,
            &pi
        );

        if (!created)
        {
            REX::WARN(
                "urlHandler - failed to spawn cmd.exe for: {}",
                command
            );
            ::CloseHandle(outRead);
            ::CloseHandle(outWrite);
            ::CloseHandle(errRead);
            ::CloseHandle(errWrite);
            return false;
        }

        const DWORD waitResult =
            ::WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout.count()));
        const bool timedOut = (waitResult == WAIT_TIMEOUT);
        if (timedOut)
            ::TerminateProcess(pi.hProcess, 1u);

        DWORD exitCode = 0;
        ::GetExitCodeProcess(pi.hProcess, &exitCode);

        drainPipe(outRead, stdoutOut);
        drainPipe(errRead, stderrOut);

        ::CloseHandle(outRead);
        ::CloseHandle(outWrite);
        ::CloseHandle(errRead);
        ::CloseHandle(errWrite);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);

        if (timedOut)
            REX::WARN(
                "urlHandler - yt-dlp timed out after {}s and was terminated",
                timeout.count()
            );

        return true;
    }

    std::optional<std::filesystem::path>
    resolveUrl(
        const std::string& url,
        int cookieSource,
        const std::string& cookieData,
        std::chrono::seconds timeout
    )
    {
        std::string stdoutOut;
        std::string stderrOut;
        if (!spawnYtDlp(url, cookieSource, cookieData, timeout, stdoutOut, stderrOut))
            return std::nullopt;

        // Full trace of what the process produced, success or failure.
        REX::TRACE(
            "urlHandler - yt-dlp output for '{}':\nSTDOUT:\n{}\nSTDERR:\n{}",
            url,
            stdoutOut,
            stderrOut
        );

        // Extract the first non-whitespace line of stdout.
        std::string firstUrl = stdoutOut;
        const auto begin = firstUrl.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
        {
            if (stderrOut.find("not recognized") != std::string::npos)
                REX::WARN("urlHandler - yt-dlp not found/valid on PATH");
            else
                REX::DEBUG(
                    "urlHandler - yt-dlp returned no direct URL for '{}'",
                    url
                );
            return std::nullopt;
        }

        const auto end = firstUrl.find_last_not_of(" \t\r\n");
        firstUrl = firstUrl.substr(begin, end - begin + 1);

        const auto firstNewline = firstUrl.find('\n');
        if (firstNewline != std::string::npos)
            firstUrl = firstUrl.substr(0, firstNewline);

        return std::filesystem::path(firstUrl);
    }
}
