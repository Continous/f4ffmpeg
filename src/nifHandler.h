#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace f4ffmpeg
{
    class manager;
    struct producedFrame;

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

    // Resolves a texture against the immutable loose-video index built during
    // initializeNifHandler(). No filesystem work or manager creation occurs here.
    std::optional<std::string>
    getVideoPathForTexture(
        const char* texturePath
    );

    // Returns the already-dispatched target associated with a texture.
    // Managers are created while Data\\Video is indexed, not from a NIF hook.
    std::shared_ptr<const videoTarget>
    getVideoTargetForTexture(
        const char* texturePath
    );

    // Requires Fallout graphics to already be initialized. Scans loose .mov
    // files beneath Data\\Video, dispatches one looping manager per physical
    // video, builds both vanilla and *_video.dds mappings, and installs the
    // BSShaderTextureSet discovery hooks plus D3D11 presentation hook.
    bool initializeNifHandler();
}
