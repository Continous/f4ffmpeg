#include "pch.h"
#include "nifHandler.h"
#include "manager.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <RE/B/BSShaderTextureSet.h>
#include <RE/B/BSTextureSet.h>
#include <RE/N/NiPointer.h>
#include <REL/Relocation.h>

namespace f4ffmpeg
{
    namespace
    {
        constexpr std::string_view dataPrefix =
            "data\\";

        constexpr std::string_view texturesPrefix =
            "textures\\";

        // This marker is now only an opt-in hint for the cheap raw texture
        // replacement path. It is not required for f4ffmpeg recognition.
        constexpr std::string_view directSwapSuffix =
            "_video.dds";

        constexpr std::string_view ddsSuffix =
            ".dds";

        constexpr std::string_view videoRoot =
            "Data\\Video\\";

        constexpr std::string_view videoExtension =
            ".mov";

        char asciiLower(char value)
        {
            if (
                value >= 'A' &&
                value <= 'Z')
            {
                return static_cast<char>(
                    value - 'A' + 'a'
                );
            }

            return value;
        }

        bool startsWithInsensitive(
            std::string_view value,
            std::string_view prefix)
        {
            if (value.size() < prefix.size())
                return false;

            for (
                std::size_t index = 0;
                index < prefix.size();
                ++index)
            {
                if (
                    asciiLower(value[index]) !=
                    asciiLower(prefix[index]))
                {
                    return false;
                }
            }

            return true;
        }

        bool endsWithInsensitive(
            std::string_view value,
            std::string_view suffix)
        {
            if (value.size() < suffix.size())
                return false;

            const auto offset =
                value.size() - suffix.size();

            for (
                std::size_t index = 0;
                index < suffix.size();
                ++index)
            {
                if (
                    asciiLower(value[offset + index]) !=
                    asciiLower(suffix[index]))
                {
                    return false;
                }
            }

            return true;
        }

        std::string lowercasePath(
            std::string_view path)
        {
            std::string result{path};

            std::transform(
                result.begin(),
                result.end(),
                result.begin(),
                [](char value)
                {
                    return asciiLower(value);
                }
            );

            return result;
        }

        std::string normalizeTexturePath(
            std::string_view texturePath)
        {
            std::string normalized{
                texturePath
            };

            std::replace(
                normalized.begin(),
                normalized.end(),
                '/',
                '\\'
            );

            while (
                !normalized.empty() &&
                normalized.front() == '\\')
            {
                normalized.erase(
                    normalized.begin()
                );
            }

            if (startsWithInsensitive(
                    normalized,
                    dataPrefix))
            {
                normalized.erase(
                    0,
                    dataPrefix.size()
                );
            }

            return normalized;
        }

        struct resolvedVideoTarget
        {
            videoTargetMode mode =
                videoTargetMode::vanillaOverride;

            std::string texturePath;
            std::string videoPath;
        };

        std::mutex resolutionCacheMutex;

        // Recognition now sees every base DDS path, so cache misses as well as
        // hits. A texture incurs at most one sidecar filesystem probe.
        std::unordered_map<
            std::string,
            std::optional<resolvedVideoTarget>>
            resolutionCache;

        std::optional<resolvedVideoTarget>
        resolveVideoTargetUncached(
            std::string normalized)
        {
            if (!startsWithInsensitive(
                    normalized,
                    texturesPrefix))
            {
                return std::nullopt;
            }

            videoTargetMode mode =
                videoTargetMode::vanillaOverride;

            std::size_t suffixSize = 0;

            if (endsWithInsensitive(
                    normalized,
                    directSwapSuffix))
            {
                mode =
                    videoTargetMode::directTextureSwap;

                suffixSize =
                    directSwapSuffix.size();
            }
            else if (endsWithInsensitive(
                         normalized,
                         ddsSuffix))
            {
                suffixSize =
                    ddsSuffix.size();
            }
            else
            {
                return std::nullopt;
            }

            const std::string authoredTexturePath =
                normalized;

            // Strip Textures\\ and either .dds or _video.dds. Everything
            // remaining becomes the path beneath Data\\Video.
            normalized.erase(
                0,
                texturesPrefix.size()
            );

            normalized.erase(
                normalized.size() - suffixSize
            );

            std::string videoPath{
                videoRoot
            };

            videoPath += normalized;
            videoPath += videoExtension;

            std::error_code error;

            if (!std::filesystem::is_regular_file(
                    videoPath,
                    error))
            {
                return std::nullopt;
            }

            return resolvedVideoTarget{
                mode,
                authoredTexturePath,
                std::move(videoPath)
            };
        }

