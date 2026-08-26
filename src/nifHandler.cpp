#include "pch.h"
#include "nifHandler.h"
#include "manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
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
#include <RE/B/BSShaderTextureSet.h>
#include <RE/B/BSTextureSet.h>
#include <RE/N/NiPointer.h>
#include <RE/N/NiTexture.h>
#include <REL/Module.h>
#include <REL/Relocation.h>
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

        constexpr std::string_view videoExtension =
            ".mov";

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
        };

        struct resolvedVideoTarget
        {
            videoTargetMode mode =
                videoTargetMode::vanillaOverride;

            std::string texturePath;
            std::string videoPath;
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

            if (
                modePriority(replacement.mode) >
                modePriority(existing->second.mode))
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
                "Scanning loose f4ffmpeg videos at '{}'.",
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
                replacement->second.videoPath
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
                "f4ffmpeg indexed texture '{}' reached acquisition, "
                "but presentation binding is not ready: {}.",
                texturePath
                    ? texturePath
                    : "<null>",
                reason
                    ? reason
                    : "unknown"
            );
        }

        bool registerNiTexturePresentationBinding(
            const char* texturePath,
            RE::NiTexture* niTexture,
            const char* sourceName)
        {
            const auto target =
                getVideoTargetForTexture(
                    texturePath
                );

            // A stable NiTexture with no indexed target needs no further work.
            if (!target)
                return true;

            if (niTexture == nullptr)
            {
                logBindingFailureOnce(
                    texturePath,
                    "NiTexture pointer is null"
                );

                return false;
            }

            if (niTexture->rendererTexture == nullptr)
            {
                logBindingFailureOnce(
                    texturePath,
                    "NiTexture has no rendererTexture yet"
                );

                return false;
            }

            const auto* rendererTexture =
                reinterpret_cast<
                    const rendererTexturePrefix*>(
                        niTexture->rendererTexture
                    );

            auto* vanillaSrv =
                rendererTexture->resourceView;

            if (vanillaSrv == nullptr)
            {
                logBindingFailureOnce(
                    texturePath,
                    "rendererTexture has no shader resource view"
                );

                return false;
            }

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
            const REL::Segment& segment,
            std::uintptr_t address,
            std::size_t size)
        {
            if (size > segment.size())
                return false;

            const auto begin =
                segment.address();

            const auto end =
                begin + segment.size();

            return
                address >= begin &&
                address <= end - size;
        }

        std::optional<std::uintptr_t>
        findBytesInSegment(
            const REL::Segment& segment,
            std::string_view bytes)
        {
            if (
                bytes.empty() ||
                bytes.size() + 1 > segment.size())
            {
                return std::nullopt;
            }

            const auto* begin =
                reinterpret_cast<const char*>(
                    segment.address()
                );

            const auto* end =
                begin + segment.size() -
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
            auto& module =
                REL::Module::get();

            const auto data =
                module.segment(
                    REL::Segment::Name::data
                );

            const auto rdata =
                module.segment(
                    REL::Segment::Name::rdata
                );

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
                        segment->address() +
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
            auto& module =
                REL::Module::get();

            const auto moduleBase =
                module.base();

            const auto rdata =
                module.segment(
                    REL::Segment::Name::rdata
                );

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
                rdata.address();

            const auto rdataEnd =
                rdataBegin +
                rdata.size();

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

        std::shared_mutex settledEffectTexturesMutex;

        std::unordered_set<RE::NiTexture*>
            settledEffectTextures;

        bool effectTextureAlreadySettled(
            RE::NiTexture* texture)
        {
            std::shared_lock lock(
                settledEffectTexturesMutex
            );

            return settledEffectTextures.find(
                texture
            ) != settledEffectTextures.end();
        }

        void markEffectTextureSettled(
            RE::NiTexture* texture)
        {
            std::unique_lock lock(
                settledEffectTexturesMutex
            );

            settledEffectTextures.emplace(
                texture
            );
        }

        RE::BSShaderProperty::RenderPassArray*
        effectGetRenderPassesHook(
            RE::BSShaderProperty* shaderProperty,
            RE::BSGeometry* geometry,
            std::uint32_t renderMode,
            RE::BSShaderAccumulator* accumulator)
        {
            auto* passes =
                originalEffectGetRenderPasses(
                    shaderProperty,
                    geometry,
                    renderMode,
                    accumulator
                );

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
                return passes;

            auto* baseTexture =
                originalEffectGetBaseTexture(
                    shaderProperty
                );

            if (baseTexture == nullptr)
                return passes;

            if (effectTextureAlreadySettled(
                    baseTexture
                ))
            {
                return passes;
            }

            const char* textureName =
                baseTexture->name.c_str();

            if (
                textureName == nullptr ||
                *textureName == '\0')
            {
                markEffectTextureSettled(
                    baseTexture
                );

                return passes;
            }

            diagnoseResolvedTexturePath(
                "BSEffectShaderProperty::GetRenderPasses",
                textureName
            );

            // A false result means this *is* an indexed texture but Bethesda has
            // not attached its renderer texture/SRV yet. Leave it unsettled so a
            // later render-pass request retries instead of permanently falling
            // back to vanilla.
            if (registerNiTexturePresentationBinding(
                    textureName,
                    baseTexture,
                    "BSEffectShaderProperty::GetRenderPasses"
                ))
            {
                markEffectTextureSettled(
                    baseTexture
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

            REL::safe_write(
                getRenderPassesSlot,
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

                {
                    std::shared_lock lock(
                        bindingMutex
                    );

                    const auto binding =
                        targetsByVanillaSrv.find(
                            vanillaSrv
                        );

                    if (binding == targetsByVanillaSrv.end())
                        continue;

                    target =
                        binding->second;
                }

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
            }

            if (
                replacedAny &&
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

        std::unordered_set<std::string>
            videoKeys;

        std::vector<std::string>
            videoPaths;

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
                videoPaths.emplace_back(
                    replacement.videoPath
                );
            }
        }

        if (videoPaths.empty())
        {
            REX::INFO(
                "f4ffmpeg post-load manager dispatch found no indexed videos."
            );

            return true;
        }

        std::size_t runningVideos = 0;
        std::size_t newlyDispatched = 0;

        for (const auto& videoPath : videoPaths)
        {
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
                        true
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
            videoPaths.size()
        );

        return runningVideos ==
            videoPaths.size();
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
