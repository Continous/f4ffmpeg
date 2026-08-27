#include "pch.h"
#include "nifHandler.h"
#include "manager.h"
#include "config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <RE/B/BSGraphics.h>
#include <RE/B/BSGeometry.h>
#include <RE/B/BSShaderProperty.h>
#include <RE/B/BSShaderTextureSet.h>
#include <RE/B/BSTextureSet.h>
#include <RE/N/NiPointer.h>
#include <RE/N/NiTexture.h>
#include <REL/Relocation.h>
#include <REX/FModule.h>
#include <REX/W32/D3D11.h>
#include <REX/W32/KERNEL32.h>

namespace f4ffmpeg
{
    namespace
    {
        constexpr std::string_view dataPrefix =
            "data\\";

        constexpr std::string_view texturesPrefix =
            "textures\\";

        constexpr std::string_view directSwapSuffix =
            "_video.dds";

        constexpr std::string_view ddsSuffix =
            ".dds";

        constexpr std::string_view workshopTvRasterScanTexture =
            "textures\\effects\\tvanim\\rasterscananim_d.dds";

        // Keep .mov first for backward-compatible collision precedence.
        // The decoder itself is FFmpeg-backed; nifHandler only needs to avoid
        // treating unrelated Data\\Video sidecars/config files as video inputs.
        constexpr std::array<std::string_view, 12>
            supportedVideoExtensions{
                ".mov",
                ".mp4",
                ".m4v",
                ".mkv",
                ".webm",
                ".avi",
                ".wmv",
                ".mpg",
                ".mpeg",
                ".ts",
                ".m2ts",
                ".ogv"
            };

        constexpr std::size_t psSetShaderResourcesVtableIndex =
            8;

        // BSEffectShaderProperty overrides both of these BSShaderProperty
        // virtuals. GetRenderPasses is the reliable draw-time boundary; the
        // original GetBaseTexture slot gives us Bethesda's exact source NiTexture.
        constexpr std::size_t effectGetRenderPassesVtableIndex =
            0x2B;

        constexpr std::size_t effectGetBaseTextureVtableIndex =
            0x39;

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

        int videoExtensionPriority(
            std::string_view extension)
        {
            for (
                std::size_t index = 0;
                index < supportedVideoExtensions.size();
                ++index)
            {
                if (endsWithInsensitive(
                        extension,
                        supportedVideoExtensions[index]))
                {
                    // Earlier entries have higher precedence. This makes .mov
                    // win stem collisions, preserving the behavior of releases
                    // that only recognized .mov.
                    return static_cast<int>(
                        supportedVideoExtensions.size() - index
                    );
                }
            }

            return -1;
        }

