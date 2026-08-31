#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace f4ffmpeg
{
    struct producedFrame;

    // Decoder-gap presentation policy. "image" is intentionally accepted only
    // from playlist metadata; the global configuration parser does not expose it.
    enum class transitionMethod : std::uint8_t
    {
        vanillaDDS,
        holdLastFrame,
        blackFrame,
        image
    };

    struct videoPlaybackSettings
    {
        // Preserve original f4ffmpeg behavior when no sidecar INI exists.
        bool looping = true;
        bool shuffle = false;
        std::vector<std::string> playlist;

        // Playlist-local transition override. The global permission switch is
        // evaluated by manager when a decoder source transition begins.
        std::optional<transitionMethod> transition;
        std::optional<std::string> transitionImage;
    };

    struct transitionPresentation
    {
        bool active = false;
        transitionMethod method = transitionMethod::vanillaDDS;
        std::shared_ptr<const producedFrame> frame;
    };

    std::optional<transitionMethod>
    parseTransitionMethod(
        std::string_view value,
        bool allowImage
    );

    const char* transitionMethodName(
        transitionMethod method
    ) noexcept;
}
