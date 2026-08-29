#include "frameConverter.h"

#include "config.h"
#include "decoder.h"
#include "graphics.h"
#include "pch.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#ifdef ERROR
#undef ERROR
#endif

namespace f4ffmpeg
{
namespace
{
    struct conversionPolicy
    {
        const char* name = "balanced";
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
                SWS_FAST_BILINEAR
            };
        }

        if (
            configured == "quality" ||
            configured == "high" ||
            configured == "best")
        {
            return {
                "quality",
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
            auto selected =
                readConversionPolicy();

            REX::INFO(
                "f4ffmpeg frame conversion profile: {} (libswscale).",
                selected.name
            );

            return selected;
        }();

        return policy;
    }

    bool isRgbPixelFormat(
        AVPixelFormat format)
    {
        const auto* descriptor =
            av_pix_fmt_desc_get(format);

        return
            descriptor != nullptr &&
            (
                descriptor->flags &
                AV_PIX_FMT_FLAG_RGB
            ) != 0;
    }

    bool isHardwarePixelFormat(
        AVPixelFormat format)
    {
        const auto* descriptor =
            av_pix_fmt_desc_get(format);

        return
            descriptor != nullptr &&
            (
                descriptor->flags &
                AV_PIX_FMT_FLAG_HWACCEL
            ) != 0;
    }

    bool isHdrTransfer(
        AVColorTransferCharacteristic transfer)
    {
        return
            transfer == AVCOL_TRC_SMPTE2084 ||
            transfer == AVCOL_TRC_ARIB_STD_B67;
    }

    bool isWideGamutPrimaries(
        AVColorPrimaries primaries)
    {
        return
            primaries == AVCOL_PRI_BT2020 ||
            primaries == AVCOL_PRI_SMPTE431 ||
            primaries == AVCOL_PRI_SMPTE432;
    }

    void normalizeColorMetadata(
        AVFrame& frame)
    {
        const auto format =
            static_cast<AVPixelFormat>(
                frame.format
            );

        const bool rgb =
            isRgbPixelFormat(format);

        const bool hd =
            frame.width >= 1280 ||
            frame.height > 576;

        const bool bt2020Hint =
            frame.color_primaries == AVCOL_PRI_BT2020 ||
            frame.colorspace == AVCOL_SPC_BT2020_NCL ||
            frame.colorspace == AVCOL_SPC_BT2020_CL ||
            isHdrTransfer(frame.color_trc);

        if (
            frame.colorspace ==
            AVCOL_SPC_UNSPECIFIED)
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

        if (
            frame.color_primaries ==
            AVCOL_PRI_UNSPECIFIED)
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

        if (
            frame.color_trc ==
            AVCOL_TRC_UNSPECIFIED)
        {
            frame.color_trc =
                hd
                    ? AVCOL_TRC_BT709
                    : AVCOL_TRC_SMPTE170M;
        }

        if (
            frame.color_range ==
            AVCOL_RANGE_UNSPECIFIED)
        {
            frame.color_range =
                rgb
                    ? AVCOL_RANGE_JPEG
                    : AVCOL_RANGE_MPEG;
        }

        if (
            frame.chroma_location ==
                AVCHROMA_LOC_UNSPECIFIED &&
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

        preparedFrame(
            const preparedFrame&
        ) = delete;

        preparedFrame& operator=(
            const preparedFrame&
        ) = delete;

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
            static_cast<AVPixelFormat>(
                input.format
            );

        if (
            isHardwarePixelFormat(
                inputFormat
            ))
        {
            output.frame =
                av_frame_alloc();

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

            // The transfer establishes the software pixel layout. Copy source
            // properties/side-data afterwards so decoder-authored color
            // metadata survives the hardware -> system-memory boundary.
            const int propertyResult =
                av_frame_copy_props(
                    output.frame,
                    &input
                );

            if (propertyResult < 0)
            {
                REX::DEBUG(
                    "Could not copy all decoded-frame properties after hardware transfer: {}.",
                    propertyResult
                );
            }

            // Explicitly preserve the fields the conversion policy relies on.
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
            // Keep the decoder-owned frame immutable while permitting local
            // normalization of unspecified metadata.
            output.frame =
                av_frame_clone(
                    &input
                );

            if (output.frame == nullptr)
                return false;
        }

        if (
            output.frame->width <= 0 ||
            output.frame->height <= 0)
        {
            return false;
        }

        normalizeColorMetadata(
            *output.frame
        );

        return true;
    }

    void releaseProducedFrameResources(
        producedFrame& output)
    {
        if (
            output.resourceView !=
            nullptr)
        {
            output.resourceView->Release();
            output.resourceView = nullptr;
        }

        if (
            output.texture !=
            nullptr)
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
        const std::uint8_t* pixels,
        std::uint32_t pitch,
        producedFrame& output)
    {
        if (
            width <= 0 ||
            height <= 0 ||
            pixels == nullptr ||
            pitch == 0)
        {
            return false;
        }

        auto* device =
            getD3D11Device();

        if (device == nullptr)
            return false;

        releaseProducedFrameResources(
            output
        );

        REX::W32::D3D11_TEXTURE2D_DESC
            textureDesc{};

        textureDesc.width =
            static_cast<std::uint32_t>(
                width
            );

        textureDesc.height =
            static_cast<std::uint32_t>(
                height
            );

        textureDesc.mipLevels = 1;
        textureDesc.arraySize = 1;

        textureDesc.format =
            REX::W32::
                DXGI_FORMAT_R8G8B8A8_UNORM;

        textureDesc.sampleDesc.count = 1;
        textureDesc.sampleDesc.quality = 0;

        textureDesc.usage =
            REX::W32::
                D3D11_USAGE_DEFAULT;

        // The canonical frame is complete before upload; Fallout only needs
        // to sample it.
        textureDesc.bindFlags =
            REX::W32::
                D3D11_BIND_SHADER_RESOURCE;

        textureDesc.cpuAccessFlags = 0;
        textureDesc.miscFlags = 0;

        REX::W32::D3D11_SUBRESOURCE_DATA
            initialData{};

        initialData.sysMem =
            pixels;

        initialData.sysMemPitch =
            pitch;

        initialData.sysMemSlicePitch =
            pitch *
            static_cast<std::uint32_t>(
                height
            );

        REX::W32::ID3D11Texture2D*
            texture = nullptr;

        const auto textureResult =
            device->CreateTexture2D(
                &textureDesc,
                &initialData,
                &texture
            );

        if (
            textureResult < 0 ||
            texture == nullptr)
        {
            REX::ERROR(
                "CreateTexture2D for canonical produced frame failed: 0x{:08X}.",
                static_cast<std::uint32_t>(
                    textureResult
                )
            );

            return false;
        }

        REX::W32::ID3D11ShaderResourceView*
            resourceView = nullptr;

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
                static_cast<std::uint32_t>(
                    viewResult
                )
            );

            texture->Release();
            return false;
        }