        bool isSupportedVideoPath(
            const std::filesystem::path& path)
        {
            return videoExtensionPriority(
                       path.extension().string()
                   ) >= 0;
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

        // Bethesda returns texture names in both "Textures\..." and root-relative
        // forms (for example "Effects\TVAnim\..."). Keep one canonical lookup
        // namespace so BGSM/texture-set and BGEM/effect paths meet the same index.
        std::string canonicalTextureLookupPath(
            std::string_view texturePath)
        {
            std::string canonical =
                normalizeTexturePath(
                    texturePath
                );

            if (canonical.empty())
                return canonical;

            if (!startsWithInsensitive(
                    canonical,
                    texturesPrefix))
            {
                canonical.insert(
                    0,
                    texturesPrefix
                );
            }

            // NiTexture names are normally explicit DDS paths, but tolerate the
            // extensionless form because the engine's texture APIs do as well.
            const auto lastSlash =
                canonical.find_last_of('\\');

            const auto lastDot =
                canonical.find_last_of('.');

            if (
                lastDot == std::string::npos ||
                (lastSlash != std::string::npos &&
                 lastDot < lastSlash))
            {
                canonical += ddsSuffix;
            }

            return canonical;
        }


        std::string trimIniValue(
            std::string_view value)
        {
            std::size_t begin = 0;
            std::size_t end = value.size();

            while (
                begin < end &&
                (value[begin] == ' ' ||
                 value[begin] == '\t' ||
                 value[begin] == '\r' ||
                 value[begin] == '\n'))
            {
                ++begin;
            }

            while (
                end > begin &&
                (value[end - 1] == ' ' ||
                 value[end - 1] == '\t' ||
                 value[end - 1] == '\r' ||
                 value[end - 1] == '\n'))
            {
                --end;
            }

            std::string result{
                value.substr(
                    begin,
                    end - begin
                )
            };

            if (
                result.size() >= 2 &&
                ((result.front() == '"' &&
                  result.back() == '"') ||
                 (result.front() == '\'' &&
                  result.back() == '\'')))
            {
                result = result.substr(
                    1,
                    result.size() - 2
                );
            }

            return result;
        }

        std::optional<bool> parseIniBool(
            std::string_view value)
        {
            const std::string lowered =
                lowercasePath(
                    trimIniValue(value)
                );

            if (
                lowered == "1" ||
                lowered == "true" ||
                lowered == "yes" ||
                lowered == "on")
            {
                return true;
            }

            if (
                lowered == "0" ||
                lowered == "false" ||
                lowered == "no" ||
                lowered == "off")
            {
                return false;
            }

            return std::nullopt;
        }

        void appendPlaylistEntry(
            videoPlaybackSettings& settings,
            const std::filesystem::path& iniPath,
            std::string_view value)
        {
            std::string entry =
                trimIniValue(value);

            if (entry.empty())
                return;

            std::filesystem::path entryPath{
                entry
            };

            if (entryPath.is_relative())
            {
                entryPath =
                    iniPath.parent_path() /
                    entryPath;
            }

            settings.playlist.emplace_back(
                entryPath.lexically_normal().string()
            );
        }

        videoPlaybackSettings loadPlaybackSettings(
            const std::filesystem::path& videoPath)
        {
            videoPlaybackSettings settings{};

            std::filesystem::path iniPath =
                videoPath;

            iniPath.replace_extension(".ini");

            std::error_code statusError;

            const bool exists =
                std::filesystem::is_regular_file(
                    iniPath,
                    statusError
                );

            if (statusError)
            {
                REX::WARN(
                    "f4ffmpeg could not inspect playback INI '{}': {}.",
                    iniPath.string(),
                    statusError.message()
                );

                return settings;
            }

            if (!exists)
                return settings;

            std::ifstream input(
                iniPath,
                std::ios::in |
                    std::ios::binary
            );

            if (!input)
            {
                REX::WARN(
                    "f4ffmpeg could not open playback INI '{}'; using defaults.",
                    iniPath.string()
                );

                return settings;
            }

            std::string section;
            std::string line;
            std::size_t lineNumber = 0;

            while (std::getline(input, line))
            {
                ++lineNumber;

                if (
                    lineNumber == 1 &&
                    line.size() >= 3 &&
                    static_cast<unsigned char>(line[0]) == 0xEF &&
                    static_cast<unsigned char>(line[1]) == 0xBB &&
                    static_cast<unsigned char>(line[2]) == 0xBF)
                {
                    line.erase(0, 3);
                }

                std::string trimmed =
                    trimIniValue(line);

                if (
                    trimmed.empty() ||
                    trimmed.front() == ';' ||
                    trimmed.front() == '#')
                {
                    continue;
                }

                if (
                    trimmed.size() >= 2 &&
                    trimmed.front() == '[' &&
                    trimmed.back() == ']')
                {
                    section =
                        lowercasePath(
                            trimIniValue(
                                std::string_view{trimmed}.substr(
                                    1,
                                    trimmed.size() - 2
                                )
                            )
                        );

                    continue;
                }

                const auto equals =
                    trimmed.find('=');

                if (equals == std::string::npos)
                {
                    REX::WARN(
                        "f4ffmpeg ignored malformed playback INI line {} in '{}'.",
                        lineNumber,
                        iniPath.string()
                    );

                    continue;
                }

                const std::string key =
                    lowercasePath(
                        trimIniValue(
                            std::string_view{trimmed}.substr(
                                0,
                                equals
                            )
                        )
                    );

                const std::string value =
                    trimIniValue(
                        std::string_view{trimmed}.substr(
                            equals + 1
                        )
                    );

                const bool playbackSection =
                    section.empty() ||
                    section == "playback";

                if (
                    playbackSection &&
                    (key == "loop" ||
                     key == "looping"))
                {
                    const auto parsed =
                        parseIniBool(value);

                    if (parsed)
                    {
                        settings.looping = *parsed;
                    }
                    else
                    {
                        REX::WARN(
                            "f4ffmpeg ignored invalid Loop value '{}' on line {} of '{}'.",
                            value,
                            lineNumber,
                            iniPath.string()
                        );
                    }

                    continue;
                }

                if (
                    playbackSection &&
                    key == "shuffle")
                {
                    const auto parsed =
                        parseIniBool(value);

                    if (parsed)
                    {
                        settings.shuffle = *parsed;
                    }
                    else
                    {
                        REX::WARN(
                            "f4ffmpeg ignored invalid Shuffle value '{}' on line {} of '{}'.",
                            value,
                            lineNumber,
                            iniPath.string()
                        );
                    }

                    continue;
                }

                if (
                    (playbackSection &&
                     (key == "playlist" ||
                      key == "playlistitem")) ||
                    (section == "playlist" &&
                     (key == "item" ||
                      key == "entry" ||
                      key == "file")))
                {
                    appendPlaylistEntry(
                        settings,
                        iniPath,
                        value
                    );

                    continue;
                }

                REX::WARN(
                    "f4ffmpeg ignored unknown playback INI setting '{}={}' in section [{}] of '{}'.",
                    key,
                    value,
                    section.empty()
                        ? "Playback"
                        : section,
                    iniPath.string()
                );
            }

            REX::INFO(
                "f4ffmpeg loaded playback INI '{}': loop={}, shuffle={}, playlist entries={}.",
                iniPath.string(),
                settings.looping,
                settings.shuffle,
                settings.playlist.size()
            );

            return settings;
        }

        bool isWorkshopTvRasterScanTexture(
            std::string_view texturePath)
        {
            return lowercasePath(
                       canonicalTextureLookupPath(
                           texturePath
                       )
                   ) == workshopTvRasterScanTexture;
        }

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

        struct indexedVideoReplacement
        {
            videoTargetMode mode =
                videoTargetMode::vanillaOverride;

            std::string videoPath;
            videoPlaybackSettings playbackSettings;
        };

        struct resolvedVideoTarget
        {
            videoTargetMode mode =
                videoTargetMode::vanillaOverride;

            std::string texturePath;
            std::string videoPath;
            videoPlaybackSettings playbackSettings;
        };

        // Published once before hooks are installed, then read-only.
        std::unordered_map<
            std::string,
            indexedVideoReplacement>
            replacementIndex;

        std::mutex playbackRegistryMutex;

        std::unordered_map<
            std::string,
            std::shared_ptr<manager>>
            playbackByVideo;

        std::mutex dispatchMutex;

        void registerIndexedReplacement(
            std::string textureKey,
            indexedVideoReplacement replacement)
        {
            textureKey =
                lowercasePath(
                    canonicalTextureLookupPath(
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

            const int replacementModePriority =
                modePriority(replacement.mode);

            const int existingModePriority =
                modePriority(existing->second.mode);

            const int replacementExtensionPriority =
                videoExtensionPriority(
                    std::filesystem::path(
                        replacement.videoPath
                    ).extension().string()
                );

            const int existingExtensionPriority =
                videoExtensionPriority(
                    std::filesystem::path(
                        existing->second.videoPath
                    ).extension().string()
                );

            const bool replacementWins =
                replacementModePriority >
                    existingModePriority ||
                (
                    replacementModePriority ==
                        existingModePriority &&
                    replacementExtensionPriority >
                        existingExtensionPriority
                );

            if (replacementWins)
            {
                REX::WARN(
                    "f4ffmpeg replacement collision for '{}': "
                    "'{}' [{}] supersedes '{}' [{}].",
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
                "f4ffmpeg replacement collision for '{}': keeping '{}' [{}], "
                "ignoring '{}' [{}].",
                textureKey,
                existing->second.videoPath,
                modeName(existing->second.mode),
                replacement.videoPath,
                modeName(replacement.mode)
            );
        }

        std::optional<std::filesystem::path>
        getGameRootPath()
        {
            std::vector<wchar_t> buffer(1024);

            while (true)
            {
                const auto length =
                    REX::W32::GetModuleFileNameW(
                        nullptr,
                        buffer.data(),
                        static_cast<std::uint32_t>(
                            buffer.size()
                        )
                    );

                if (length == 0)
                {
                    REX::WARN(
                        "f4ffmpeg could not resolve the Fallout executable path "
                        "while locating loose videos."
                    );

                    return std::nullopt;
                }

                if (length < buffer.size() - 1)
                {
                    std::filesystem::path executablePath(
                        buffer.data(),
                        buffer.data() + length
                    );

                    return executablePath.parent_path();
                }

                if (buffer.size() >= 32768)
                {
                    REX::WARN(
                        "f4ffmpeg executable path exceeded the supported length."
                    );

                    return std::nullopt;
                }

                buffer.resize(
                    buffer.size() * 2
                );
            }
        }

        bool buildReplacementIndex()
        {
            replacementIndex.clear();

            const auto gameRoot =
                getGameRootPath();

            if (!gameRoot)
                return false;

            const std::filesystem::path root =
                *gameRoot / "Data" / "Video";

            REX::INFO(
                "Scanning loose f4ffmpeg videos at '{}' "
                "(supported containers: .mov, .mp4, .m4v, .mkv, .webm, "
                ".avi, .wmv, .mpg, .mpeg, .ts, .m2ts, .ogv).",
                root.string()
            );

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
                        "No loose f4ffmpeg video directory found at '{}'; "
                        "replacement index is empty.",
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
                    isSupportedVideoPath(
                        iterator->path()
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

            std::size_t activeVideos = 0;

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

                const std::string physicalVideoPath =
                    videoPath.string();

                const videoPlaybackSettings playbackSettings =
                    loadPlaybackSettings(
                        videoPath
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

                registerIndexedReplacement(
                    std::move(vanillaTexture),
                    indexedVideoReplacement{
                        videoTargetMode::vanillaOverride,
                        physicalVideoPath,
                        playbackSettings
                    }
                );

                registerIndexedReplacement(
                    std::move(directTexture),
                    indexedVideoReplacement{
                        videoTargetMode::directTextureSwap,
                        physicalVideoPath,
                        playbackSettings
                    }
                );

                ++activeVideos;
            }

            REX::INFO(
                "f4ffmpeg indexed {} loose video replacement(s) into {} "
                "texture mapping(s); manager dispatch is deferred until game load.",
                activeVideos,
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
                canonicalTextureLookupPath(
                    texturePath
                );

            if (normalized.empty())
                return std::nullopt;

            const std::string key =
                lowercasePath(
                    normalized
                );

            const auto replacement =
                replacementIndex.find(
                    key
                );

            if (replacement == replacementIndex.end())
                return std::nullopt;

            return resolvedVideoTarget{
                replacement->second.mode,
                std::move(normalized),
                replacement->second.videoPath,
                replacement->second.playbackSettings
            };
        }

        std::mutex targetRegistryMutex;

        std::unordered_map<
            std::string,
            std::shared_ptr<videoTarget>>
            targetsByTexture;

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

        using getTextureFilename_t = const char*(*)(
            RE::BSShaderTextureSet*,
            textureType
        );

        REL::Relocation<getTextureFilename_t>
            originalGetTextureFilename;

        REL::Relocation<getTexturePrefetched_t>
            originalGetTexturePrefetched;

        REL::Relocation<getTexture_t>
            originalGetTexture;

        std::atomic_bool filenameHookObserved = false;
        std::atomic_bool prefetchedHookObserved = false;
        std::atomic_bool ordinaryHookObserved = false;

        std::mutex diagnosticTextureMutex;
        std::unordered_set<std::string>
            diagnosticTexturePaths;

        void diagnoseResolvedTexturePath(
            const char* hookName,
            const char* texturePath)
        {
            if (
                texturePath == nullptr ||
                *texturePath == '\0')
            {
                return;
            }

            const std::string normalized =
                canonicalTextureLookupPath(
                    texturePath
                );

            const std::string key =
                lowercasePath(
                    normalized
                );

            const bool indexed =
                replacementIndex.find(key) !=
                    replacementIndex.end();

            const bool tvRelated =
                key.find("tvanim") != std::string::npos ||
                key.find("standby") != std::string::npos ||
                key.find("television") != std::string::npos;

            if (!indexed && !tvRelated)
                return;

            std::string diagnosticKey{
                hookName
                    ? hookName
                    : "unknown"
            };

            diagnosticKey += "|";
            diagnosticKey += key;

            {
                std::scoped_lock lock(
                    diagnosticTextureMutex
                );

                if (!diagnosticTexturePaths.emplace(
                        std::move(diagnosticKey)
                    ).second)
                {
                    return;
                }
            }

            if (indexed)
            {
                REX::INFO(
                    "f4ffmpeg {} observed indexed texture '{}'.",
                    hookName,
                    normalized
                );
            }
            else
            {
                REX::INFO(
                    "f4ffmpeg {} observed TV-related texture '{}' "
                    "with no indexed replacement.",
                    hookName,
                    normalized
                );
            }
        }

        void diagnoseTexturePath(
            const char* hookName,
            textureType type,
            const char* texturePath)
        {
            if (type != textureType::kBase)
                return;

            diagnoseResolvedTexturePath(
                hookName,
                texturePath
            );
        }

        const char* getTextureFilenameHook(
            RE::BSShaderTextureSet* textureSet,
            textureType type)
        {
            const char* texturePath =
                originalGetTextureFilename(
                    textureSet,
                    type
                );

            if (!filenameHookObserved.exchange(
                    true,
                    std::memory_order_acq_rel
                ))
            {
                REX::INFO(
                    "f4ffmpeg BSShaderTextureSet::GetTextureFilename "
                    "hook reached (first type={}).",
                    static_cast<std::uint32_t>(type)
                );
            }

            diagnoseTexturePath(
                "GetTextureFilename",
                type,
                texturePath
            );

            return texturePath;
        }

        // F4SE's public reverse-engineered BSRenderData layout places the
        // ID3D11ShaderResourceView at offset 0. CommonLib exposes the containing
        // object as opaque BSGraphics::Texture, so only this first pointer is
        // observed. We never mutate the object.
        struct rendererTexturePrefix
        {
            REX::W32::ID3D11ShaderResourceView*
                resourceView = nullptr;
        };

        static_assert(
            offsetof(
                rendererTexturePrefix,
                resourceView
            ) == 0
        );

        std::shared_mutex bindingMutex;

        std::unordered_map<
            REX::W32::ID3D11ShaderResourceView*,
            std::shared_ptr<const videoTarget>>
            targetsByVanillaSrv;

        std::unordered_set<
            REX::W32::ID3D11ShaderResourceView*>
            suppressedVanillaSrvs;

        bool workshopTvRasterScanSuppressionEnabled =
            false;

        bool workshopTvStaticSuppressionEnabled =
            false;

        bool workshopTvWarpSuppressionEnabled =
            false;

        std::atomic_bool rasterScanSuppressionObserved =
            false;

        std::atomic_bool staticEffectSuppressionObserved =
            false;

        std::atomic_bool warpEffectSuppressionObserved =
            false;

        std::atomic_bool hasPresentationBindings =
            false;

        std::atomic_bool presentationObserved =
            false;

        std::atomic_bool psHookObserved =
            false;

        std::mutex observedBindingMutex;

        std::unordered_set<std::string>
            observedBindings;

        std::mutex bindingFailureMutex;
        std::unordered_set<std::string>
            bindingFailures;

        void logBindingFailureOnce(
            const char* texturePath,
            const char* reason)
        {
            std::string key =
                lowercasePath(
                    canonicalTextureLookupPath(
                        texturePath
                            ? texturePath
                            : ""
                    )
                );

            key += "|";
            key += reason
                ? reason
                : "unknown";

            {
                std::scoped_lock lock(
                    bindingFailureMutex
                );

                if (!bindingFailures.emplace(
                        std::move(key)
                    ).second)
                {
                    return;
                }
            }

            REX::INFO(
                "f4ffmpeg handled texture '{}' reached acquisition, "
                "but presentation binding is not ready: {}.",
                texturePath
                    ? texturePath
                    : "<null>",
                reason
                    ? reason
                    : "unknown"
            );
        }

        REX::W32::ID3D11ShaderResourceView*
        getNiTextureSrv(
            const char* texturePath,
            RE::NiTexture* niTexture)
        {
            if (niTexture == nullptr)
            {
                logBindingFailureOnce(
                    texturePath,
                    "NiTexture pointer is null"
                );

                return nullptr;
            }

            if (niTexture->rendererTexture == nullptr)
            {
                logBindingFailureOnce(
                    texturePath,
                    "NiTexture has no rendererTexture yet"
                );

                return nullptr;
            }

            const auto* rendererTexture =
                reinterpret_cast<
                    const rendererTexturePrefix*>(
                        niTexture->rendererTexture
                    );

            auto* srv =
                rendererTexture->resourceView;

            if (srv == nullptr)
            {
                logBindingFailureOnce(
                    texturePath,
                    "rendererTexture has no shader resource view"
                );
            }

            return srv;
        }

        bool registerNiTexturePresentationBinding(
            const char* texturePath,
            RE::NiTexture* niTexture,
            const char* sourceName)
        {
            auto target =
                getVideoTargetForTexture(
                    texturePath
                );

            if (!target)
                return true;

            auto* vanillaSrv =
                getNiTextureSrv(
                    texturePath,
                    niTexture
                );

            if (vanillaSrv == nullptr)
                return false;

            bool changed = false;

            {
                std::unique_lock lock(
                    bindingMutex
                );

                const auto existing =
                    targetsByVanillaSrv.find(
                        vanillaSrv
                    );

                if (existing == targetsByVanillaSrv.end())
                {
                    targetsByVanillaSrv.emplace(
                        vanillaSrv,
                        target
                    );

                    changed = true;
                }
                else if (
                    existing->second->getVideoPath() !=
                        target->getVideoPath() &&
                    modePriority(target->getMode()) >
                        modePriority(existing->second->getMode()))
                {
                    existing->second =
                        target;

                    changed = true;
                }
            }

            hasPresentationBindings.store(
                true,
                std::memory_order_release
            );

            if (!changed)
                return true;

            std::string logKey =
                lowercasePath(
                    canonicalTextureLookupPath(
                        texturePath
                            ? texturePath
                            : ""
                    )
                );

            logKey += "|";
            logKey += sourceName
                ? sourceName
                : "unknown";

            {
                std::scoped_lock lock(
                    observedBindingMutex
                );

                if (!observedBindings.emplace(
                        std::move(logKey)
                    ).second)
                {
                    return true;
                }
            }

            REX::INFO(
                "Bound f4ffmpeg {} presentation '{}' -> '{}' via {}.",
                modeName(target->getMode()),
                target->getTexturePath(),
                target->getVideoPath(),
                sourceName
                    ? sourceName
                    : "unknown"
            );

            return true;
        }

        bool registerNiTextureSuppressionBinding(
            const char* texturePath,
            RE::NiTexture* niTexture,
            const char* sourceName)
        {
            auto* vanillaSrv =
                getNiTextureSrv(
                    texturePath,
                    niTexture
                );

            if (vanillaSrv == nullptr)
                return false;

            bool changed = false;

            {
                std::unique_lock lock(
                    bindingMutex
                );

                changed =
                    suppressedVanillaSrvs.emplace(
                        vanillaSrv
                    ).second;
            }

            if (!changed)
                return true;

            hasPresentationBindings.store(
                true,
                std::memory_order_release
            );

            REX::INFO(
                "Bound target-local f4ffmpeg TV raster-scan suppression '{}' via {}.",
                canonicalTextureLookupPath(
                    texturePath
                        ? texturePath
                        : ""
                ),
                sourceName
                    ? sourceName
                    : "unknown"
            );

            return true;
        }

        void registerPresentationBinding(
            RE::BSShaderTextureSet* textureSet,
            textureType type,
            RE::NiPointer<RE::NiTexture>* texture)
        {
            if (
                textureSet == nullptr ||
                type != textureType::kBase)
            {
                return;
            }

            const char* texturePath =
                originalGetTextureFilename(
                    textureSet,
                    type
                );

            if (
                texture == nullptr ||
                !*texture)
            {
                if (getVideoTargetForTexture(texturePath))
                {
                    logBindingFailureOnce(
                        texturePath,
                        "Bethesda returned no NiTexture yet"
                    );
                }

                return;
            }

            registerNiTexturePresentationBinding(
                texturePath,
                texture->get(),
                "BSShaderTextureSet"
            );
        }

        void getTexturePrefetchedHook(
            RE::BSShaderTextureSet* textureSet,
            const void* prefetchedHandle,
            textureType type,
            RE::NiPointer<RE::NiTexture>* texture,
            bool srgb)
        {
            // Bethesda always owns acquisition. f4ffmpeg only registers a
            // presentation override for the object Bethesda actually returned.
            originalGetTexturePrefetched(
                textureSet,
                prefetchedHandle,
                type,
                texture,
                srgb
            );

            if (!prefetchedHookObserved.exchange(
                    true,
                    std::memory_order_acq_rel
                ))
            {
                REX::INFO(
                    "f4ffmpeg BSShaderTextureSet::GetTexture(prefetched) "
                    "hook reached (first type={}).",
                    static_cast<std::uint32_t>(type)
                );
            }

            diagnoseTexturePath(
                "GetTexture(prefetched)",
                type,
                originalGetTextureFilename(
                    textureSet,
                    type
                )
            );

            registerPresentationBinding(
                textureSet,
                type,
                texture
            );
        }

        void getTextureHook(
            RE::BSShaderTextureSet* textureSet,
            textureType type,
            RE::NiPointer<RE::NiTexture>* texture,
            bool srgb)
        {
            originalGetTexture(
                textureSet,
                type,
                texture,
                srgb
            );

            if (!ordinaryHookObserved.exchange(
                    true,
                    std::memory_order_acq_rel
                ))
            {
                REX::INFO(
                    "f4ffmpeg BSShaderTextureSet::GetTexture ordinary "
                    "hook reached (first type={}).",
                    static_cast<std::uint32_t>(type)
                );
            }

            diagnoseTexturePath(
                "GetTexture",
                type,
                originalGetTextureFilename(
                    textureSet,
                    type
                )
            );

            registerPresentationBinding(
                textureSet,
                type,
                texture
            );
        }

        struct msvcCompleteObjectLocator64
        {
            std::uint32_t signature = 0;
            std::uint32_t offset = 0;
            std::uint32_t constructorDisplacementOffset = 0;
            std::int32_t typeDescriptorRva = 0;
            std::int32_t classDescriptorRva = 0;
            std::int32_t selfRva = 0;
        };

        static_assert(
            sizeof(msvcCompleteObjectLocator64) == 0x18
        );

        template <class T>
        T readUnaligned(
            std::uintptr_t address)
        {
            T value{};

            std::memcpy(
                &value,
                reinterpret_cast<const void*>(
                    address
                ),
                sizeof(T)
            );

            return value;
        }

        bool containsAddressRange(
            const REX::FModuleSection& segment,
            std::uintptr_t address,
            std::size_t size)
        {
            if (size > segment.GetSize())
                return false;

            const auto begin =
                segment.GetAddress();

            const auto end =
                begin + segment.GetSize();

            return
                address >= begin &&
                address <= end - size;
        }

        std::optional<std::uintptr_t>
        findBytesInSegment(
            const REX::FModuleSection& segment,
            std::string_view bytes)
        {
            if (
                bytes.empty() ||
                bytes.size() + 1 > segment.GetSize())
            {
                return std::nullopt;
            }

            const auto* begin =
                reinterpret_cast<const char*>(
                    segment.GetAddress()
                );

            const auto* end =
                begin + segment.GetSize() -
                    bytes.size() - 1;

            for (const char* current = begin;
                 current <= end;
                 ++current)
            {
                if (
                    std::memcmp(
                        current,
                        bytes.data(),
                        bytes.size()
                    ) == 0 &&
                    current[bytes.size()] == '\0')
                {
                    return reinterpret_cast<
                        std::uintptr_t>(
                            current
                        );
                }
            }

            return std::nullopt;
        }

        std::optional<std::uintptr_t>
        findEffectShaderPropertyTypeDescriptor()
        {
            const auto module =
                REX::FModule::GetExecutingModule();

            const auto data =
                module.GetSection(".data");

            const auto rdata =
                module.GetSection(".rdata");

            // Fallout's engine types live in the executable's global namespace.
            // Keep the RE-qualified spelling as a harmless fallback for forks or
            // alternate symbol-generation conventions.
            static constexpr std::array names{
                std::string_view{
                    ".?AVBSEffectShaderProperty@@"
                },
                std::string_view{
                    ".?AVBSEffectShaderProperty@RE@@"
                }
            };

            for (const auto name : names)
            {
                for (const auto* segment :
                     std::array{
                         &data,
                         &rdata
                     })
                {
                    const auto nameAddress =
                        findBytesInSegment(
                            *segment,
                            name
                        );

                    if (!nameAddress)
                        continue;

                    constexpr std::size_t
                        typeDescriptorPrefixSize =
                            sizeof(void*) * 2;

                    if (
                        *nameAddress <
                        segment->GetAddress() +
                            typeDescriptorPrefixSize)
                    {
                        continue;
                    }

                    const auto typeDescriptor =
                        *nameAddress -
                        typeDescriptorPrefixSize;

                    REX::INFO(
                        "f4ffmpeg located BSEffectShaderProperty MSVC RTTI "
                        "type descriptor at {} (name '{}').",
                        reinterpret_cast<void*>(
                            typeDescriptor
                        ),
                        name
                    );

                    return typeDescriptor;
                }
            }

            return std::nullopt;
        }

        std::optional<std::uintptr_t>
        findEffectShaderPropertyVtable()
        {
            const auto module =
                REX::FModule::GetExecutingModule();

            const auto moduleBase =
                module.GetBaseAddress();

            const auto rdata =
                module.GetSection(".rdata");

            const auto typeDescriptor =
                findEffectShaderPropertyTypeDescriptor();

            if (!typeDescriptor)
                return std::nullopt;

            const auto typeDescriptorOffset =
                *typeDescriptor -
                moduleBase;

            if (
                typeDescriptorOffset >
                static_cast<std::uintptr_t>(
                    std::numeric_limits<std::int32_t>::max()
                ))
            {
                return std::nullopt;
            }

            const auto typeDescriptorRva =
                static_cast<std::int32_t>(
                    typeDescriptorOffset
                );

            std::vector<std::uintptr_t>
                completeObjectLocators;

            const auto rdataBegin =
                rdata.GetAddress();

            const auto rdataEnd =
                rdataBegin +
                rdata.GetSize();

            for (
                std::uintptr_t address =
                    rdataBegin;
                address +
                    sizeof(msvcCompleteObjectLocator64) <=
                    rdataEnd;
                address += alignof(std::uint32_t))
            {
                const auto locator =
                    readUnaligned<
                        msvcCompleteObjectLocator64>(
                            address
                        );

                if (
                    locator.signature != 1 ||
                    locator.typeDescriptorRva !=
                        typeDescriptorRva)
                {
                    continue;
                }

                const auto selfOffset =
                    address -
                    moduleBase;

                if (
                    selfOffset >
                    static_cast<std::uintptr_t>(
                        std::numeric_limits<std::int32_t>::max()
                    ) ||
                    locator.selfRva !=
                        static_cast<std::int32_t>(
                            selfOffset
                        ))
                {
                    continue;
                }

                completeObjectLocators.emplace_back(
                    address
                );
            }

            if (completeObjectLocators.empty())
                return std::nullopt;

            std::vector<std::uintptr_t>
                vtables;

            for (const auto locatorAddress :
                 completeObjectLocators)
            {
                for (
                    std::uintptr_t address =
                        rdataBegin;
                    address + sizeof(std::uintptr_t) <=
                        rdataEnd;
                    address += alignof(std::uintptr_t))
                {
                    if (
                        readUnaligned<std::uintptr_t>(
                            address
                        ) != locatorAddress)
                    {
                        continue;
                    }

                    const auto vtable =
                        address +
                        sizeof(std::uintptr_t);

                    const auto requiredSize =
                        sizeof(std::uintptr_t) *
                        (effectGetBaseTextureVtableIndex + 1);

                    if (!containsAddressRange(
                            rdata,
                            vtable,
                            requiredSize
                        ))
                    {
                        continue;
                    }

                    const auto firstVirtual =
                        readUnaligned<std::uintptr_t>(
                            vtable
                        );

                    const auto getBaseTexture =
                        readUnaligned<std::uintptr_t>(
                            vtable +
                            sizeof(std::uintptr_t) *
                                effectGetBaseTextureVtableIndex
                        );

                    if (
                        firstVirtual == 0 ||
                        getBaseTexture == 0)
                    {
                        continue;
                    }

                    if (
                        std::find(
                            vtables.begin(),
                            vtables.end(),
                            vtable
                        ) == vtables.end())
                    {
                        vtables.emplace_back(
                            vtable
                        );
                    }
                }
            }

            if (vtables.size() != 1)
            {
                REX::ERROR(
                    "f4ffmpeg BSEffectShaderProperty RTTI produced {} "
                    "candidate vtable(s); refusing to patch an ambiguous target.",
                    vtables.size()
                );

                return std::nullopt;
            }

            return vtables.front();
        }

        using effectGetBaseTexture_t =
            RE::NiTexture* (*)(
                const RE::BSShaderProperty*
            );

        using effectGetRenderPasses_t =
            RE::BSShaderProperty::RenderPassArray* (*)(
                RE::BSShaderProperty*,
                RE::BSGeometry*,
                std::uint32_t,
                RE::BSShaderAccumulator*
            );

        REL::Relocation<effectGetBaseTexture_t>
            originalEffectGetBaseTexture;

        REL::Relocation<effectGetRenderPasses_t>
            originalEffectGetRenderPasses;

        std::atomic_bool effectRenderPassHookObserved =
            false;

        enum class workshopTvEffectKind : std::uint8_t
        {
            other,
            rasterScan,
            staticFuzz,
            screenWarp
        };

        std::shared_mutex touchedTvNodesMutex;

        // BSShaderProperty::fadeNode is instance-local render ownership. Using
        // it as the association key means the effect options apply only to the
        // particular TV/NIF instance whose screen texture f4ffmpeg is replacing.
        std::unordered_set<const void*>
            touchedTvNodes;

        std::mutex tvEffectDiagnosticMutex;
        std::unordered_set<std::string>
            tvEffectDiagnostics;

        RE::BSShaderProperty::RenderPassArray
            emptyEffectRenderPasses{nullptr};

        const void* getEffectOwnerKey(
            const RE::BSShaderProperty* shaderProperty)
        {
            if (
                shaderProperty == nullptr ||
                shaderProperty->fadeNode == nullptr)
            {
                return nullptr;
            }

            return static_cast<const void*>(
                shaderProperty->fadeNode
            );
        }

        void markTvNodeTouched(
            const RE::BSShaderProperty* shaderProperty)
        {
            const void* owner =
                getEffectOwnerKey(
                    shaderProperty
                );

            if (owner == nullptr)
                return;

            std::unique_lock lock(
                touchedTvNodesMutex
            );

            touchedTvNodes.emplace(
                owner
            );
        }

        bool isTvNodeTouched(
            const RE::BSShaderProperty* shaderProperty)
        {
            const void* owner =
                getEffectOwnerKey(
                    shaderProperty
                );

            if (owner == nullptr)
                return false;

            std::shared_lock lock(
                touchedTvNodesMutex
            );

            return touchedTvNodes.find(
                       owner
                   ) != touchedTvNodes.end();
        }

        void appendEffectSignaturePart(
            std::string& signature,
            const char* value)
        {
            if (
                value == nullptr ||
                *value == '\0')
            {
                return;
            }

            if (!signature.empty())
                signature += '|';

            signature +=
                lowercasePath(
                    value
                );
        }

        std::string buildEffectSignature(
            const RE::BSShaderProperty* shaderProperty,
            const RE::BSGeometry* geometry,
            const RE::NiTexture* baseTexture)
        {
            std::string signature;

            if (geometry != nullptr)
            {
                appendEffectSignaturePart(
                    signature,
                    geometry->name.c_str()
                );
            }

            if (shaderProperty != nullptr)
            {
                appendEffectSignaturePart(
                    signature,
                    shaderProperty->name.c_str()
                );
            }

            if (baseTexture != nullptr)
            {
                appendEffectSignaturePart(
                    signature,
                    baseTexture->name.c_str()
                );
            }

            return signature;
        }

        bool signatureContainsAny(
            std::string_view signature,
            std::initializer_list<std::string_view> tokens)
        {
            for (const auto token : tokens)
            {
                if (
                    signature.find(token) !=
                        std::string_view::npos)
                {
                    return true;
                }
            }

            return false;
        }

        workshopTvEffectKind classifyWorkshopTvEffect(
            const RE::BSShaderProperty* shaderProperty,
            const RE::BSGeometry* geometry,
            const RE::NiTexture* baseTexture)
        {
            if (
                baseTexture != nullptr &&
                isWorkshopTvRasterScanTexture(
                    baseTexture->name.c_str()
                ))
            {
                return workshopTvEffectKind::rasterScan;
            }

            const std::string signature =
                buildEffectSignature(
                    shaderProperty,
                    geometry,
                    baseTexture
                );

            // Bethesda's TV is authored as separate NIF effect geometries for
            // distortion/warp and animated noise/static. Prefer semantic NIF /
            // material names over generic texture paths so shared utility
            // textures (notably ColorBlackUtility.dds) are never globally killed.
            if (signatureContainsAny(
                    signature,
                    {
                        "warp",
                        "distort",
                        "distortion",
                        "wobble"
                    }))
            {
                return workshopTvEffectKind::screenWarp;
            }

            if (signatureContainsAny(
                    signature,
                    {
                        "static",
                        "fuzz",
                        "noise",
                        "interference"
                    }))
            {
                return workshopTvEffectKind::staticFuzz;
            }

            return workshopTvEffectKind::other;
        }

        const char* workshopTvEffectKindName(
            workshopTvEffectKind kind)
        {
            switch (kind)
            {
                case workshopTvEffectKind::rasterScan:
                    return "raster scan";

                case workshopTvEffectKind::staticFuzz:
                    return "static/fuzz";

                case workshopTvEffectKind::screenWarp:
                    return "screen warp";

                case workshopTvEffectKind::other:
                default:
                    return "unclassified";
            }
        }

        void diagnoseTouchedTvEffect(
            const RE::BSShaderProperty* shaderProperty,
            const RE::BSGeometry* geometry,
            const RE::NiTexture* baseTexture,
            workshopTvEffectKind kind)
        {
            const std::string geometryName =
                geometry != nullptr &&
                geometry->name.c_str() != nullptr
                    ? geometry->name.c_str()
                    : "<unnamed>";

            const std::string propertyName =
                shaderProperty != nullptr &&
                shaderProperty->name.c_str() != nullptr
                    ? shaderProperty->name.c_str()
                    : "<unnamed>";

            const std::string textureName =
                baseTexture != nullptr &&
                baseTexture->name.c_str() != nullptr
                    ? baseTexture->name.c_str()
                    : "<none>";

            std::string key =
                lowercasePath(
                    geometryName + "|" +
                    propertyName + "|" +
                    textureName
                );

            {
                std::scoped_lock lock(
                    tvEffectDiagnosticMutex
                );

                if (!tvEffectDiagnostics.emplace(
                        std::move(key)
                    ).second)
                {
                    return;
                }
            }

            REX::INFO(
                "f4ffmpeg touched-TV effect: kind={}, geometry='{}', shader='{}', baseTexture='{}'.",
                workshopTvEffectKindName(kind),
                geometryName,
                propertyName,
                textureName
            );
        }

        bool shouldSuppressWorkshopTvEffect(
            workshopTvEffectKind kind)
        {
            switch (kind)
            {
                case workshopTvEffectKind::staticFuzz:
                    return workshopTvStaticSuppressionEnabled;

                case workshopTvEffectKind::screenWarp:
                    return workshopTvWarpSuppressionEnabled;

                default:
                    return false;
            }
        }

        void reportWorkshopTvEffectSuppression(
            workshopTvEffectKind kind)
        {
            std::atomic_bool* observed = nullptr;

            switch (kind)
            {
                case workshopTvEffectKind::staticFuzz:
                    observed =
                        &staticEffectSuppressionObserved;
                    break;

                case workshopTvEffectKind::screenWarp:
                    observed =
                        &warpEffectSuppressionObserved;
                    break;

                default:
                    return;
            }

            if (!observed->exchange(
                    true,
                    std::memory_order_acq_rel
                ))
            {
                REX::INFO(
                    "f4ffmpeg target-local workshop-TV {} suppression reached GetRenderPasses.",
                    workshopTvEffectKindName(kind)
                );
            }
        }

        RE::BSShaderProperty::RenderPassArray*
        effectGetRenderPassesHook(
            RE::BSShaderProperty* shaderProperty,
            RE::BSGeometry* geometry,
            std::uint32_t renderMode,
            RE::BSShaderAccumulator* accumulator)
        {
            if (!effectRenderPassHookObserved.exchange(
                    true,
                    std::memory_order_acq_rel
                ))
            {
                REX::INFO(
                    "f4ffmpeg BSEffectShaderProperty::GetRenderPasses "
                    "hook reached."
                );
            }

            if (shaderProperty == nullptr)
            {
                return originalEffectGetRenderPasses(
                    shaderProperty,
                    geometry,
                    renderMode,
                    accumulator
                );
            }

            auto* baseTexture =
                originalEffectGetBaseTexture(
                    shaderProperty
                );

            const char* textureName =
                baseTexture != nullptr
                    ? baseTexture->name.c_str()
                    : nullptr;

            std::shared_ptr<const videoTarget>
                target;

            if (
                textureName != nullptr &&
                *textureName != '\0')
            {
                target =
                    getVideoTargetForTexture(
                        textureName
                    );

                if (target)
                {
                    // This particular fade-node/NIF instance is now proven to
                    // contain a texture f4ffmpeg handles. Sibling TV effects can
                    // be changed without affecting unrelated vanilla TVs.
                    markTvNodeTouched(
                        shaderProperty
                    );
                }
            }

            const bool touched =
                isTvNodeTouched(
                    shaderProperty
                );

            workshopTvEffectKind effectKind =
                workshopTvEffectKind::other;

            if (touched)
            {
                effectKind =
                    classifyWorkshopTvEffect(
                        shaderProperty,
                        geometry,
                        baseTexture
                    );

                diagnoseTouchedTvEffect(
                    shaderProperty,
                    geometry,
                    baseTexture,
                    effectKind
                );

                // Never classify the actual handled screen texture as an effect
                // to remove, even if a mod gives its geometry/material a name
                // containing one of our diagnostic tokens.
                if (
                    !target &&
                    shouldSuppressWorkshopTvEffect(
                        effectKind
                    ))
                {
                    reportWorkshopTvEffectSuppression(
                        effectKind
                    );

                    // Return a stable empty render-pass list instead of mutating
                    // the shared shader property or NIF. Suppression is therefore
                    // draw-local and instance-scoped.
                    return &emptyEffectRenderPasses;
                }
            }

            auto* passes =
                originalEffectGetRenderPasses(
                    shaderProperty,
                    geometry,
                    renderMode,
                    accumulator
                );

            if (
                textureName == nullptr ||
                *textureName == '\0')
            {
                return passes;
            }

            diagnoseResolvedTexturePath(
                "BSEffectShaderProperty::GetRenderPasses",
                textureName
            );

            if (target)
            {
                registerNiTexturePresentationBinding(
                    textureName,
                    baseTexture,
                    "BSEffectShaderProperty::GetRenderPasses"
                );
            }
            else if (
                touched &&
                workshopTvRasterScanSuppressionEnabled &&
                effectKind == workshopTvEffectKind::rasterScan)
            {
                registerNiTextureSuppressionBinding(
                    textureName,
                    baseTexture,
                    "BSEffectShaderProperty::GetRenderPasses"
                );
            }

            return passes;
        }

        bool installEffectTextureHook()
        {
            const auto vtable =
                findEffectShaderPropertyVtable();

            if (!vtable)
            {
                REX::ERROR(
                    "Failed to locate Fallout's BSEffectShaderProperty vtable; "
                    "effect/flipbook texture replacement is unavailable."
                );

                return false;
            }

            const auto getRenderPassesSlot =
                *vtable +
                sizeof(std::uintptr_t) *
                    effectGetRenderPassesVtableIndex;

            const auto getBaseTextureSlot =
                *vtable +
                sizeof(std::uintptr_t) *
                    effectGetBaseTextureVtableIndex;

            const auto renderPassesOriginal =
                readUnaligned<std::uintptr_t>(
                    getRenderPassesSlot
                );

            const auto baseTextureOriginal =
                readUnaligned<std::uintptr_t>(
                    getBaseTextureSlot
                );

            if (
                renderPassesOriginal == 0 ||
                baseTextureOriginal == 0)
            {
                return false;
            }

            originalEffectGetRenderPasses =
                renderPassesOriginal;

            originalEffectGetBaseTexture =
                baseTextureOriginal;

            const auto replacement =
                reinterpret_cast<std::uintptr_t>(
                    &effectGetRenderPassesHook
                );

            REX::INFO(
                "f4ffmpeg BSEffectShaderProperty vtable={}; "
                "GetRenderPasses slot 2B={}, GetBaseTexture slot 39={}.",
                reinterpret_cast<void*>(
                    *vtable
                ),
                reinterpret_cast<void*>(
                    renderPassesOriginal
                ),
                reinterpret_cast<void*>(
                    baseTextureOriginal
                )
            );

            REL::Relocation<std::uintptr_t> effectVtable{
                *vtable
            };

            effectVtable.write_vfunc(
                effectGetRenderPassesVtableIndex,
                replacement
            );

            const auto patched =
                readUnaligned<std::uintptr_t>(
                    getRenderPassesSlot
                );

            if (patched != replacement)
            {
                REX::ERROR(
                    "Failed to patch BSEffectShaderProperty::GetRenderPasses."
                );

                return false;
            }

            REX::INFO(
                "f4ffmpeg patched BSEffectShaderProperty "
                "GetRenderPasses slot 2B={}.",
                reinterpret_cast<void*>(
                    patched
                )
            );

            REX::INFO(
                "f4ffmpeg effect/flipbook base-texture interception initialized."
            );

            return true;
        }

        REX::W32::ID3D11ShaderResourceView* getProducedSrv(
            const producedFrame& frame)
        {
            // The decoder now creates the Fallout-device SRV together with the
            // texture. The producedFrame owns both COM references, and the
            // shared_ptr held by the presentation hook keeps them alive through
            // PSSetShaderResources. D3D11 retains its own reference once bound.
            return frame.resourceView;
        }

        using psSetShaderResources_t = void(*)(
            REX::W32::ID3D11DeviceContext*,
            std::uint32_t,
            std::uint32_t,
            REX::W32::ID3D11ShaderResourceView* const*
        );

        REL::Relocation<psSetShaderResources_t>
            originalPSSetShaderResources;

        REX::W32::ID3D11DeviceContext*
            falloutDeviceContext = nullptr;

        void psSetShaderResourcesHook(
            REX::W32::ID3D11DeviceContext* context,
            std::uint32_t startSlot,
            std::uint32_t numViews,
            REX::W32::ID3D11ShaderResourceView* const* views)
        {
            if (
                context == falloutDeviceContext &&
                !psHookObserved.exchange(
                    true,
                    std::memory_order_acq_rel
                ))
            {
                REX::INFO(
                    "f4ffmpeg D3D11 PSSetShaderResources hook reached."
                );
            }

            if (
                context != falloutDeviceContext ||
                views == nullptr ||
                numViews == 0 ||
                !hasPresentationBindings.load(
                    std::memory_order_acquire
                ))
            {
                originalPSSetShaderResources(
                    context,
                    startSlot,
                    numViews,
                    views
                );

                return;
            }

            if (
                numViews >
                    REX::W32::D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
            {
                originalPSSetShaderResources(
                    context,
                    startSlot,
                    numViews,
                    views
                );

                return;
            }

            std::array<
                REX::W32::ID3D11ShaderResourceView*,
                REX::W32::D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>
                replacementViews{};

            std::copy_n(
                views,
                numViews,
                replacementViews.begin()
            );

            bool replacedAny = false;
            bool presentedVideoFrame = false;

            for (
                std::uint32_t index = 0;
                index < numViews;
                ++index)
            {
                auto* vanillaSrv =
                    views[index];

                if (vanillaSrv == nullptr)
                    continue;

                std::shared_ptr<const videoTarget>
                    target;

                bool suppress = false;

                {
                    std::shared_lock lock(
                        bindingMutex
                    );

                    suppress =
                        suppressedVanillaSrvs.find(
                            vanillaSrv
                        ) != suppressedVanillaSrvs.end();

                    if (!suppress)
                    {
                        const auto binding =
                            targetsByVanillaSrv.find(
                                vanillaSrv
                            );

                        if (binding != targetsByVanillaSrv.end())
                        {
                            target =
                                binding->second;
                        }
                    }
                }

                if (suppress)
                {
                    // A null D3D11 SRV is a legal unbound resource. Shader reads
                    // return zero, removing the target-local RasterScan texture
                    // without manufacturing a replacement resource.
                    replacementViews[index] =
                        nullptr;

                    replacedAny = true;

                    if (!rasterScanSuppressionObserved.exchange(
                            true,
                            std::memory_order_acq_rel
                        ))
                    {
                        REX::INFO(
                            "f4ffmpeg target-local workshop-TV raster-scan suppression reached D3D11 presentation."
                        );
                    }

                    continue;
                }

                if (!target)
                    continue;

                const auto frame =
                    target->getLatestFrame();

                if (
                    !frame ||
                    frame->texture == nullptr ||
                    frame->resourceView == nullptr)
                {
                    // No produced frame: use the exact vanilla SRV Fallout
                    // supplied. This is the normal fallback path.
                    continue;
                }

                auto* producedSrv =
                    getProducedSrv(
                        *frame
                    );

                if (producedSrv == nullptr)
                    continue;

                replacementViews[index] =
                    producedSrv;

                replacedAny = true;
                presentedVideoFrame = true;
            }

            if (
                presentedVideoFrame &&
                !presentationObserved.exchange(
                    true,
                    std::memory_order_acq_rel
                ))
            {
                REX::INFO(
                    "f4ffmpeg D3D11 produced-frame presentation reached."
                );
            }

            originalPSSetShaderResources(
                context,
                startSlot,
                numViews,
                replacedAny
                    ? replacementViews.data()
                    : views
            );
        }

        bool installPresentationHook()
        {
            auto* rendererData =
                RE::BSGraphics::GetRendererData();

            if (
                rendererData == nullptr ||
                rendererData->context == nullptr)
            {
                REX::ERROR(
                    "Cannot install f4ffmpeg presentation hook: "
                    "Fallout D3D11 context is unavailable."
                );

                return false;
            }

            falloutDeviceContext =
                rendererData->context;

            const auto vtableAddress =
                *reinterpret_cast<std::uintptr_t*>(
                    falloutDeviceContext
                );

            if (vtableAddress == 0)
            {
                REX::ERROR(
                    "Cannot install f4ffmpeg presentation hook: "
                    "D3D11 context vtable is unavailable."
                );

                return false;
            }

            REL::Relocation<std::uintptr_t>
                vtable{
                    vtableAddress
                };

            const auto original =
                *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() +
                    sizeof(void*) *
                        psSetShaderResourcesVtableIndex
                );

            if (original == 0)
            {
                REX::ERROR(
                    "Cannot install f4ffmpeg presentation hook: "
                    "PSSetShaderResources target is unavailable."
                );

                return false;
            }

            originalPSSetShaderResources =
                original;

            REX::INFO(
                "f4ffmpeg D3D11 context vtable={}; "
                "PSSetShaderResources original={}.",
                reinterpret_cast<void*>(vtable.address()),
                reinterpret_cast<void*>(original)
            );

            vtable.write_vfunc(
                psSetShaderResourcesVtableIndex,
                psSetShaderResourcesHook
            );

            const auto patched =
                *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() +
                    sizeof(void*) *
                        psSetShaderResourcesVtableIndex
                );

            REX::INFO(
                "f4ffmpeg D3D11 PSSetShaderResources patched={}.",
                reinterpret_cast<void*>(patched)
            );

            REX::INFO(
                "f4ffmpeg D3D11 texture presentation hook initialized."
            );

            return true;
        }

        bool installTextureHooks()
        {
            REL::Relocation<std::uintptr_t>
                vtable{
                    RE::BSShaderTextureSet::VTABLE[0]
                };

            const auto filenameOriginal =
                *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() +
                    sizeof(void*) * 0x29
                );

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
                filenameOriginal == 0 ||
                prefetchedOriginal == 0 ||
                ordinaryOriginal == 0)
            {
                REX::ERROR(
                    "Failed to resolve f4ffmpeg BSShaderTextureSet "
                    "interception targets."
                );

                return false;
            }

            originalGetTextureFilename =
                filenameOriginal;

            originalGetTexturePrefetched =
                prefetchedOriginal;

            originalGetTexture =
                ordinaryOriginal;

            REX::INFO(
                "f4ffmpeg BSShaderTextureSet vtable={}; "
                "original slots 29={}, 2A={}, 2B={}.",
                reinterpret_cast<void*>(vtable.address()),
                reinterpret_cast<void*>(filenameOriginal),
                reinterpret_cast<void*>(prefetchedOriginal),
                reinterpret_cast<void*>(ordinaryOriginal)
            );

            vtable.write_vfunc(
                0x29,
                getTextureFilenameHook
            );

            vtable.write_vfunc(
                0x2A,
                getTexturePrefetchedHook
            );

            vtable.write_vfunc(
                0x2B,
                getTextureHook
            );

            const auto filenamePatched =
                *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() +
                    sizeof(void*) * 0x29
                );

            const auto prefetchedPatched =
                *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() +
                    sizeof(void*) * 0x2A
                );

            const auto ordinaryPatched =
                *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() +
                    sizeof(void*) * 0x2B
                );

            REX::INFO(
                "f4ffmpeg patched BSShaderTextureSet slots "
                "29={}, 2A={}, 2B={}.",
                reinterpret_cast<void*>(filenamePatched),
                reinterpret_cast<void*>(prefetchedPatched),
                reinterpret_cast<void*>(ordinaryPatched)
            );

            REX::INFO(
                "f4ffmpeg BSShaderTextureSet interception initialized."
            );

            return true;
        }
    }