        std::optional<resolvedVideoTarget>
        resolveVideoTarget(
            const char* texturePath)
        {
            if (
                texturePath == nullptr ||
                *texturePath == '\0')
            {
                return std::nullopt;
            }

            std::string normalized =
                normalizeTexturePath(
                    texturePath
                );

            const std::string cacheKey =
                lowercasePath(
                    normalized
                );

            {
                std::scoped_lock lock(
                    resolutionCacheMutex
                );

                if (const auto cached =
                        resolutionCache.find(
                            cacheKey
                        );
                    cached != resolutionCache.end())
                {
                    return cached->second;
                }
            }

            auto resolved =
                resolveVideoTargetUncached(
                    std::move(normalized)
                );

            {
                std::scoped_lock lock(
                    resolutionCacheMutex
                );

                resolutionCache.emplace(
                    cacheKey,
                    resolved
                );
            }

            return resolved;
        }

        std::mutex targetRegistryMutex;

        // One target per authored texture path. This intentionally preserves
        // mode even when two texture paths resolve to the same movie.
        std::unordered_map<
            std::string,
            std::shared_ptr<videoTarget>>
            targetsByTexture;

        // One decoder/producer manager per unique movie path.
        std::unordered_map<
            std::string,
            std::shared_ptr<manager>>
            playbackByVideo;

        using textureType =
            RE::BSShaderProperty::TextureTypeEnum;

        using getTexturePrefetched_t = void(*)(
            RE::BSShaderTextureSet*,
            const void*,
            textureType,
            RE::NiPointer<RE::NiTexture>*,
            bool
        );

        using getTexture_t = void(*)(
            RE::BSShaderTextureSet*,
            textureType,
            RE::NiPointer<RE::NiTexture>*,
            bool
        );

        REL::Relocation<getTexturePrefetched_t>
            originalGetTexturePrefetched;

        REL::Relocation<getTexture_t>
            originalGetTexture;

        const char* modeName(
            videoTargetMode mode)
        {
            switch (mode)
            {
                case videoTargetMode::vanillaOverride:
                    return "vanilla override";

                case videoTargetMode::directTextureSwap:
                    return "direct texture swap";
            }

            return "unknown";
        }

        void activateVideoReplacement(
            RE::BSShaderTextureSet* textureSet,
            textureType type)
        {
            if (
                textureSet == nullptr ||
                type != textureType::kBase)
            {
                return;
            }

            const char* texturePath =
                textureSet->GetTextureFilename(
                    type
                );

            // This resolves ordinary vanilla DDS paths as well as explicit
            // *_video.dds direct-swap markers. If no sidecar exists, nothing
            // happens and Bethesda's texture remains authoritative.
            (void)getVideoTargetForTexture(
                texturePath
            );
        }

        void getTexturePrefetchedHook(
            RE::BSShaderTextureSet* textureSet,
            const void* prefetchedHandle,
            textureType type,
            RE::NiPointer<RE::NiTexture>* texture,
            bool srgb)
        {
            // Vanilla always runs first. f4ffmpeg is an overriding method,
            // never a replacement for NIF loading itself.
            originalGetTexturePrefetched(
                textureSet,
                prefetchedHandle,
                type,
                texture,
                srgb
            );

            activateVideoReplacement(
                textureSet,
                type
            );
        }

        void getTextureHook(
            RE::BSShaderTextureSet* textureSet,
            textureType type,
            RE::NiPointer<RE::NiTexture>* texture,
            bool srgb)
        {
            // Vanilla always runs first. If matching f4ffmpeg content exists,
            // our target is activated afterward and becomes the preferred
            // presentation once its produced frame is bindable.
            originalGetTexture(
                textureSet,
                type,
                texture,
                srgb
            );

            activateVideoReplacement(
                textureSet,
                type
            );
        }
    }

