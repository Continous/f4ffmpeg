#include "pch.h"
#include "urlHandler.h"
#include "config.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <REX/LOG.h>

#include <windows.h>

namespace f4ffmpeg
{
    // Quotes a single argument per the MSVCRT command-line parsing rules.
    // This is used only for arguments controlled by f4ffmpeg itself.
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

    // Locates yt-dlp.exe.
    //
    // Resolution order:
    //   1. Streaming.YtDlpPath, when configured and present
    //   2. Fallout 4's process directory
    //   3. The process PATH
    //
    // Returns an empty string if nothing was found.
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
            ::SearchPathA(
                nullptr,
                "yt-dlp.exe",
                nullptr,
                MAX_PATH,
                searchBuffer,
                nullptr
            );

        if (found != 0u)

            REX::TRACE("yt-dlp found. {}", searchBuffer)

            return searchBuffer;

        return {};
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

        // The parent must retain the read end, while only the child inherits
        // the write end.
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

    // Continuously drains a pipe into a string.
    //
    // This runs concurrently with yt-dlp so that stdout/stderr pipe buffers
    // cannot fill and deadlock the child process.
    static void
    drainPipe(HANDLE readEnd, std::string& out)
    {
        char buffer[8192];
        DWORD bytesRead = 0;

        while (
            ::ReadFile(
                readEnd,
                buffer,
                sizeof(buffer),
                &bytesRead,
                nullptr
            ) &&
            bytesRead != 0u)
        {
            out.append(buffer, bytesRead);
        }
    }

    // Spawns yt-dlp directly without cmd.exe.
    //
    // f4ffmpeg always supplies --get-url.
    // Streaming.YtDlpFlags is appended verbatim so the user can control all
    // other yt-dlp behavior, including format selection and JS runtimes.
    static bool
    spawnYtDlp(
        const std::string& exePath,
        const std::string& url,
        int cookieSource,
        const std::string& cookieData,
        const std::string& ytDlpFlags,
        std::chrono::seconds timeout,
        std::string& stdoutOut,
        std::string& stderrOut
    )
    {

        REX::TRACE("Spawning yt-dlp...")

        std::string command =
            QuoteArg(exePath) + " --get-url";

        // User-provided yt-dlp arguments are deliberately not parsed or
        // re-quoted here. This preserves yt-dlp's normal command-line syntax.
        if (!ytDlpFlags.empty())
        {
            command += " ";
            command += ytDlpFlags;
        }

        if (cookieSource == kCookieSourceFromBrowser ||
            cookieSource == kCookieSourceFile)
        {
            if (cookieData.empty())
            {
                REX::DEBUG(
                    "urlHandler - cookie source {} configured but "
                    "CookieValue is empty; proceeding without cookies",
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

        // Keep the URL as the final positional argument.
        command += " ";
        command += QuoteArg(url);

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
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = outWrite;
        si.hStdError = errWrite;
        si.dwFlags = STARTF_USESTDHANDLES;

        PROCESS_INFORMATION pi{};

        std::vector<char> cmdLineBuf(
            command.begin(),
            command.end()
        );
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

        // The child inherited the write ends. The parent must close its copies
        // immediately, otherwise the read side will never observe EOF while
        // the parent is still holding a writer.
        ::CloseHandle(outWrite);
        outWrite = nullptr;

        ::CloseHandle(errWrite);
        errWrite = nullptr;

        // Drain both streams concurrently while yt-dlp runs.
        std::thread stdoutThread([&]() {
            drainPipe(outRead, stdoutOut);
        });

        std::thread stderrThread([&]() {
            drainPipe(errRead, stderrOut);
        });

        const DWORD waitTimeout =
            static_cast<DWORD>(
                std::chrono::duration_cast<
                    std::chrono::milliseconds
                >(timeout).count()
            );

        const DWORD waitResult =
            ::WaitForSingleObject(
                pi.hProcess,
                waitTimeout
            );

        const bool timedOut =
            waitResult == WAIT_TIMEOUT;

        if (timedOut)
        {
            REX::WARN(
                "urlHandler - yt-dlp timed out after {}s and will be terminated",
                timeout.count()
            );

            ::TerminateProcess(
                pi.hProcess,
                1u
            );

            // Ensure the process has actually exited before waiting for the
            // pipe-draining threads to finish.
            ::WaitForSingleObject(
                pi.hProcess,
                INFINITE
            );
        }

        // At this point yt-dlp has exited (normally or forcibly), so both
        // reader threads can finish once their pipe reaches EOF.
        stdoutThread.join();
        stderrThread.join();

        DWORD exitCode = 0;

        if (!::GetExitCodeProcess(
                pi.hProcess,
                &exitCode))
        {
            REX::WARN(
                "urlHandler - failed to obtain yt-dlp exit code"
            );
        }
        else
        {
            REX::DEBUG(
                "urlHandler - yt-dlp exited with code {}",
                exitCode
            );
        }

        ::CloseHandle(outRead);
        ::CloseHandle(errRead);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);

        return !timedOut;
    }

    std::optional<std::string>
    resolveUrl(
        const std::string& url,
        int cookieSource,
        const std::string& cookieData,
        std::chrono::seconds timeout
    )
    {
        const std::string exePath =
            findYtDlp(config::ytDlpPath.GetValue());

        if (exePath.empty())
        {
            REX::WARN(
                "urlHandler - could not locate yt-dlp.exe"
            );

            return std::nullopt;
        }

        std::string stdoutOut;
        std::string stderrOut;

        if (!spawnYtDlp(
                exePath,
                url,
                cookieSource,
                cookieData,
                config::ytDlpFlags.GetValue(),
                timeout,
                stdoutOut,
                stderrOut))
        {
            return std::nullopt;
        }

        // Full trace of what the process produced, success or failure.
        REX::TRACE(
            "urlHandler - yt-dlp output for '{}':\n"
            "STDOUT:\n{}\n"
            "STDERR:\n{}",
            url,
            stdoutOut,
            stderrOut
        );

        // Extract the first non-whitespace line of stdout.
        std::string firstUrl = stdoutOut;

        const auto begin =
            firstUrl.find_first_not_of(" \t\r\n");

        if (begin == std::string::npos)
        {
            if (stderrOut.find("not recognized") != std::string::npos)
            {
                REX::WARN(
                    "urlHandler - yt-dlp not found/valid on PATH"
                );
            }
            else
            {
                REX::DEBUG(
                    "urlHandler - yt-dlp returned no direct URL for '{}'",
                    url
                );
            }

            return std::nullopt;
        }

        const auto end =
            firstUrl.find_last_not_of(" \t\r\n");

        firstUrl =
            firstUrl.substr(
                begin,
                end - begin + 1
            );

        const auto firstNewline =
            firstUrl.find('\n');

        if (firstNewline != std::string::npos)
            firstUrl = firstUrl.substr(0, firstNewline);

        return firstUrl;
    }
}