    std::shared_ptr<const producedFrame>
    videoTarget::getLatestFrame() const
    {
        std::shared_ptr<manager> currentPlayback;

        {
            std::shared_lock lock(
                playbackMutex
            );

            currentPlayback =
                playback;
        }

        if (!currentPlayback)
            return nullptr;

        return currentPlayback->getLatestFrame();
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

        std::scoped_lock lock(
            targetRegistryMutex
        );

        if (const auto existing =
                targetsByTexture.find(
                    textureKey
                );
            existing != targetsByTexture.end())
        {
            return existing->second;
        }

        // Targets may be discovered before post-load manager dispatch. Keep
        // the binding alive with a null playback pointer; dispatchVideoManagers()
        // attaches the manager later without requiring Bethesda to reacquire the
        // texture.
        auto target =
            std::make_shared<videoTarget>();

        target->mode =
            resolved->mode;

        target->texturePath =
            resolved->texturePath;

        target->videoPath =
            resolved->videoPath;

        target->playbackSettings =
            resolved->playbackSettings;

        {
            const std::string videoKey =
                lowercasePath(
                    resolved->videoPath
                );

            std::scoped_lock playbackLock(
                playbackRegistryMutex
            );

            if (const auto playback =
                    playbackByVideo.find(
                        videoKey
                    );
                playback != playbackByVideo.end())
            {
                target->playback =
                    playback->second;
            }
        }

        targetsByTexture.emplace(
            textureKey,
            target
        );

        return target;
    }

