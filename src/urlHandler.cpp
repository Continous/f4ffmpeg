#include "pch.h"
#include "urlHandler.h"
#include "config.h"

#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>

#include <REX/LOG.h>

#include <windows.h>

namespace f4ffmpeg
{
    // Quotes a single argument per the MSVCRT command-line parsing rules
    // (runs of backslashes before a quote are folded so the quote is
    // preserved literally). The resolved yt-dlp.exe parses its own command
    // line with this convention, so this replaces the old cmd.exe shell.
    static std::string QuoteArg(const std::string& value)
    {
        std::string quoted;
        quoted.reserve(value.size() + 2);
        quoted.push_back('"');

        size_t backslashRun = 0;
        for (const char c : value)
        {
            if (c == '\\')
            {
                ++backslashRun;
                continue;
            }

            if (backslashRun != 0u)
                quoted.append(backslashRun, '\\');
            backslashRun = 0u;

            if (c == '"')
                quoted.push_back('\\');
            quoted.push_back(c);
        }
        if (backslashRun != 0u)
            quoted.append(backslashRun, '\\');

        quoted.push_back('"');
        return quoted;
    }

    // Locates yt-dlp.exe. Resolution order: configured Streaming.YtDlpPath
    // (when it exists on disk), then the game process directory, then the
    // process PATH. Empty string if nothing was found.
    static std::string
    findYtDlp(const std::string& configuredPath)
    {
        if (!configuredPath.empty())
        {
            if (std::filesystem::exists(configuredPath))
                return configuredPath;

            REX::WARN(
                "urlHandler - configured YtDlpPath {} does not exist",
                configuredPath
            );
        }

        char exePath[MAX_PATH]{};
        const DWORD exePathLen =
            ::GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (exePathLen != 0u && exePathLen < MAX_PATH)
        {
            const std::filesystem::path candidate(
                std::filesystem::path{exePath}.parent_path() / "yt-dlp.exe"
            );
            if (std::filesystem::exists(candidate))
                return candidate.string();
        }

        char searchBuffer[MAX_PATH]{};
        const DWORD found =
            ::SearchPathA(nullptr, "yt-dlp.exe", nullptr, MAX_PATH, searchBuffer, nullptr);
        if (found != 0u)
            return searchBuffer;

        return std::string{};
    }

    static HANDLE
    createPipe(HANDLE& writeEnd)
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;

        HANDLE readEnd = nullptr;

        if (!::CreatePipe(&readEnd, &writeEnd, &sa, 0u))
            return nullptr;

        if (!::SetHandleInformation(
                readEnd,
                HANDLE_FLAG_INHERIT,
                0u))
        {
            ::CloseHandle(readEnd);
            ::CloseHandle(writeEnd);
            writeEnd = nullptr;
            return nullptr;
        }

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

    // Spawns the resolved yt-dlp.exe directly (no cmd.exe wrapper), applying
    // the configured cookie mode. Captures stdout and stderr separately.
    // Returns false if the process could not be spawned.
    static bool
    spawnYtDlp(
        const std::string& exePath,
        const std::string& url,
        int cookieSource,
        const std::string& cookieData,
        std::chrono::seconds timeout,
        std::string& stdoutOut,
        std::string& stderrOut
    )
    {
        std::string command =
            QuoteArg(exePath) + " -g -f best " + QuoteArg(url);

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
                    QuoteArg(cookieData);
            }
        }
        else if (cookieSource != kCookieSourceNone)
        {
            REX::DEBUG(
                "urlHandler - unknown cookie source {}; treating as no cookies",
                cookieSource
            );
        }

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
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION pi{};
        std::vector<char> cmdLineBuf(command.begin(), command.end());
        cmdLineBuf.push_back('\0');

        const BOOL created = ::CreateProcessA(
            nullptr,
            cmdLineBuf.data(),
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
                "urlHandler - failed to spawn yt-dlp.exe for: {}",
                command
            );
            ::CloseHandle(outRead);
            ::CloseHandle(outWrite);
            ::CloseHandle(errRead);
            ::CloseHandle(errWrite);
            return false;
        }

        // The child has inherited the write ends; the parent must close its copies.
        ::CloseHandle(outWrite);
        outWrite = nullptr;
        ::CloseHandle(errWrite);
        errWrite = nullptr;

        // Drain stdout/stderr while yt-dlp is running so the pipe buffers
        // cannot fill and deadlock the child.
        std::thread stdoutThread([&]() {
            drainPipe(outRead, stdoutOut);
        });

        std::thread stderrThread([&]() {
            drainPipe(errRead, stderrOut);
        });

        const DWORD waitTimeout = static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()
        );

        const DWORD waitResult =
            ::WaitForSingleObject(pi.hProcess, waitTimeout);

        const bool timedOut = (waitResult == WAIT_TIMEOUT);

        if (timedOut)
        {
            ::TerminateProcess(pi.hProcess, 1u);
            ::WaitForSingleObject(pi.hProcess, INFINITE);
        }

        stdoutThread.join();
        stderrThread.join();

        DWORD exitCode = 0;
        ::GetExitCodeProcess(pi.hProcess, &exitCode);

        ::CloseHandle(outRead);
        ::CloseHandle(errRead);
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
        std::string exePath = findYtDlp(config::ytDlpPath);
        if (exePath.empty())
        {
            REX::WARN("urlHandler - could not locate yt-dlp.exe");
            return std::nullopt;
        }

        std::string stdoutOut;
        std::string stderrOut;
        if (!spawnYtDlp(exePath, url, cookieSource, cookieData, timeout, stdoutOut, stderrOut))
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
