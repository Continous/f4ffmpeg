#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace f4ffmpeg
{
    class manager;
    struct producedFrame;

    // How f4ffmpeg supersedes the vanilla texture presentation.
    //
    // vanillaOverride:
    //   A normal vanilla texture has matching f4ffmpeg content. Vanilla is
    //   allowed to load/run normally (including flipbook controllers), while
    //   f4ffmpeg becomes the preferred presentation when a produced frame is
    //   available. Flipbook UV neutralization is handled at the later
    //   presentation boundary.
    //
    // directTextureSwap:
    //   Explicit convenience path selected by an authored *_video.dds marker.
    //   No vanilla flipbook bypass is assumed; the produced frame may directly
    //   replace the returned texture once the NiTexture bridge is attached.
    enum class videoTargetMode : std::uint8_t
    {
        vanillaOverride,
        directTextureSwap
    };

    class videoTarget
    {
    public:
        videoTargetMode getMode() const noexcept
        {
            return mode;
        }

        const std::string& getTexturePath() const noexcept
        {
            return texturePath;
        }

        const std::string& getVideoPath() const noexcept
        {
            return videoPath;
        }

        std::shared_ptr<const producedFrame>
        getLatestFrame() const;

    private:
        friend std::shared_ptr<const videoTarget>
        getVideoTargetForTexture(const char* texturePath);

        videoTargetMode mode =
            videoTargetMode::vanillaOverride;

        std::string texturePath;
        std::string videoPath;

        std::shared_ptr<manager> playback;
    };

    // Resolve a DDS texture to an existing f4ffmpeg sidecar.
    //
    // Normal vanilla path:
    //   Textures\\Effects\\TVAnim\\PleaseStandByFull01_d.dds
    //       -> Data\\Video\\Effects\\TVAnim\\PleaseStandByFull01_d.mov
    //       -> vanillaOverride
    //
    // Explicit direct-swap convenience path:
    //   Textures\\foo\\screen_video.dds
    //       -> Data\\Video\\foo\\screen.mov
    //       -> directTextureSwap
    //
    // The movie must actually exist. Ordinary DDS files without a matching
    // sidecar remain completely vanilla.
    std::optional<std::string>
    getVideoPathForTexture(
        const char* texturePath
    );

    // Resolves and activates playback for f4ffmpeg content associated with a
    // vanilla texture. Playback is shared per unique movie path.
    std::shared_ptr<const videoTarget>
    getVideoTargetForTexture(
        const char* texturePath
    );

    // Installs BSShaderTextureSet texture-acquisition arbitration. Bethesda's
    // original acquisition always executes first. Safe to call more than once.
    bool initializeNifHandler();
}