    bool dispatchVideoManagers()
    {
        std::scoped_lock dispatchLock(
            dispatchMutex
        );

        struct managerDispatchEntry
        {
            std::string videoPath;
            videoPlaybackSettings playbackSettings;
        };

        std::unordered_set<std::string>
            videoKeys;

        std::vector<managerDispatchEntry>
            videos;

        videoKeys.reserve(
            replacementIndex.size()
        );

        for (const auto& [textureKey, replacement] :
             replacementIndex)
        {
            (void)textureKey;

            const std::string videoKey =
                lowercasePath(
                    replacement.videoPath
                );

            if (videoKeys.emplace(videoKey).second)
            {
                videos.emplace_back(
                    managerDispatchEntry{
                        replacement.videoPath,
                        replacement.playbackSettings
                    }
                );
            }
        }

        if (videos.empty())
        {
            REX::INFO(
                "f4ffmpeg post-load manager dispatch found no indexed videos."
            );

            return true;
        }

        std::size_t runningVideos = 0;
        std::size_t newlyDispatched = 0;

        for (const auto& video : videos)
        {
            const std::string& videoPath =
                video.videoPath;

            const std::string videoKey =
                lowercasePath(
                    videoPath
                );

            std::shared_ptr<manager> playback;

            {
                std::scoped_lock playbackLock(
                    playbackRegistryMutex
                );

                if (const auto existing =
                        playbackByVideo.find(
                            videoKey
                        );
                    existing != playbackByVideo.end())
                {
                    playback =
                        existing->second;
                }
            }

            if (!playback)
            {
                playback =
                    createManager(
                        videoPath.c_str(),
                        video.playbackSettings.looping
                    );

                if (!playback)
                {
                    REX::WARN(
                        "f4ffmpeg post-load dispatch could not start manager for '{}'.",
                        videoPath
                    );

                    continue;
                }

                {
                    std::scoped_lock playbackLock(
                        playbackRegistryMutex
                    );

                    playbackByVideo.emplace(
                        videoKey,
                        playback
                    );
                }

                ++newlyDispatched;

                REX::INFO(
                    "f4ffmpeg post-load dispatched manager for loose replacement '{}'.",
                    videoPath
                );
            }

            playback->setLooping(
                video.playbackSettings.looping
            );

            if (
                video.playbackSettings.shuffle ||
                !video.playbackSettings.playlist.empty())
            {
                REX::DEBUG(
                    "f4ffmpeg playback policy for '{}': shuffle={} and {} playlist entry/entries are parsed and bound but not consumed by manager yet.",
                    videoPath,
                    video.playbackSettings.shuffle,
                    video.playbackSettings.playlist.size()
                );
            }

            ++runningVideos;

            // A target may already be registered against Fallout's vanilla SRV
            // from loading activity that occurred before this post-load event.
            // Attach playback now so that existing presentation bindings become
            // live without requiring the texture to be requested again.
            std::scoped_lock targetLock(
                targetRegistryMutex
            );

            for (auto& [textureKey, target] :
                 targetsByTexture)
            {
                (void)textureKey;

                if (
                    !target ||
                    lowercasePath(
                        target->videoPath
                    ) != videoKey)
                {
                    continue;
                }

                std::unique_lock playbackLock(
                    target->playbackMutex
                );

                target->playback =
                    playback;
            }
        }

        REX::INFO(
            "f4ffmpeg post-load manager dispatch complete: {} running, {} newly dispatched, {} indexed video(s).",
            runningVideos,
            newlyDispatched,
            videos.size()
        );

        return runningVideos ==
            videos.size();
    }