    std::shared_ptr<const producedFrame>
    videoTarget::getLatestFrame() const
    {
        if (!playback)
            return nullptr;

        return playback->getLatestFrame();
    }

    std::optional<std::string>
    getVideoPathForTexture(
        const char* texturePath)
    {
        const auto resolved =
            resolveVideoTarget(
                texturePath
            );

        if (!resolved)
            return std::nullopt;

        return resolved->videoPath;
    }

    std::shared_ptr<const videoTarget>
    getVideoTargetForTexture(
        const char* texturePath)
    {
        const auto resolved =
            resolveVideoTarget(
                texturePath
            );

        if (!resolved)
            return nullptr;

        const std::string textureKey =
            lowercasePath(
                resolved->texturePath
            );

        const std::string videoKey =
            lowercasePath(
                resolved->videoPath
            );

        std::scoped_lock lock(
            targetRegistryMutex
        );

        if (const auto target =
                targetsByTexture.find(
                    textureKey
                );
            target != targetsByTexture.end())
        {
            return target->second;
        }

        std::shared_ptr<manager> playback;

        if (const auto existingPlayback =
                playbackByVideo.find(
                    videoKey
                );
            existingPlayback !=
                playbackByVideo.end())
        {
            playback =
                existingPlayback->second;
        }
        else
        {
            playback =
                createManager(
                    resolved->videoPath.c_str(),
                    true
                );

            if (!playback)
            {
                REX::WARN(
                    "f4ffmpeg found video replacement '{}' for '{}' "
                    "but could not start playback. Vanilla remains active.",
                    resolved->videoPath,
                    resolved->texturePath
                );

                return nullptr;
            }

            playbackByVideo.emplace(
                videoKey,
                playback
            );
        }

        auto target =
            std::make_shared<videoTarget>();

        target->mode =
            resolved->mode;

        target->texturePath =
            resolved->texturePath;

        target->videoPath =
            resolved->videoPath;

        target->playback =
            std::move(playback);

        targetsByTexture.emplace(
            textureKey,
            target
        );

        REX::INFO(
            "Activated f4ffmpeg {} for '{}' -> '{}'.",
            modeName(target->mode),
            target->texturePath,
            target->videoPath
        );

        return target;
    }

    bool initializeNifHandler()
    {
        static std::once_flag initializeOnce;
        static bool initialized = false;

        std::call_once(
            initializeOnce,
            []()
            {
                // BSTextureSet exposes both texture acquisition paths at 0x2A
                // and 0x2B. Patch the concrete BSShaderTextureSet vtable, but
                // always run Bethesda's original implementation first.
                REL::Relocation<std::uintptr_t>
                    vtable{
                        RE::BSShaderTextureSet::VTABLE[0]
                    };

                const auto prefetchedOriginal =
                    *reinterpret_cast<const std::uintptr_t*>(
                        vtable.address() +
                        sizeof(void*) * 0x2A
                    );

                const auto ordinaryOriginal =
                    *reinterpret_cast<const std::uintptr_t*>(
                        vtable.address() +
                        sizeof(void*) * 0x2B
                    );

                if (
                    prefetchedOriginal == 0 ||
                    ordinaryOriginal == 0)
                {
                    REX::ERROR(
                        "Failed to resolve f4ffmpeg "
                        "texture replacement interception targets."
                    );

                    return;
                }

                // Save both originals before either hook becomes visible.
                originalGetTexturePrefetched =
                    prefetchedOriginal;

                originalGetTexture =
                    ordinaryOriginal;

                vtable.write_vfunc(
                    0x2A,
                    getTexturePrefetchedHook
                );

                vtable.write_vfunc(
                    0x2B,
                    getTextureHook
                );

                initialized = true;

                REX::INFO(
                    "f4ffmpeg texture replacement arbitration initialized."
                );
            }
        );

        return initialized;
    }
}
