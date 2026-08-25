#include "pch.h"
#include "nifHandler.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

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

        constexpr std::string_view textureTargetPrefix =
            "textures\\f4se_rttv\\";

        constexpr std::string_view textureTargetSuffix =
            "_video.dds";

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

        std::mutex observedTargetsMutex;
        std::unordered_set<std::string>
            observedTargets;

        void observeVideoTarget(
            RE::BSShaderTextureSet* textureSet,
            textureType type)
        {
            if (textureSet == nullptr)
                return;

            const char* texturePath =
                textureSet->GetTextureFilename(
                    type
                );

            const auto videoPath =
                getVideoPathForTexture(
                    texturePath
                );

            if (!videoPath)
                return;

            bool firstObservation = false;

            {
                std::scoped_lock lock(
                    observedTargetsMutex
                );

                firstObservation =
                    observedTargets.emplace(
                        *videoPath
                    ).second;
            }

            if (!firstObservation)
                return;

            REX::INFO(
                "Intercepted f4ffmpeg NIF video target: "
                "'{}' -> '{}'",
                texturePath,
                *videoPath
            );
        }

        void getTexturePrefetchedHook(
            RE::BSShaderTextureSet* textureSet,
            const void* prefetchedHandle,
            textureType type,
            RE::NiPointer<RE::NiTexture>* texture,
            bool srgb)
        {
            observeVideoTarget(
                textureSet,
                type
            );

            originalGetTexturePrefetched(
                textureSet,
                prefetchedHandle,
                type,
                texture,
                srgb
            );
        }

        void getTextureHook(
            RE::BSShaderTextureSet* textureSet,
            textureType type,
            RE::NiPointer<RE::NiTexture>* texture,
            bool srgb)
        {
            observeVideoTarget(
                textureSet,
                type
            );

            originalGetTexture(
                textureSet,
                type,
                texture,
                srgb
            );
        }
    }

    std::optional<std::string>
    getVideoPathForTexture(
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

        if (
            !startsWithInsensitive(
                normalized,
                textureTargetPrefix) ||
            !endsWithInsensitive(
                normalized,
                textureTargetSuffix))
        {
            return std::nullopt;
        }

        // Remove "Textures\\" but keep f4se_rttv and any
        // subdirectories beneath it.
        normalized.erase(
            0,
            std::string_view{"textures\\"}.size()
        );

        // Remove the marker suffix. The filename stem itself
        // becomes the video filename.
        normalized.erase(
            normalized.size() -
                textureTargetSuffix.size()
        );

        std::string videoPath{
            videoRoot
        };

        videoPath += normalized;
        videoPath += videoExtension;

        return videoPath;
    }

    bool initializeNifHandler()
    {
        static std::once_flag initializeOnce;
        static bool initialized = false;

        std::call_once(
            initializeOnce,
            []()
            {
                // BSTextureSet declares the two texture-acquisition
                // vfuncs at slots 0x2A and 0x2B. Hook both on the
                // concrete BSShaderTextureSet vtable so prefetched
                // and ordinary NIF texture acquisition are covered.
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
                        "BSShaderTextureSet interception targets."
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
                    "f4ffmpeg NIF texture interception initialized."
                );
            }
        );

        return initialized;
    }
}
