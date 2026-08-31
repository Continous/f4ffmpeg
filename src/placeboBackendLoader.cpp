#include "placeboBackendLoader.h"

#include "decoder.h"
#include "graphics.h"
#include "pch.h"
#include "placeboBackendApi.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace f4ffmpeg
{
namespace
{
    class optionalPlaceboBackend
    {
    public:
        bool convert(
            const AVFrame& frame,
            std::uint32_t quality,
            producedFrame& output)
        {
#if defined(F4FFMPEG_ALLOW_PLACEBO_BACKEND) && F4FFMPEG_ALLOW_PLACEBO_BACKEND
            ensureLoaded();

            if (convertFunction == nullptr)
                return false;

            auto* device = getD3D11Device();
            if (device == nullptr)
                return false;

            f4ffmpeg_placebo_output backendOutput{};

            if (!convertFunction(
                    reinterpret_cast<void*>(device),
                    &frame,
                    quality,
                    &backendOutput))
            {
                return false;
            }

            if (
                backendOutput.texture == nullptr ||
                backendOutput.resource_view == nullptr)
            {
                return false;
            }

            output.texture =
                reinterpret_cast<REX::W32::ID3D11Texture2D*>(
                    backendOutput.texture
                );
            output.resourceView =
                reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(
                    backendOutput.resource_view
                );
            output.width = backendOutput.width;
            output.height = backendOutput.height;

            return true;
#else
            (void)frame;
            (void)quality;
            (void)output;
            return false;
#endif
        }

        bool available()
        {
#if defined(F4FFMPEG_ALLOW_PLACEBO_BACKEND) && F4FFMPEG_ALLOW_PLACEBO_BACKEND
            ensureLoaded();
            return convertFunction != nullptr;
#else
            return false;
#endif
        }

    private:
        void ensureLoaded()
        {
            std::call_once(loadOnce, [this]()
            {
                const auto backendPath = locateBackend();

                module = LoadLibraryExW(
                    backendPath.c_str(),
                    nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
                );

                if (module == nullptr)
                {
                    REX::DEBUG(
                        "Optional libplacebo backend '{}' is not installed; using libswscale.",
                        backendPath.string()
                    );
                    return;
                }

                const auto abi =
                    reinterpret_cast<f4ffmpeg_placebo_abi_fn>(
                        GetProcAddress(
                            module,
                            "f4ffmpeg_placebo_backend_abi"
                        )
                    );

                const auto convert =
                    reinterpret_cast<f4ffmpeg_placebo_convert_fn>(
                        GetProcAddress(
                            module,
                            "f4ffmpeg_placebo_convert"
                        )
                    );

                if (
                    abi == nullptr ||
                    convert == nullptr ||
                    abi() != F4FFMPEG_PLACEBO_BACKEND_ABI)
                {
                    REX::WARN(
                        "Optional f4ffmpeg_placebo.dll has an incompatible or incomplete ABI; using libswscale."
                    );
                    FreeLibrary(module);
                    module = nullptr;
                    return;
                }

                convertFunction = convert;

                REX::INFO(
                    "Optional libplacebo presentation backend loaded from '{}'.",
                    backendPath.string()
                );
            });
        }

        static std::filesystem::path locateBackend()
        {
            HMODULE self = nullptr;
            static const int moduleAnchor = 0;

            if (GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(
                        &moduleAnchor
                    ),
                    &self))
            {
                std::wstring buffer(32768, L'\0');
                const auto length = GetModuleFileNameW(
                    self,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size())
                );

                if (
                    length > 0 &&
                    length < buffer.size())
                {
                    buffer.resize(length);
                    auto path = std::filesystem::path(buffer);
                    return path.parent_path() /
                        L"f4ffmpeg_placebo.dll";
                }
            }

            return std::filesystem::path(
                L"Data\\F4SE\\Plugins\\f4ffmpeg_placebo.dll"
            );
        }

        std::once_flag loadOnce;
        HMODULE module = nullptr;
        f4ffmpeg_placebo_convert_fn convertFunction = nullptr;
    };

    optionalPlaceboBackend& sharedBackend()
    {
        // Intentionally process-lifetime. Avoid DLL unload ordering against the
        // Fallout D3D11 device during process teardown.
        static auto* backend =
            new optionalPlaceboBackend();
        return *backend;
    }
}

bool tryPlaceboBackendConvert(
    const AVFrame& frame,
    std::uint32_t quality,
    producedFrame& output)
{
    return sharedBackend().convert(
        frame,
        quality,
        output
    );
}

bool placeboBackendAvailable()
{
    return sharedBackend().available();
}
}
