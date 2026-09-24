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
            return searchBuffer;

        return {};
    }

    // Truncates process output for a diagnostic error message so a chatty
    // yt-dlp run does not flood the log, while still preserving the
    // human-readable reason it failed.
    static std::string
    truncateForLog(std::string_view text, std::size_t maxLen = 2000)
    {
        std::string value(text);

        // Strip a trailing newline so the message ends cleanly.
        while (
            !value.empty() &&
            (value.back() == '\n' || value.back() == '\r'))
        {
            value.pop_back();
        }

        value = value.substr(0, maxLen);

        if (value.empty())
            value = "(no output)";

        return value;
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
        std::string& stderrOut,
        DWORD& exitCodeOut
    )
    {
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
            // Force-killed; there is no meaningful exit code.
            exitCodeOut = 1u;
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
            exitCodeOut = exitCode;

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
            REX::ERROR(
                "urlHandler - could not locate yt-dlp.exe. Set "
                "Streaming.YtDlpPath to the yt-dlp.exe location or place it "
                "on PATH / next to Fallout4.exe. This is required for "
                "streaming YouTube sources."
            );

            return std::nullopt;
        }

        std::string stdoutOut;
        std::string stderrOut;

        DWORD exitCode = 0;

        if (!spawnYtDlp(
                exePath,
                url,
                cookieSource,
                cookieData,
                config::ytDlpFlags.GetValue(),
                timeout,
                stdoutOut,
                stderrOut,
                exitCode))
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
            // yt-dlp produced no usable media URL. This is the single most
            // common cause of "Format lrc detected" / "uninitialized Video
            // Decoder" errors further down the pipeline, so the reason it
            // failed must be surfaced clearly and cannot be left at DEBUG.
            const bool botBlocked =
                stderrOut.find("Sign in to confirm you're not a bot") !=
                    std::string::npos ||
                stderrOut.find("Sign in to confirm your age") !=
                    std::string::npos ||
                stderrOut.find("This video is unavailable") !=
                    std::string::npos;

            const bool noJsRuntime =
                stderrOut.find("deno") != std::string::npos ||
                stderrOut.find("js-runtime") != std::string::npos;

            if (botBlocked)
            {
                REX::ERROR(
                    "urlHandler - YouTube rejected the request for '{}' "
                    "(bot/age-check protection). Playback will not start. "
                    "Try providing cookies via Streaming.CookieSource / "
                    "Streaming.CookieValue, or a different YtDlpFlags.",
                    url
                );
            }
            else if (noJsRuntime)
            {
                REX::ERROR(
                    "urlHandler - yt-dlp could not resolve '{}' because it "
                    "needs a JS runtime (deno). Install deno on PATH or "
                    "configure Streaming.YtDlpFlags with a working "
                    "--js-runtimes value.",
                    url
                );
            }
            else if (stderrOut.find("not recognized") != std::string::npos)
            {
                REX::ERROR(
                    "urlHandler - yt-dlp is not found or is not a valid "
                    "executable on PATH for '{}'. Set Streaming.YtDlpPath to "
                    "the yt-dlp.exe location.",
                    url
                );
            }
            else
            {
                REX::ERROR(
                    "urlHandler - yt-dlp failed to resolve '{}' "
                    "(exit code {}): {}",
                    url,
                    exitCode,
                    truncateForLog(stderrOut)
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
