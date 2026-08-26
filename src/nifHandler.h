#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace f4ffmpeg
{
    class manager;
    struct producedFrame;

    enum class videoTargetMode : std::uint8_t
    {
        vanillaOverride,
        directTextureSwap
    };

    struct videoPlaybackSettings
    {
        // Preserve the original f4ffmpeg behavior when no sidecar INI exists.
        bool looping = true;
        bool shuffle = false;
        std::vector<std::string> playlist;
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

        const videoPlaybackSettings& getPlaybackSettings() const noexcept
        {
            return playbackSettings;
        }

        bool isLooping() const noexcept
        {
            return playbackSettings.looping;
        }

        bool isShuffleEnabled() const noexcept
        {
            return playbackSettings.shuffle;
        }

        const std::vector<std::string>& getPlaylist() const noexcept
        {
            return playbackSettings.playlist;
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
        videoPlaybackSettings playbackSettings;

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

    // Requires Fallout graphics to already be initialized. Scans supported
    // loose FFmpeg video containers beneath Data\Video, builds both vanilla
    // and *_video.dds mappings,
    // and installs BSShaderTextureSet plus BSEffectShaderProperty base-texture
    // discovery adapters feeding the common D3D11 presentation hook. No
    // decoder/producer managers are started here.
    bool initializeNifHandler();

    // Starts one manager per indexed physical video using its parsed playback policy. Intended to be
    // called only after a successful game load (F4SE kPostLoadGame). Safe to
    // call more than once; already-running videos are reused and failed starts
    // may be retried by a later call.
    bool dispatchVideoManagers();
}