    bool initializeNifHandler()
    {
        static std::once_flag initializeOnce;
        static bool initialized = false;

        std::call_once(
            initializeOnce,
            []()
            {
                auto* rendererData =
                    RE::BSGraphics::GetRendererData();

                if (
                    rendererData == nullptr ||
                    !rendererData->initialized ||
                    rendererData->device == nullptr ||
                    rendererData->context == nullptr)
                {
                    REX::ERROR(
                        "f4ffmpeg texture replacement initialization requires "
                        "Fallout graphics to be ready."
                    );

                    return;
                }

                REX::INFO(
                    "Initializing f4ffmpeg texture replacement."
                );

                workshopTvRasterScanSuppressionEnabled =
                    config::disableWorkshopTVRasterScan.GetValue();

                workshopTvStaticSuppressionEnabled =
                    config::disableWorkshopTVStatic.GetValue();

                workshopTvWarpSuppressionEnabled =
                    config::disableWorkshopTVWarp.GetValue();

                REX::INFO(
                    "f4ffmpeg target-local workshop-TV effects: raster scan={}, static/fuzz={}, warp={}.",
                    workshopTvRasterScanSuppressionEnabled
                        ? "disabled"
                        : "preserved",
                    workshopTvStaticSuppressionEnabled
                        ? "disabled"
                        : "preserved",
                    workshopTvWarpSuppressionEnabled
                        ? "disabled"
                        : "preserved"
                );

                if (!buildReplacementIndex())
                {
                    REX::WARN(
                        "f4ffmpeg replacement index encountered filesystem "
                        "errors; successfully indexed entries remain usable."
                    );
                }

                if (!installPresentationHook())
                    return;

                if (!installTextureHooks())
                    return;

                if (!installEffectTextureHook())
                {
                    REX::WARN(
                        "f4ffmpeg effect/flipbook interception could not be "
                        "installed; BSShaderTextureSet replacement remains active."
                    );
                }

                initialized = true;

                REX::INFO(
                    "f4ffmpeg texture replacement initialized."
                );
            }
        );

        return initialized;
    }
}
