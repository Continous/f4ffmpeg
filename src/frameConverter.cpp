#include "frameConverter.h"

#include "config.h"
#include "decoder.h"
#include "graphics.h"
#include "pch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

// libplacebo's headers already provide their own C/C++ linkage guards.
#include <libplacebo/colorspace.h>
#include <libplacebo/d3d11.h>
#include <libplacebo/renderer.h>
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#ifdef ERROR
#undef ERROR
#endif

namespace f4ffmpeg
{
namespace
{
    enum class conversionMode : std::uint8_t
    {
        cheapest,
        balanced,
        quality
    };

    struct conversionPolicy
    {
        const char* name = "balanced";
        conversionMode mode = conversionMode::balanced;
        int swsFlags =
            SWS_BICUBIC |
            SWS_FULL_CHR_H_INT |
            SWS_ACCURATE_RND;
    };

    char asciiLower(char value)
    {
        if (value >= 'A' && value <= 'Z')
            return static_cast<char>(value - 'A' + 'a');
        return value;
    }

    conversionPolicy readConversionPolicy()
    {
        std::string configured =
            config::conversionQuality.GetValue();

        for (auto& character : configured)
            character = asciiLower(character);

        if (
            configured == "cheapest" ||
            configured == "cheap" ||
            configured == "legacy")
        {
            return {
                "cheapest",
                conversionMode::cheapest,
                SWS_BILINEAR
            };
        }

        if (
            configured == "quality" ||
            configured == "high" ||
            configured == "best")
        {
            return {
                "quality",
                conversionMode::quality,
                SWS_LANCZOS |
                    SWS_FULL_CHR_H_INT |
                    SWS_ACCURATE_RND
            };
        }

        if (
            configured != "balanced" &&
            configured != "default" &&
            !configured.empty())
        {
            REX::WARN(
                "Unknown Playback.ConversionQuality '{}'; using balanced conversion.",
                configured
            );
        }

        return {};
    }

    const conversionPolicy& activeConversionPolicy()
    {
        static const conversionPolicy policy = []
        {
            auto selected = readConversionPolicy();

            REX::INFO(
                "f4ffmpeg frame conversion profile: {} (libplacebo primary).",
                selected.name
            );

            return selected;
        }();

        return policy;
    }

    const struct pl_render_params* placeboRenderParams(
        conversionMode mode)
    {
        switch (mode)
        {
            case conversionMode::cheapest:
                return &pl_render_fast_params;

            case conversionMode::quality:
                return &pl_render_high_quality_params;

            case conversionMode::balanced:
            default:
                return &pl_render_default_params;
        }
    }

