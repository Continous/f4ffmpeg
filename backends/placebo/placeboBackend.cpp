#include "placeboBackendApi.h"

#include <array>
#include <cstdint>
#include <mutex>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>

extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <libplacebo/colorspace.h>
#include <libplacebo/d3d11.h>
#include <libplacebo/renderer.h>
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

namespace
{
    const pl_render_params* renderParams(
        std::uint32_t quality)
    {
        switch (quality)
        {
            case 0:
                return &pl_render_fast_params;
            case 2:
                return &pl_render_high_quality_params;
            case 1:
            default:
                return &pl_render_default_params;
        }
    }

    void releaseOutput(
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

    bool createOutput(
        ID3D11Device* device,
        int width,
        int height,
        f4ffmpeg_placebo_output& output)
    {
        if (
            device == nullptr ||
            width <= 0 ||
            height <= 0)
        {
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags =
            D3D11_BIND_RENDER_TARGET |
            D3D11_BIND_SHADER_RESOURCE;

        ID3D11Texture2D* texture = nullptr;
        if (FAILED(device->CreateTexture2D(
                &desc,
                nullptr,
                &texture)))
        {
            return false;
        }

        ID3D11ShaderResourceView* view = nullptr;
        if (FAILED(device->CreateShaderResourceView(
                texture,
                nullptr,
                &view)))
        {
            texture->Release();
            return false;
        }

        output.texture = texture;
        output.resource_view = view;
        output.width = static_cast<std::uint32_t>(width);
        output.height = static_cast<std::uint32_t>(height);
        return true;
    }

    class backendContext
    {
    public:
        bool convert(
            ID3D11Device* device,
            const AVFrame& source,
            std::uint32_t quality,
            f4ffmpeg_placebo_output& output)
        {
            std::scoped_lock lock(mutex);

            output = {};

            if (!initialize(device))
                return false;

            const auto format =
                static_cast<AVPixelFormat>(source.format);

            if (!pl_test_pixfmt(d3d11->gpu, format))
                return false;

            if (!createOutput(
                    device,
                    source.width,
                    source.height,
                    output))
            {
                return false;
            }

            pl_frame image{};
            pl_avframe_params mapParams{};
            mapParams.frame = &source;
            mapParams.tex = sourceTextures.data();
            mapParams.map_dovi = true;

            if (!pl_map_avframe_ex(
                    d3d11->gpu,
                    &image,
                    &mapParams))
            {
                releaseOutput(output);
                return false;
            }

            pl_d3d11_wrap_params wrapParams{};
            wrapParams.tex =
                static_cast<ID3D11Resource*>(
                    static_cast<ID3D11Texture2D*>(
                        output.texture
                    )
                );

            pl_tex targetTexture =
                pl_d3d11_wrap(
                    d3d11->gpu,
                    &wrapParams
                );

            if (targetTexture == nullptr)
            {
                pl_unmap_avframe(
                    d3d11->gpu,
                    &image
                );
                releaseOutput(output);
                return false;
            }

            pl_frame target{};
            target.num_planes = 1;
            target.planes[0].texture = targetTexture;
            target.planes[0].components = 4;
            target.planes[0].component_mapping[0] = 0;
            target.planes[0].component_mapping[1] = 1;
            target.planes[0].component_mapping[2] = 2;
            target.planes[0].component_mapping[3] = 3;
            target.repr = pl_color_repr_rgb;
            target.repr.levels = PL_COLOR_LEVELS_FULL;
            target.repr.alpha = PL_ALPHA_INDEPENDENT;
            target.color = pl_color_space_bt709;
            target.crop = {
                0.0f,
                0.0f,
                static_cast<float>(source.width),
                static_cast<float>(source.height)
            };

            const bool rendered =
                pl_render_image(
                    renderer,
                    &image,
                    &target,
                    renderParams(quality)
                );

            pl_tex_destroy(
                d3d11->gpu,
                &targetTexture
            );
            pl_unmap_avframe(
                d3d11->gpu,
                &image
            );

            if (!rendered)
            {
                releaseOutput(output);
                return false;
            }

            return true;
        }

    private:
        bool initialize(
            ID3D11Device* device)
        {
            if (initialized)
                return device == attachedDevice;

            if (
                initializationFailed ||
                device == nullptr)
            {
                return false;
            }

            pl_log_params logParams{};
            logParams.log_cb = pl_log_simple;
            logParams.log_level = PL_LOG_WARN;

            log = pl_log_create(
                PL_API_VER,
                &logParams
            );

            if (log == nullptr)
            {
                initializationFailed = true;
                return false;
            }

            pl_d3d11_params params{};
            params.device = device;

            d3d11 = pl_d3d11_create(
                log,
                &params
            );

            if (d3d11 == nullptr)
            {
                initializationFailed = true;
                return false;
            }

            renderer = pl_renderer_create(
                log,
                d3d11->gpu
            );

            if (renderer == nullptr)
            {
                initializationFailed = true;
                return false;
            }

            attachedDevice = device;
            initialized = true;
            return true;
        }

        std::mutex mutex;
        ID3D11Device* attachedDevice = nullptr;
        pl_log log = nullptr;
        pl_d3d11 d3d11 = nullptr;
        pl_renderer renderer = nullptr;
        std::array<pl_tex, 4> sourceTextures{};
        bool initialized = false;
        bool initializationFailed = false;
    };

    backendContext& context()
    {
        static auto* instance = new backendContext();
        return *instance;
    }
}

extern "C" __declspec(dllexport)
uint32_t __cdecl f4ffmpeg_placebo_backend_abi(void)
{
    return F4FFMPEG_PLACEBO_BACKEND_ABI;
}

extern "C" __declspec(dllexport)
int __cdecl f4ffmpeg_placebo_convert(
    void* d3d11_device,
    const AVFrame* frame,
    uint32_t quality,
    f4ffmpeg_placebo_output* output)
{
    if (
        d3d11_device == nullptr ||
        frame == nullptr ||
        output == nullptr)
    {
        return 0;
    }

    return context().convert(
        static_cast<ID3D11Device*>(d3d11_device),
        *frame,
        quality,
        *output
    ) ? 1 : 0;
}
