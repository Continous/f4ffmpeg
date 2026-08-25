#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
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

        friend bool dispatchVideoManagers();

        videoTargetMode mode =
            videoTargetMode::vanillaOverride;

        std::string texturePath;
        std::string videoPath;

        mutable std::shared_mutex playbackMutex;
        std::shared_ptr<manager> playback;
    };

    // Resolves a texture against the immutable loose-video index built during
    // initializeNifHandler(). No filesystem work or manager creation occurs here.
    std::optional<std::string>
    getVideoPathForTexture(
        const char* texturePath
    );

    // Returns the target associated with a texture. The target may exist before
    // its manager is dispatched; until then getLatestFrame() returns nullptr and
    // Fallout's vanilla presentation remains active.
    std::shared_ptr<const videoTarget>
    getVideoTargetForTexture(
        const char* texturePath
    );

    // Requires Fallout graphics to already be initialized. Scans loose .mov
    // files beneath Data\Video, builds both vanilla and *_video.dds mappings,
    // and installs the BSShaderTextureSet discovery hooks plus D3D11 presentation
    // hook. No decoder/producer managers are started here.
    bool initializeNifHandler();

    // Starts one looping manager per indexed physical video. Intended to be
    // called only after a successful game load (F4SE kPostLoadGame). Safe to
    // call more than once; already-running videos are reused and failed starts
    // may be retried by a later call.
    bool dispatchVideoManagers();
}
