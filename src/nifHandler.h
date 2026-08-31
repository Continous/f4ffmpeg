#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "playbackPolicy.h"

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

        const std::string& getPlaybackKey() const noexcept
        {
            return playbackKey;
        }

        // Internal lazy-activation handoff. Safe to call from the activation
        // worker while render threads concurrently query presentation state.
        void attachPlayback(
            std::shared_ptr<manager> newPlayback
        );

        std::shared_ptr<const producedFrame>
        getLatestFrame() const;

        transitionPresentation
        getTransitionPresentation() const;

    private:
        friend std::shared_ptr<const videoTarget>
        getVideoTargetForTexture(const char* texturePath);


        videoTargetMode mode =
            videoTargetMode::vanillaOverride;

        std::string texturePath;
        std::string videoPath;
        std::string playbackKey;
        videoPlaybackSettings playbackSettings;

        std::atomic_bool activationRequestIssued = false;

        mutable std::shared_mutex playbackMutex;
        std::shared_ptr<manager> playback;
    };

    // Resolves a texture against the immutable loose-video index built during
    // initializeNifHandler(). No filesystem work or manager creation occurs here.
    std::optional<std::string>
    getVideoPathForTexture(
        const char* texturePath
    );

    // Returns the target associated with a texture and queues that playback
    // definition for lazy asynchronous activation. Until its manager is ready,
    // getLatestFrame() returns nullptr and Fallout's vanilla presentation remains
    // active. No decoder initialization occurs on the calling/render thread.
    std::shared_ptr<const videoTarget>
    getVideoTargetForTexture(
        const char* texturePath
    );

    // Requires Fallout graphics to already be initialized. Scans supported
    // loose FFmpeg video containers plus standalone playlist INIs beneath
    // Data\Video, builds both vanilla and *_video.dds mappings,
    // and installs BSShaderTextureSet plus BSEffectShaderProperty base-texture
    // discovery adapters feeding the common D3D11 presentation hook. No
    // decoder/producer managers are started here.
    bool initializeNifHandler();

    // Arms lazy manager activation after a successful game load (F4SE
    // kPostLoadGame). This does not start any decoder by itself. Playback
    // definitions are activated asynchronously only after a mapped texture is
    // actually requested. The historical name is retained for source
    // compatibility with the existing main.cpp post-load call site.
    bool dispatchVideoManagers();
}