    bool isRgbPixelFormat(AVPixelFormat format)
    {
        const auto* descriptor =
            av_pix_fmt_desc_get(format);

        return
            descriptor != nullptr &&
            (descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    }

    bool isHardwarePixelFormat(AVPixelFormat format)
    {
        const auto* descriptor =
            av_pix_fmt_desc_get(format);

        return
            descriptor != nullptr &&
            (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
    }

    bool isHdrTransfer(
        AVColorTransferCharacteristic transfer)
    {
        return
            transfer == AVCOL_TRC_SMPTE2084 ||
            transfer == AVCOL_TRC_ARIB_STD_B67;
    }

    void normalizeColorMetadata(AVFrame& frame)
    {
        const auto format =
            static_cast<AVPixelFormat>(frame.format);

        const bool rgb =
            isRgbPixelFormat(format);

        const bool hd =
            frame.width >= 1280 ||
            frame.height > 576;

        const bool hdr =
            isHdrTransfer(frame.color_trc);

        const bool bt2020Hint =
            hdr ||
            frame.color_primaries == AVCOL_PRI_BT2020 ||
            frame.colorspace == AVCOL_SPC_BT2020_NCL ||
            frame.colorspace == AVCOL_SPC_BT2020_CL;

        if (frame.colorspace == AVCOL_SPC_UNSPECIFIED)
        {
            frame.colorspace =
                rgb
                    ? AVCOL_SPC_RGB
                    : (
                        bt2020Hint
                            ? AVCOL_SPC_BT2020_NCL
                            : (
                                hd
                                    ? AVCOL_SPC_BT709
                                    : AVCOL_SPC_SMPTE170M
                            )
                    );
        }

        if (frame.color_primaries == AVCOL_PRI_UNSPECIFIED)
        {
            frame.color_primaries =
                bt2020Hint
                    ? AVCOL_PRI_BT2020
                    : (
                        hd
                            ? AVCOL_PRI_BT709
                            : AVCOL_PRI_SMPTE170M
                    );
        }

        if (frame.color_trc == AVCOL_TRC_UNSPECIFIED)
        {
            frame.color_trc =
                hd
                    ? AVCOL_TRC_BT709
                    : AVCOL_TRC_SMPTE170M;
        }

        if (frame.color_range == AVCOL_RANGE_UNSPECIFIED)
        {
            frame.color_range =
                rgb
                    ? AVCOL_RANGE_JPEG
                    : AVCOL_RANGE_MPEG;
        }

        if (
            frame.chroma_location == AVCHROMA_LOC_UNSPECIFIED &&
            !rgb)
        {
            frame.chroma_location =
                AVCHROMA_LOC_LEFT;
        }
    }

    struct preparedFrame
    {
        AVFrame* frame = nullptr;

        preparedFrame() = default;
        preparedFrame(const preparedFrame&) = delete;
        preparedFrame& operator=(const preparedFrame&) = delete;

        ~preparedFrame()
        {
            av_frame_free(&frame);
        }
    };

    bool prepareFrameForConversion(
        const AVFrame& input,
        preparedFrame& output)
    {
        const auto inputFormat =
            static_cast<AVPixelFormat>(input.format);

        if (isHardwarePixelFormat(inputFormat))
        {
            output.frame = av_frame_alloc();
            if (output.frame == nullptr)
                return false;

            const int transferResult =
                av_hwframe_transfer_data(
                    output.frame,
                    &input,
                    0
                );

            if (transferResult < 0)
            {
                REX::WARN(
                    "Could not transfer hardware decoded frame for presentation conversion: {}.",
                    transferResult
                );
                return false;
            }

            // Preserve all decoder-authored side data first, then explicitly
            // restore the color fields which are important to libplacebo.
            av_frame_copy_props(
                output.frame,
                &input
            );

            output.frame->color_range =
                input.color_range;
            output.frame->color_primaries =
                input.color_primaries;
            output.frame->color_trc =
                input.color_trc;
            output.frame->colorspace =
                input.colorspace;
            output.frame->chroma_location =
                input.chroma_location;
            output.frame->sample_aspect_ratio =
                input.sample_aspect_ratio;
        }
        else
        {
            // Keep the decoder's AVFrame immutable. A shallow clone refs the
            // same buffers while letting us normalize missing metadata locally.
            output.frame = av_frame_clone(&input);
            if (output.frame == nullptr)
                return false;
        }

        if (
            output.frame->width <= 0 ||
            output.frame->height <= 0)
        {
            return false;
        }

        normalizeColorMetadata(*output.frame);
        return true;
    }

    void releaseProducedFrameResources(
        producedFrame& output)
    {
        if (output.resourceView != nullptr)
        {
            output.resourceView->Release();
            output.resourceView = nullptr;
        }

        if (output.texture != nullptr)
        {
            output.texture->Release();
            output.texture = nullptr;
        }

        output.width = 0;
        output.height = 0;
    }

    bool createCanonicalD3D11Frame(
        int width,
        int height,
        producedFrame& output,
        const std::uint8_t* initialPixels = nullptr,
        std::uint32_t initialPitch = 0)
    {
        if (width <= 0 || height <= 0)
            return false;

        auto* device = getD3D11Device();
        if (device == nullptr)
            return false;

        releaseProducedFrameResources(output);

        REX::W32::D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.width =
            static_cast<std::uint32_t>(width);
        textureDesc.height =
            static_cast<std::uint32_t>(height);
        textureDesc.mipLevels = 1;
        textureDesc.arraySize = 1;
        textureDesc.format =
            REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.sampleDesc.count = 1;
        textureDesc.sampleDesc.quality = 0;
        textureDesc.usage =
            REX::W32::D3D11_USAGE_DEFAULT;

        // libplacebo renders into this resource, while Fallout samples it.
        textureDesc.bindFlags =
            REX::W32::D3D11_BIND_RENDER_TARGET |
            REX::W32::D3D11_BIND_SHADER_RESOURCE;
        textureDesc.cpuAccessFlags = 0;
        textureDesc.miscFlags = 0;

        REX::W32::D3D11_SUBRESOURCE_DATA initialData{};
        REX::W32::D3D11_SUBRESOURCE_DATA* initialDataPtr = nullptr;

        if (initialPixels != nullptr)
        {
            initialData.sysMem = initialPixels;
            initialData.sysMemPitch = initialPitch;
            initialData.sysMemSlicePitch =
                initialPitch *
                static_cast<std::uint32_t>(height);
            initialDataPtr = &initialData;
        }

        REX::W32::ID3D11Texture2D* texture = nullptr;

        const auto textureResult =
            device->CreateTexture2D(
                &textureDesc,
                initialDataPtr,
                &texture
            );

        if (
            textureResult < 0 ||
            texture == nullptr)
        {
            REX::ERROR(
                "CreateTexture2D for canonical produced frame failed: 0x{:08X}.",
                static_cast<std::uint32_t>(textureResult)
            );
            return false;
        }

        REX::W32::ID3D11ShaderResourceView* resourceView =
            nullptr;

        const auto viewResult =
            device->CreateShaderResourceView(
                texture,
                nullptr,
                &resourceView
            );

        if (
            viewResult < 0 ||
            resourceView == nullptr)
        {
            REX::ERROR(
                "CreateShaderResourceView for canonical produced frame failed: 0x{:08X}.",
                static_cast<std::uint32_t>(viewResult)
            );
            texture->Release();
            return false;
        }

        output.texture = texture;
        output.resourceView = resourceView;
        output.width =
            static_cast<std::uint32_t>(width);
        output.height =
            static_cast<std::uint32_t>(height);

        return true;
    }

    class placeboConverter
    {
    public:
        bool convert(
            const AVFrame& source,
            const conversionPolicy& policy,
            producedFrame& output)
        {
            std::scoped_lock lock(mutex);

            if (!initialize())
                return false;

            const auto format =
                static_cast<AVPixelFormat>(source.format);

            // pl_map_avframe_ex currently fails cleanly for source formats the
            // D3D11 GPU cannot represent. This is why swscale remains a fallback.
            if (!pl_test_pixfmt(d3d11->gpu, format))
            {
                REX::DEBUG(
                    "libplacebo cannot upload decoded pixel format '{}'.",
                    av_get_pix_fmt_name(format)
                        ? av_get_pix_fmt_name(format)
                        : "unknown"
                );
                return false;
            }

            if (!createCanonicalD3D11Frame(
                    source.width,
                    source.height,
                    output))
            {
                return false;
            }

            struct pl_frame image{};
            struct pl_avframe_params mapParams{};
            mapParams.frame = &source;
            mapParams.tex = sourceTextures.data();
            mapParams.map_dovi = true;

            if (!pl_map_avframe_ex(
                    d3d11->gpu,
                    &image,
                    &mapParams))
            {
                REX::WARN(
                    "libplacebo could not map decoded AVFrame format '{}'.",
                    av_get_pix_fmt_name(format)
                        ? av_get_pix_fmt_name(format)
                        : "unknown"
                );
                releaseProducedFrameResources(output);
                return false;
            }

            auto unmapImage = [&]
            {
                pl_unmap_avframe(
                    d3d11->gpu,
                    &image
                );
            };

            struct pl_d3d11_wrap_params wrapParams{};
            wrapParams.tex =
                reinterpret_cast<ID3D11Resource*>(
                    output.texture
                );

            pl_tex targetTexture =
                pl_d3d11_wrap(
                    d3d11->gpu,
                    &wrapParams
                );

            if (targetTexture == nullptr)
            {
                REX::WARN(
                    "libplacebo could not wrap the canonical Fallout D3D11 target texture."
                );
                unmapImage();
                releaseProducedFrameResources(output);
                return false;
            }

            struct pl_frame target{};
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

            // Canonical Fallout presentation output is SDR BT.709 RGBA8.
            // If the source is HDR/wide-gamut, pl_render_image performs the
            // corresponding tone/gamut mapping as part of this render.
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
                    placeboRenderParams(policy.mode)
                );

            pl_tex_destroy(
                d3d11->gpu,
                &targetTexture
            );
            unmapImage();

            if (!rendered)
            {
                const auto errors =
                    pl_renderer_get_errors(renderer);

                REX::WARN(
                    "libplacebo failed to render decoded frame (errors=0x{:X}).",
                    static_cast<std::uint32_t>(errors.errors)
                );

                releaseProducedFrameResources(output);
                return false;
            }

            if (!firstSuccessReported.exchange(
                    true,
                    std::memory_order_acq_rel))
            {
                REX::INFO(
                    "libplacebo D3D11 presentation conversion active: profile={}, target=BT.709 full-range RGBA8.",
                    policy.name
                );
            }

            return true;
        }

    private:
        bool initialize()
        {
            if (initialized)
                return true;

            if (initializationFailed)
                return false;

            auto* rexDevice =
                getD3D11Device();

            if (rexDevice == nullptr)
            {
                initializationFailed = true;
                return false;
            }

            // CommonLibF4 mirrors the D3D11 COM ABI under REX::W32. The object
            // itself is the native Windows ID3D11Device, so isolate the ABI cast
            // here at the libplacebo boundary.
            auto* nativeDevice =
                reinterpret_cast<ID3D11Device*>(
                    rexDevice
                );

            struct pl_log_params logParams{};
            logParams.log_cb = pl_log_simple;
            logParams.log_level = PL_LOG_WARN;

            log =
                pl_log_create(
                    PL_API_VER,
                    &logParams
                );

            if (log == nullptr)
            {
                initializationFailed = true;
                return false;
            }

            struct pl_d3d11_params d3dParams{};
            d3dParams.device = nativeDevice;

            d3d11 =
                pl_d3d11_create(
                    log,
                    &d3dParams
                );

            if (d3d11 == nullptr)
            {
                REX::ERROR(
                    "libplacebo failed to attach to Fallout's D3D11 device."
                );
                initializationFailed = true;
                return false;
            }

            renderer =
                pl_renderer_create(
                    log,
                    d3d11->gpu
                );

            if (renderer == nullptr)
            {
                REX::ERROR(
                    "libplacebo failed to create its high-level renderer."
                );
                initializationFailed = true;
                return false;
            }

            initialized = true;
            return true;
        }

        // Intentionally process-lifetime state. Fallout owns the underlying
        // D3D11 device; avoiding static destruction prevents teardown ordering
        // from releasing libplacebo objects after the game's device is gone.
        std::mutex mutex;
        pl_log log = nullptr;
        pl_d3d11 d3d11 = nullptr;
        pl_renderer renderer = nullptr;
        std::array<pl_tex, 4> sourceTextures{};
        bool initialized = false;
        bool initializationFailed = false;
        std::atomic_bool firstSuccessReported = false;
    };

    placeboConverter& sharedPlaceboConverter()
    {
        // Deliberately leaked at process shutdown; see placeboConverter state
        // comment above. This is one tiny GPU/context wrapper for plugin life.
        static auto* converter =
            new placeboConverter();
        return *converter;
    }

    int swscaleColorSpace(
        AVColorSpace colorSpace)
    {
        switch (colorSpace)
        {
            case AVCOL_SPC_BT709:
                return SWS_CS_ITU709;

            case AVCOL_SPC_FCC:
                return SWS_CS_FCC;

            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M:
                return SWS_CS_ITU601;

            case AVCOL_SPC_SMPTE240M:
                return SWS_CS_SMPTE240M;

            case AVCOL_SPC_BT2020_NCL:
            case AVCOL_SPC_BT2020_CL:
                return SWS_CS_BT2020;

            default:
                return SWS_CS_DEFAULT;
        }
    }

    bool convertWithSwscaleFallback(
        const AVFrame& source,
        const conversionPolicy& policy,
        producedFrame& output)
    {
        const auto sourceFormat =
            static_cast<AVPixelFormat>(source.format);

        const int stride =
            source.width * 4;

        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(stride) *
            static_cast<std::size_t>(source.height)
        );

        SwsContext* sws =
            sws_getContext(
                source.width,
                source.height,
                sourceFormat,
                source.width,
                source.height,
                AV_PIX_FMT_RGBA,
                policy.swsFlags,
                nullptr,
                nullptr,
                nullptr
            );

        if (sws == nullptr)
            return false;

        const int* coefficients =
            sws_getCoefficients(
                swscaleColorSpace(source.colorspace)
            );

        sws_setColorspaceDetails(
            sws,
            coefficients,
            source.color_range == AVCOL_RANGE_JPEG ? 1 : 0,
            sws_getCoefficients(SWS_CS_ITU709),
            1,
            0,
            1 << 16,
            1 << 16
        );

        std::uint8_t* outputData[4]{
            pixels.data(),
            nullptr,
            nullptr,
            nullptr
        };

        int outputLinesize[4]{
            stride,
            0,
            0,
            0
        };

        const int convertedHeight =
            sws_scale(
                sws,
                source.data,
                source.linesize,
                0,
                source.height,
                outputData,
                outputLinesize
            );

        sws_freeContext(sws);

        if (convertedHeight != source.height)
            return false;

        return createCanonicalD3D11Frame(
            source.width,
            source.height,
            output,
            pixels.data(),
            static_cast<std::uint32_t>(stride)
        );
    }
}

bool convertFrameForPresentation(
    const AVFrame& frame,
    producedFrame& output)
{
    preparedFrame prepared;

    if (!prepareFrameForConversion(
            frame,
            prepared))
    {
        return false;
    }

    const auto& policy =
        activeConversionPolicy();

    if (sharedPlaceboConverter().convert(
            *prepared.frame,
            policy,
            output))
    {
        return true;
    }

    static std::atomic_bool fallbackReported = false;
    static std::atomic_bool hdrFallbackReported = false;

    if (
        isHdrTransfer(prepared.frame->color_trc) &&
        !hdrFallbackReported.exchange(
            true,
            std::memory_order_acq_rel))
    {
        REX::WARN(
            "libplacebo rejected an HDR frame; libswscale fallback cannot reproduce libplacebo tone/gamut mapping and is compatibility-only."
        );
    }

    if (!fallbackReported.exchange(
            true,
            std::memory_order_acq_rel))
    {
        REX::WARN(
            "libplacebo presentation conversion failed or rejected the source format; using metadata-aware libswscale fallback."
        );
    }

    return convertWithSwscaleFallback(
        *prepared.frame,
        policy,
        output
    );
}
}
