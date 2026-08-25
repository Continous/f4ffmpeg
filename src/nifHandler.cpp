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
#include <vector>

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

        // Opt-in hint, f4ffmpeg will use any appropriate replacement however.
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

        struct indexedVideoReplacement
        {
            videoTargetMode mode =
                videoTargetMode::vanillaOverride;

            std::string videoPath;
        };

        // Immutable after initializeNifHandler() publishes the hooks. Runtime texture acquisition therefore performs no filesystem work.
        std::unordered_map<
            std::string,
            indexedVideoReplacement>
            replacementIndex;

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

        int modePriority(
            videoTargetMode mode)
        {
            switch (mode)
            {
                case videoTargetMode::vanillaOverride:
                    return 0;

                case videoTargetMode::directTextureSwap:
                    return 1;
            }

            return -1;
        }

        void registerIndexedReplacement(
            std::string textureKey,
            indexedVideoReplacement replacement)
        {
            textureKey =
                lowercasePath(
                    normalizeTexturePath(
                        textureKey
                    )
                );

            const auto existing =
                replacementIndex.find(
                    textureKey
                );

            if (existing == replacementIndex.end())
            {
                replacementIndex.emplace(
                    std::move(textureKey),
                    std::move(replacement)
                );

                return;
            }

            // A direct marker is explicit author intent. This also resolves the possible collision between foo.mov -> foo_video.dds (direct) and foo_video.mov -> foo_video.dds (vanilla).
            if (
                modePriority(replacement.mode) >
                modePriority(existing->second.mode))
            {
                REX::WARN(
                    "f4ffmpeg replacement collision for '{}': '{}' [{}] supersedes '{}' [{}].",
                    textureKey,
                    replacement.videoPath,
                    modeName(replacement.mode),
                    existing->second.videoPath,
                    modeName(existing->second.mode)
                );

                existing->second =
                    std::move(replacement);

                return;
            }

            REX::WARN(
                "f4ffmpeg replacement collision for '{}': keeping '{}' [{}], ignoring '{}' [{}].",
                textureKey,
                existing->second.videoPath,
                modeName(existing->second.mode),
                replacement.videoPath,
                modeName(replacement.mode)
            );
        }

        bool buildReplacementIndex()
        {
            replacementIndex.clear();

            const std::filesystem::path root{
                videoRoot
            };

            std::error_code error;

            if (!std::filesystem::is_directory(
                    root,
                    error))
            {
                if (error)
                {
                    REX::WARN(
                        "Unable to inspect loose f4ffmpeg video directory '{}': {}.",
                        root.string(),
                        error.message()
                    );
                }
                else
                {
                    REX::INFO(
                        "No loose f4ffmpeg video directory found at '{}'; replacement index is empty.",
                        root.string()
                    );
                }

                return !error;
            }

            std::vector<std::filesystem::path>
                videoFiles;

            std::filesystem::recursive_directory_iterator iterator(
                root,
                std::filesystem::directory_options::skip_permission_denied,
                error
            );

            const std::filesystem::recursive_directory_iterator end;

            if (error)
            {
                REX::WARN(
                    "Unable to enumerate loose f4ffmpeg video directory '{}': {}.",
                    root.string(),
                    error.message()
                );

                return false;
            }

            while (iterator != end)
            {
                std::error_code statusError;

                if (
                    iterator->is_regular_file(
                        statusError
                    ) &&
                    !statusError &&
                    endsWithInsensitive(
                        iterator->path()
                            .extension()
                            .string(),
                        videoExtension
                    ))
                {
                    videoFiles.emplace_back(
                        iterator->path()
                    );
                }

                iterator.increment(error);

                if (error)
                {
                    REX::WARN(
                        "Error while enumerating loose f4ffmpeg videos: {}.",
                        error.message()
                    );

                    error.clear();
                }
            }

            // Directory traversal order is unspecified. Sorting makes any same-priority collision deterministic across systems.
            std::sort(
                videoFiles.begin(),
                videoFiles.end(),
                [](const auto& left, const auto& right)
                {
                    return lowercasePath(
                               left.generic_string()
                           ) <
                           lowercasePath(
                               right.generic_string()
                           );
                }
            );

            std::size_t registeredVideos = 0;

            for (const auto& videoPath : videoFiles)
            {
                auto relativePath =
                    videoPath.lexically_relative(
                        root
                    );

                if (
                    relativePath.empty() ||
                    startsWithInsensitive(
                        relativePath.string(),
                        ".."
                    ))
                {
                    REX::WARN(
                        "Unable to derive f4ffmpeg replacement key for '{}'.",
                        videoPath.string()
                    );

                    continue;
                }

                relativePath.replace_extension();

                std::string relativeStem =
                    relativePath.string();

                std::replace(
                    relativeStem.begin(),
                    relativeStem.end(),
                    '/',
                    '\\'
                );

                std::string textureStem{
                    texturesPrefix
                };

                textureStem +=
                    relativeStem;

                std::string vanillaTexture =
                    textureStem;

                vanillaTexture +=
                    ddsSuffix;

                std::string directTexture =
                    textureStem;

                directTexture +=
                    directSwapSuffix;

                const std::string physicalVideoPath =
                    videoPath.string();

                registerIndexedReplacement(
                    std::move(vanillaTexture),
                    indexedVideoReplacement{
                        videoTargetMode::vanillaOverride,
                        physicalVideoPath
                    }
                );

                registerIndexedReplacement(
                    std::move(directTexture),
                    indexedVideoReplacement{
                        videoTargetMode::directTextureSwap,
                        physicalVideoPath
                    }
                );

                ++registeredVideos;
            }

            REX::INFO(
                "f4ffmpeg indexed {} loose video replacement(s) into {} texture mapping(s).",
                registeredVideos,
                replacementIndex.size()
            );

            return true;
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

            if (!startsWithInsensitive(
                    normalized,
                    texturesPrefix))
            {
                return std::nullopt;
            }

            const std::string key =
                lowercasePath(
                    normalized
                );

            const auto replacement =
                replacementIndex.find(
                    key
                );

            if (replacement == replacementIndex.end())
            {
                return std::nullopt;
            }

            return resolvedVideoTarget{
                replacement->second.mode,
                std::move(normalized),
                replacement->second.videoPath
            };
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

            // Ordinary vanilla DDS paths and explicit *_video.dds markers are both cheap index lookups. Unindexed textures remain entirely Bethesda-owned.
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
            // Vanilla always runs first. f4ffmpeg is an overriding method, never a replacement for NIF loading itself.
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
            // Vanilla always runs first. If matching f4ffmpeg content exists, our target is activated afterward and becomes the preferred presentation once its produced frame is bindable.
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
                    "f4ffmpeg found video replacement '{}' for '{}' but could not start playback. Vanilla remains active.",
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
                if (!buildReplacementIndex())
                {
                    REX::WARN(
                        "f4ffmpeg replacement index initialization encountered filesystem errors; successfully indexed entries remain usable."
                    );
                }

                // BSTextureSet exposes both texture acquisition paths at 0x2A and 0x2B. Patch the concrete BSShaderTextureSet vtable, but always run Bethesda's original implementation first.
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
                        "Failed to resolve f4ffmpeg texture replacement interception targets."
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
