#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace f4ffmpeg
{
    // Default timeout applied when resolving a network URL via yt-dlp.
    // yt-dlp's own network/HTTP timeouts (default 300s) far exceed this;
    // anything taking longer than this on the first URL is effectively
    // stuck and we bail out.
    constexpr std::chrono::seconds kUrlResolveTimeout{20u};

    // Cookie source modes (Streaming.CookieSource).
    constexpr int kCookieSourceNone = 0;              // no cookies
    constexpr int kCookieSourceFromBrowser = 1;       // --cookies-from-browser <value>
    constexpr int kCookieSourceFile = 2;              // --cookies <value>

    // Resolves a network URL (YouTube, Twitch, etc.) to a direct media stream URL or local file path.
    // Currently relies on yt-dlp. Returns std::nullopt on failure, spawn error,
    // or timeout. The underlying yt-dlp process is terminated on timeout.
    std::optional<std::filesystem::path>
    resolveUrl(
        const std::string& url,
        int cookieSource = kCookieSourceNone,
        const std::string& cookieData = "",
        std::chrono::seconds timeout = kUrlResolveTimeout
    );
}
