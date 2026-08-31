#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>

#include "placeboBackendApi.h"

extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <iterator>

namespace
{
    void releaseBackendOutput(
        f4ffmpeg_placebo_output& output)
    {
        if (output.resource_view != nullptr)
        {
            static_cast<ID3D11ShaderResourceView*>(
                output.resource_view
            )->Release();
        }

        if (output.texture != nullptr)
        {
            static_cast<ID3D11Texture2D*>(
                output.texture
            )->Release();
        }

        output = {};
    }

    bool populateNeutralYuv420p(
        AVFrame& frame)
    {
        if (av_frame_make_writable(&frame) < 0)
            return false;

        for (int y = 0; y < frame.height; ++y)
        {
            std::fill_n(
                frame.data[0] + y * frame.linesize[0],
                frame.width,
                static_cast<std::uint8_t>(96)
            );
        }

        const int chromaWidth = (frame.width + 1) / 2;
        const int chromaHeight = (frame.height + 1) / 2;

        for (int y = 0; y < chromaHeight; ++y)
        {
            std::fill_n(
                frame.data[1] + y * frame.linesize[1],
                chromaWidth,
                static_cast<std::uint8_t>(128)
            );
            std::fill_n(
                frame.data[2] + y * frame.linesize[2],
                chromaWidth,
                static_cast<std::uint8_t>(128)
            );
        }

        return true;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        std::wcerr
            << L"usage: placebo-render-smoke <f4ffmpeg_placebo.dll>\n";
        return 2;
    }

    HMODULE module = LoadLibraryExW(
        argv[1],
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
    );

    if (module == nullptr)
    {
        std::wcerr
            << L"LoadLibraryExW failed: "
            << GetLastError()
            << L"\n";
        return 3;
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
        std::cerr << "backend ABI/symbol validation failed\n";
        FreeLibrary(module);
        return 4;
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immediateContext = nullptr;
    D3D_FEATURE_LEVEL createdFeatureLevel{};

    const D3D_FEATURE_LEVEL requestedLevels[]{
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    const HRESULT deviceResult = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        0,
        requestedLevels,
        static_cast<UINT>(std::size(requestedLevels)),
        D3D11_SDK_VERSION,
        &device,
        &createdFeatureLevel,
        &immediateContext
    );

    if (FAILED(deviceResult) || device == nullptr)
    {
        std::cerr
            << "D3D11CreateDevice(WARP) failed: 0x"
            << std::hex
            << static_cast<unsigned long>(deviceResult)
            << "\n";
        FreeLibrary(module);
        return 5;
    }

    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr)
    {
        immediateContext->Release();
        device->Release();
        FreeLibrary(module);
        return 6;
    }

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 64;
    frame->height = 64;
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->chroma_location = AVCHROMA_LOC_LEFT;

    if (
        av_frame_get_buffer(frame, 32) < 0 ||
        !populateNeutralYuv420p(*frame))
    {
        std::cerr << "could not allocate/populate synthetic AVFrame\n";
        av_frame_free(&frame);
        immediateContext->Release();
        device->Release();
        FreeLibrary(module);
        return 7;
    }

    f4ffmpeg_placebo_output output{};

    const int converted = convert(
        device,
        frame,
        1,
        &output
    );

    if (
        converted == 0 ||
        output.texture == nullptr ||
        output.resource_view == nullptr ||
        output.width != static_cast<std::uint32_t>(frame->width) ||
        output.height != static_cast<std::uint32_t>(frame->height))
    {
        std::cerr << "libplacebo backend did not render the synthetic frame\n";
        releaseBackendOutput(output);
        av_frame_free(&frame);
        immediateContext->Release();
        device->Release();
        FreeLibrary(module);
        return 8;
    }

    releaseBackendOutput(output);
    av_frame_free(&frame);
    immediateContext->Release();
    device->Release();
    FreeLibrary(module);

    std::cout
        << "f4ffmpeg_placebo one-frame D3D11 WARP render smoke passed\n";
    return 0;
}