        output.texture =
            texture;

        output.resourceView =
            resourceView;

        output.width =
            static_cast<std::uint32_t>(
                width
            );

        output.height =
            static_cast<std::uint32_t>(
                height
            );

        return true;
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

    void reportUnsupportedColorManagement(
        const AVFrame& source)
    {
        static std::atomic_bool
            hdrWarningReported = false;

        static std::atomic_bool
            wideGamutWarningReported = false;

        if (
            isHdrTransfer(
                source.color_trc
            ) &&
            !hdrWarningReported.exchange(
                true,
                std::memory_order_acq_rel
            ))
        {
            REX::WARN(
                "HDR input detected. f4ffmpeg currently uses libswscale only; PQ/HLG tone mapping is unsupported and HDR output colors/luminance may be inaccurate."
            );
        }

        if (
            isWideGamutPrimaries(
                source.color_primaries
            ) &&
            !wideGamutWarningReported.exchange(
                true,
                std::memory_order_acq_rel
            ))
        {
            REX::WARN(
                "Wide-gamut input detected. libswscale will perform pixel/matrix conversion but f4ffmpeg currently does not gamut-map the source primaries to BT.709."
            );
        }
    }

    bool convertWithSwscale(
        const AVFrame& source,
        const conversionPolicy& policy,
        producedFrame& output)
    {
        const auto sourceFormat =
            static_cast<AVPixelFormat>(
                source.format
            );

        if (
            !sws_isSupportedInput(
                sourceFormat
            ) ||
            !sws_isSupportedOutput(
                AV_PIX_FMT_RGBA
            ))
        {
            const char* formatName =
                av_get_pix_fmt_name(
                    sourceFormat
                );

            REX::WARN(
                "libswscale cannot convert decoded pixel format '{}'.",
                formatName
                    ? formatName
                    : "unknown"
            );

            return false;
        }

        if (
            source.width >
                std::numeric_limits<int>::max() / 4)
        {
            return false;
        }

        const int stride =
            source.width * 4;

        const auto bufferSize =
            static_cast<std::size_t>(
                stride
            ) *
            static_cast<std::size_t>(
                source.height
            );

        if (
            source.height <= 0 ||
            bufferSize /
                static_cast<std::size_t>(
                    source.height
                ) !=
                static_cast<std::size_t>(
                    stride
                ))
        {
            return false;
        }

        std::vector<std::uint8_t>
            pixels(bufferSize);

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
        {
            REX::WARN(
                "Could not create libswscale conversion context for decoded frame."
            );

            return false;
        }

        if (
            !isRgbPixelFormat(
                sourceFormat
            ))
        {
            const int* coefficients =
                sws_getCoefficients(
                    swscaleColorSpace(
                        source.colorspace
                    )
                );

            const int colorResult =
                sws_setColorspaceDetails(
                    sws,
                    coefficients,
                    source.color_range ==
                        AVCOL_RANGE_JPEG
                        ? 1
                        : 0,
                    sws_getCoefficients(
                        SWS_CS_ITU709
                    ),
                    1,
                    0,
                    1 << 16,
                    1 << 16
                );

            if (colorResult < 0)
            {
                REX::WARN(
                    "libswscale rejected decoded-frame colorspace details (error={}); continuing with its default conversion state.",
                    colorResult
                );
            }
        }

        reportUnsupportedColorManagement(
            source
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

        sws_freeContext(
            sws
        );

        if (
            convertedHeight !=
            source.height)
        {
            REX::WARN(
                "libswscale converted {} of {} decoded frame lines.",
                convertedHeight,
                source.height
            );

            return false;
        }

        return createCanonicalD3D11Frame(
            source.width,
            source.height,
            pixels.data(),
            static_cast<std::uint32_t>(
                stride
            ),
            output
        );
    }
}

bool convertFrameForPresentation(
    const AVFrame& frame,
    producedFrame& output)
{
    preparedFrame prepared;

    if (
        !prepareFrameForConversion(
            frame,
            prepared
        ))
    {
        return false;
    }

    return convertWithSwscale(
        *prepared.frame,
        activeConversionPolicy(),
        output
    );
}
}
