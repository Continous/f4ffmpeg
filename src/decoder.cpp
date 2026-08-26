#include "decoder.h"
#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include "pch.h"
#include "graphics.h"
#include <cmath>
#include <filesystem>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/log.h>
}

#ifdef ERROR
#undef ERROR
#endif

namespace f4ffmpeg
{


namespace
{
    struct convertedRgbaFrame
    {
        std::vector<std::uint8_t> pixels;
        int width = 0;
        int height = 0;
        int stride = 0;
    };

    bool convertFrameToRgba(
        const AVFrame& frame,
        convertedRgbaFrame& output)
    {
        if (
            frame.width <= 0 ||
            frame.height <= 0)
        {
            return false;
        }

        const AVFrame* source =
            &frame;

        AVFrame* transferred =
            nullptr;

        const auto frameFormat =
            static_cast<AVPixelFormat>(
                frame.format
            );

        const auto* descriptor =
            av_pix_fmt_desc_get(
                frameFormat
            );

        const bool hardwareFrame =
            descriptor != nullptr &&
            (
                descriptor->flags &
                AV_PIX_FMT_FLAG_HWACCEL
            ) != 0;

        if (hardwareFrame)
        {
            transferred =
                av_frame_alloc();

            if (transferred == nullptr)
            {
                return false;
            }

            const int transferResult =
                av_hwframe_transfer_data(
                    transferred,
                    &frame,
                    0
                );

            if (transferResult < 0)
            {
                REX::ERROR(
                    "Failed to transfer hardware frame to system memory: {}.",
                    transferResult
                );

                av_frame_free(
                    &transferred
                );

                return false;
            }

            source =
                transferred;
        }

        if (
            source->width <= 0 ||
            source->height <= 0)
        {
            av_frame_free(
                &transferred
            );

            return false;
        }

        const int width =
            source->width;

        const int height =
            source->height;

        const auto sourceFormat =
            static_cast<AVPixelFormat>(
                source->format
            );

        SwsContext* sws =
            sws_getContext(
                width,
                height,
                sourceFormat,
                width,
                height,
                AV_PIX_FMT_RGBA,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr
            );

        if (sws == nullptr)
        {
            av_frame_free(
                &transferred
            );

            REX::ERROR(
                "Failed to create swscale context for produced frame."
            );

            return false;
        }

        output.width =
            width;

        output.height =
            height;

        output.stride =
            width * 4;

        output.pixels.resize(
            static_cast<std::size_t>(
                output.stride
            ) *
            static_cast<std::size_t>(
                height
            )
        );

        std::uint8_t* destinationData[4]{
            output.pixels.data(),
            nullptr,
            nullptr,
            nullptr
        };

        int destinationStride[4]{
            output.stride,
            0,
            0,
            0
        };

        const int convertedHeight =
            sws_scale(
                sws,
                source->data,
                source->linesize,
                0,
                height,
                destinationData,
                destinationStride
            );

        sws_freeContext(
            sws
        );

        av_frame_free(
            &transferred
        );

        if (convertedHeight != height)
        {
            REX::ERROR(
                "Produced frame conversion failed: {}/{} rows converted.",
                convertedHeight,
                height
            );

            return false;
        }

        return true;
    }

    bool createD3D11FrameFromRgba(
        const convertedRgbaFrame& source,
        producedFrame& output)
    {
        if (
            source.width <= 0 ||
            source.height <= 0 ||
            source.stride <= 0 ||
            source.pixels.empty())
        {
            return false;
        }

        auto* device =
            getD3D11Device();

        if (device == nullptr)
        {
            return false;
        }

        REX::W32::D3D11_TEXTURE2D_DESC textureDesc{};

        textureDesc.width =
            static_cast<std::uint32_t>(
                source.width
            );

        textureDesc.height =
            static_cast<std::uint32_t>(
                source.height
            );

        textureDesc.mipLevels = 1;
        textureDesc.arraySize = 1;

        textureDesc.format =
            REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM;

        textureDesc.sampleDesc.count = 1;
        textureDesc.sampleDesc.quality = 0;

        textureDesc.usage =
            REX::W32::D3D11_USAGE_DEFAULT;

        textureDesc.bindFlags =
            REX::W32::D3D11_BIND_SHADER_RESOURCE;

        textureDesc.cpuAccessFlags = 0;
        textureDesc.miscFlags = 0;

        REX::W32::D3D11_SUBRESOURCE_DATA initialData{};
        initialData.sysMem =
            source.pixels.data();
        initialData.sysMemPitch =
            static_cast<std::uint32_t>(
                source.stride
            );
        initialData.sysMemSlicePitch =
            static_cast<std::uint32_t>(
                source.stride *
                source.height
            );

        REX::W32::ID3D11Texture2D* texture =
            nullptr;

        REX::TRACE(
            "Creating populated produced D3D11 texture: {}x{}",
            source.width,
            source.height
        );

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
                "CreateTexture2D for produced frame failed: 0x{:08X}.",
                static_cast<std::uint32_t>(
                    textureResult
                )
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
                "CreateShaderResourceView for produced frame failed: 0x{:08X}.",
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
                source.width
            );

        output.height =
            static_cast<std::uint32_t>(
                source.height
            );

        return true;
    }

    bool produceHardwareFrameD3D11Ready(
        const AVFrame* frame,
        AVPixelFormat expectedFormat,
        producedFrame& output)
    {
        if (
            frame == nullptr ||
            frame->format != expectedFormat)
        {
            return false;
        }

        convertedRgbaFrame rgba;

        if (!convertFrameToRgba(
                *frame,
                rgba))
        {
            return false;
        }

        if (!createD3D11FrameFromRgba(
                rgba,
                output))
        {
            return false;
        }

        REX::TRACE(
            "Produced D3D11-ready RGBA frame: {}x{}, texture={}, srv={}.",
            output.width,
            output.height,
            static_cast<void*>(
                output.texture
            ),
            static_cast<void*>(
                output.resourceView
            )
        );

        return true;
    }
}



std::string makeFrameDumpPath(
    const std::string& basePath,
    const char* suffix)
{
    const std::filesystem::path path{
        basePath
    };

    return (
        path.parent_path() /
        (
            path.stem().string() +
            "_" +
            suffix +
            path.extension().string()
        )
    ).string();
}

namespace {

    bool writeTga(
        const char* path,
        const std::uint8_t* data,
        int width,
        int height,
        int stride
    );

bool dumpProducedFrame(
    const producedFrame& frame,
    const char* outputPath)
{
    if (
        frame.texture == nullptr ||
        outputPath == nullptr ||
        frame.width == 0 ||
        frame.height == 0)
    {
        return false;
    }

    auto* device =
        getD3D11Device();

    auto* context =
        getD3D11DeviceContext();

    if (
        device == nullptr ||
        context == nullptr)
    {
        return false;
    }

    REX::INFO(
        "Writing produced frame dump to {}",
        outputPath
    );

    // The produced texture is GPU-only, so make a CPU-readable staging texture.

    REX::W32::D3D11_TEXTURE2D_DESC desc{};
    frame.texture->GetDesc(
        &desc
    );

    REX::W32::D3D11_TEXTURE2D_DESC stagingDesc =
        desc;

    stagingDesc.usage =
        REX::W32::D3D11_USAGE_STAGING;

    stagingDesc.bindFlags = 0;

    stagingDesc.cpuAccessFlags =
        REX::W32::D3D11_CPU_ACCESS_READ;

    // Staging resources cannot retain the shared/keyed-mutex flags.
    stagingDesc.miscFlags = 0;

    REX::W32::ID3D11Texture2D*
        stagingTexture = nullptr;

    const auto createResult =
        device->CreateTexture2D(
            &stagingDesc,
            nullptr,
            &stagingTexture
        );

    if (
        createResult < 0 ||
        stagingTexture == nullptr)
    {
        REX::WARN(
            "Failed to create produced-frame "
            "staging texture: 0x{:08X}",
            static_cast<std::uint32_t>(
                createResult
            )
        );

        return false;
    }

    // GPU texture -> CPU-readable staging texture.

    context->CopyResource(
        stagingTexture,
        frame.texture
    );

    REX::W32::D3D11_MAPPED_SUBRESOURCE
        mapped{};

    const auto mapResult =
        context->Map(
            stagingTexture,
            0,
            REX::W32::D3D11_MAP_READ,
            0,
            &mapped
        );

    if (mapResult < 0)
    {
        REX::WARN(
            "Failed to map produced-frame "
            "staging texture: 0x{:08X}",
            static_cast<std::uint32_t>(
                mapResult
            )
        );

        stagingTexture->Release();

        return false;
    }

    // Our canonical texture is RGBA8. TGA expects B,G,R,A byte ordering.

    const int stride =
        static_cast<int>(
            frame.width * 4
        );

    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(stride) *
        static_cast<std::size_t>(
            frame.height
        )
    );

    for (
        std::uint32_t y = 0;
        y < frame.height;
        ++y)
    {
        const auto* source =
            static_cast<const std::uint8_t*>(
                mapped.data
            ) +
            static_cast<std::size_t>(y) *
                mapped.rowPitch;

        auto* destination =
            pixels.data() +
            static_cast<std::size_t>(y) *
                stride;

        for (
            std::uint32_t x = 0;
            x < frame.width;
            ++x)
        {
            destination[0] =
                source[2]; // B

            destination[1] =
                source[1]; // G

            destination[2] =
                source[0]; // R

            destination[3] =
                source[3]; // A

            source += 4;
            destination += 4;
        }
    }

    context->Unmap(
        stagingTexture,
        0
    );

    stagingTexture->Release();

    return writeTga(
        outputPath,
        pixels.data(),
        static_cast<int>(
            frame.width
        ),
        static_cast<int>(
            frame.height
        ),
        stride
    );
}

bool dumpDecodedFrame(
        const AVFrame& frame,
        const char* outputPath)
    {
        if (
            outputPath == nullptr ||
            frame.width <= 0 ||
            frame.height <= 0)
        {
            return false;
        }

        const AVFrame* source = &frame;

        AVFrame* transferred = nullptr;

        const auto frameFormat =
            static_cast<AVPixelFormat>(
                frame.format
            );

        const auto* descriptor =
            av_pix_fmt_desc_get(
                frameFormat
            );

        const bool hardwareFrame =
            descriptor != nullptr &&
            (
                descriptor->flags &
                AV_PIX_FMT_FLAG_HWACCEL
            );

        if (hardwareFrame)
        {
            transferred =
                av_frame_alloc();

            if (transferred == nullptr)
            {
                return false;
            }

            const int transferResult =
                av_hwframe_transfer_data(
                    transferred,
                    &frame,
                    0
                );

            if (transferResult < 0)
            {
                REX::ERROR(
                    "Failed to transfer hardware frame for dump: {}.",
                    transferResult
                );

                av_frame_free(
                    &transferred
                );

                return false;
            }

            source = transferred;
        }

        const int width =
            source->width;

        const int height =
            source->height;

        const auto sourceFormat =
            static_cast<AVPixelFormat>(
                source->format
            );

        SwsContext* sws =
            sws_getContext(
                width,
                height,
                sourceFormat,
                width,
                height,
                AV_PIX_FMT_BGRA,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr
            );

        if (sws == nullptr)
        {
            av_frame_free(
                &transferred
            );

            REX::ERROR(
                "Failed to create swscale context "
                "for decoded frame dump."
            );

            return false;
        }

        const int stride =
            width * 4;

        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(stride) *
            static_cast<std::size_t>(height)
        );

        std::uint8_t* destinationData[4]{
            pixels.data(),
            nullptr,
            nullptr,
            nullptr
        };

        int destinationStride[4]{
            stride,
            0,
            0,
            0
        };

        const int convertedHeight =
            sws_scale(
                sws,
                source->data,
                source->linesize,
                0,
                height,
                destinationData,
                destinationStride
            );

        sws_freeContext(
            sws
        );

        av_frame_free(
            &transferred
        );

        if (convertedHeight != height)
        {
            REX::ERROR(
                "Decoded frame conversion failed: "
                "{}/{} rows converted.",
                convertedHeight,
                height
            );

            return false;
        }

        REX::INFO(
            "Writing decoded frame dump to {}",
            outputPath
        );

        return writeTga(
            outputPath,
            pixels.data(),
            width,
            height,
            stride
        );
    }

    AVPixelFormat getHardwareFormat(
        AVCodecContext* codecContext,
        const AVPixelFormat* formats)
    {
        if (
            codecContext == nullptr ||
            codecContext->opaque == nullptr ||
            formats == nullptr)
        {
            REX::ERROR(
                "FFmpeg supplied invalid hardware format context."
            );

            return AV_PIX_FMT_NONE;
        }

        const auto* hardwarePixelFormat =
            static_cast<const AVPixelFormat*>(
                codecContext->opaque
            );

        REX::INFO(
            "FFmpeg requested hardware pixel format selection."
        );

        for (
            const AVPixelFormat* format = formats;
            *format != AV_PIX_FMT_NONE;
            ++format)
        {
            const char* formatName =
                av_get_pix_fmt_name(*format);

            const AVPixFmtDescriptor* descriptor =
                av_pix_fmt_desc_get(*format);

            const bool hardwareFormat =
                descriptor != nullptr &&
                (
                    descriptor->flags &
                    AV_PIX_FMT_FLAG_HWACCEL
                );

            REX::INFO(
                "Offered pixel format: {} ({}) hardware={}",
                formatName
                    ? formatName
                    : "unknown",
                static_cast<int>(*format),
                hardwareFormat
            );

            if (*format == *hardwarePixelFormat)
            {
                REX::INFO(
                    "Selecting hardware pixel format: {}",
                    formatName
                        ? formatName
                        : "unknown"
                );

                return *format;
            }
        }

        REX::ERROR(
            "FFmpeg did not offer the selected hardware pixel format."
        );

        return AV_PIX_FMT_NONE;
    }

    const AVCodecHWConfig* findHardwareConfig(
        const AVCodec* codec,
        AVHWDeviceType deviceType)
    {
        if (codec == nullptr)
        {
            return nullptr;
        }

        for (int index = 0;; ++index)
        {
            const AVCodecHWConfig* config =
                avcodec_get_hw_config(
                    codec,
                    index
                );

            if (config == nullptr)
            {
                return nullptr;
            }

            if (
                config->device_type == deviceType &&
                (
                    config->methods &
                    AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX
                ))
            {
                return config;
            }
        }
    }

    bool writeTga( //TGA is simplest, most direct-to-write image format, find a conversion tool, or a tool that opens it if you don't like that.
        const char* path,
        const std::uint8_t* data,
        int width,
        int height,
        int stride)
    {
        if (
            path == nullptr ||
            data == nullptr ||
            width <= 0 ||
            height <= 0 ||
            stride < width * 4)
        {
            return false;
        }

        std::ofstream file(
            path,
            std::ios::binary
        );

        if (!file)
        {
            return false;
        }

        std::uint8_t header[18]{};

        // Uncompressed true-color image.
        header[2] = 2;

        // Width.
        header[12] = static_cast<std::uint8_t>(width & 0xFF);
        header[13] = static_cast<std::uint8_t>((width >> 8) & 0xFF);

        // Height.
        header[14] = static_cast<std::uint8_t>(height & 0xFF);
        header[15] = static_cast<std::uint8_t>((height >> 8) & 0xFF);

        // 32 bits per pixel.
        header[16] = 32;

        // 8 alpha bits + top-left origin.
        header[17] = 0x28;

        file.write(
            reinterpret_cast<const char*>(header),
            sizeof(header)
        );

        for (int y = 0; y < height; ++y)
        {
            file.write(
                reinterpret_cast<const char*>(
                    data + static_cast<std::size_t>(y) * stride
                ),
                static_cast<std::streamsize>(width * 4)
            );
        }

        return file.good();
    }
}

double decoder::getDuration() const
{
    if (
        formatContext == nullptr ||
        videoStreamIndex < 0 ||
        videoStreamIndex >=
            static_cast<int>(
                formatContext->nb_streams
            ))
    {
        return -1.0;
    }

    const AVStream* stream =
        formatContext
            ->streams[videoStreamIndex];

    if (
        stream->duration !=
            AV_NOPTS_VALUE &&
        stream->duration > 0)
    {
        return
            static_cast<double>(
                stream->duration
            ) *
            av_q2d(
                stream->time_base
            );
    }

    if (
        formatContext->duration !=
            AV_NOPTS_VALUE &&
        formatContext->duration > 0)
    {
        return
            static_cast<double>(
                formatContext->duration
            ) /
            AV_TIME_BASE;
    }

    return -1.0;
}

bool decoder::seek(
    double timestamp)
{
    if (
        formatContext == nullptr ||
        codecContext == nullptr ||
        videoStreamIndex < 0 ||
        !std::isfinite(timestamp))
    {
        return false;
    }
    REX::TRACE("Decoder is seeking");

    AVStream* stream =
        formatContext
            ->streams[videoStreamIndex];

    const auto timestampUs =
        static_cast<std::int64_t>(
            std::llround(
                timestamp *
                AV_TIME_BASE
            )
        );

    const auto targetTimestamp =
        av_rescale_q(
            timestampUs,
            AV_TIME_BASE_Q,
            stream->time_base
        );

    if (packet != nullptr)
    {
        av_packet_unref(packet);
    }

    const int result =
        av_seek_frame(
            formatContext,
            videoStreamIndex,
            targetTimestamp,
            AVSEEK_FLAG_BACKWARD
        );

    if (result < 0)
    {
        REX::TRACE(
            "FFmpeg seek to {:.3f}s failed: {}",
            timestamp,
            result
        );

        return false;
    }

    avcodec_flush_buffers(
        codecContext
    );

    decoderDraining = false;
    decoderEOF = false;
    currentTimestamp = -1.0;

    REX::TRACE(
        "Decoder seeked toward {:.3f}s.",
        timestamp
    );

    return true;
}

bool decoder::initializeHardwareDevice(
    AVHWDeviceType deviceType)
{
    av_buffer_unref(
        &hardwareDeviceContext
    );

    const char* deviceName =
        av_hwdevice_get_type_name(
            deviceType
        );

    REX::DEBUG(
        "Trying FFmpeg hardware device: {}",
        deviceName
            ? deviceName
            : "unknown"
    );

    const int result =
        av_hwdevice_ctx_create(
            &hardwareDeviceContext,
            deviceType,
            nullptr,
            nullptr,
            0
        );

    if (result < 0)
    {
        hardwareDeviceContext = nullptr;

        REX::ERROR(
            "Failed to create FFmpeg hardware device {}: {}",
            deviceName
                ? deviceName
                : "unknown",
            result
        );

        return false;
    }

    REX::INFO(
        "FFmpeg hardware device initialized: {}",
        deviceName
            ? deviceName
            : "unknown"
    );

    return true;
}

namespace
{
    std::atomic<std::uint64_t>
        frameDumpGeneration{0};

    std::mutex frameDumpPathMutex;

    std::string frameDumpPath;
}

std::shared_ptr<producedFrame>
decoder::frameProduce(
    const AVFrame* frame)
{
    if (frame == nullptr)
        return nullptr;

    auto output =
        std::make_shared<producedFrame>();

    const bool vulkan =
        frame->format ==
            AV_PIX_FMT_VULKAN;

    const bool d3d11 =
        frame->format ==
            AV_PIX_FMT_D3D11;

    bool produced = false;

    if (vulkan)
    {
        produced =
            frameProduceVulkan(
                frame,
                *output
            );
    }
    else if (d3d11)
    {
        produced =
            frameProduceD3D11(
                frame,
                *output
            );
    }
    else
    {
        REX::TRACE(
            "Unsupported produced-frame hardware format: {}.",
            frame->format
        );
    }

    const auto dumpGeneration =
        frameDumpGeneration.load(
            std::memory_order_acquire
        );

    if (
        dumpGeneration !=
            handledProducedFrameDumpGeneration)
    {
        handledProducedFrameDumpGeneration =
            dumpGeneration;

        std::string basePath;

        {
            std::scoped_lock lock(
                frameDumpPathMutex
            );

            basePath =
                frameDumpPath;
        }

        if (produced)
        {
            const auto outputPath =
                makeFrameDumpPath(
                    basePath,
                    vulkan
                        ? "vulkan"
                        : "d3d11"
                );

            if (!dumpProducedFrame(
                    *output,
                    outputPath.c_str()))
            {
                REX::WARN(
                    "Produced frame asset was not dumped."
                );
            }
        }
        else
        {
            REX::DEBUG(
                "Produced frame dump skipped: "
                "no produced frame asset exists."
            );
        }
    }

    if (!produced)
        return nullptr;

    return output;
}

producedFrame::~producedFrame()
{
    if (resourceView != nullptr)
    {
        resourceView->Release();
        resourceView = nullptr;
    }

    if (texture != nullptr)
    {
        texture->Release();
        texture = nullptr;
    }
}

bool decoder::frameProduceVulkan(
    const AVFrame* frame,
    producedFrame& output)
{
    if (
        frame == nullptr ||
        frame->format != AV_PIX_FMT_VULKAN)
    {
        return false;
    }

    REX::TRACE(
        "Producing Vulkan hardware frame as D3D11-ready RGBA: {}x{}.",
        frame->width,
        frame->height
    );

    return produceHardwareFrameD3D11Ready(
        frame,
        AV_PIX_FMT_VULKAN,
        output
    );
}

bool decoder::frameProduceD3D11(
    const AVFrame* frame,
    producedFrame& output)
{
    if (
        frame == nullptr ||
        frame->format != AV_PIX_FMT_D3D11)
    {
        return false;
    }

    REX::TRACE(
        "Producing D3D11VA hardware frame as Fallout D3D11-ready RGBA: {}x{}.",
        frame->width,
        frame->height
    );

    return produceHardwareFrameD3D11Ready(
        frame,
        AV_PIX_FMT_D3D11,
        output
    );
}

void frameDump(
    const char* outputPath)
{
    if (outputPath == nullptr)
        return;

    {
        std::scoped_lock lock(
            frameDumpPathMutex
        );

        frameDumpPath =
            outputPath;
    }

    frameDumpGeneration.fetch_add(
        1,
        std::memory_order_release
    );

    REX::DEBUG(
        "Decoded frame dump requested."
    );
}

double decoder::getFrameTimestamp(
    const AVFrame* frame) const
{
    if (
        frame == nullptr ||
        formatContext == nullptr ||
        videoStreamIndex < 0 ||
        frame->best_effort_timestamp == AV_NOPTS_VALUE)
    {
        return -1.0;
    }

    const AVStream* stream =
        formatContext->streams[videoStreamIndex];

    return
        frame->best_effort_timestamp *
        av_q2d(stream->time_base);
}


double decoder::getCurrentTimestamp() const
{
    return currentTimestamp;
}

decodeResult decoder::decodeNextFrame(
    AVFrame* outputFrame)
{
    if (
        formatContext == nullptr ||
        codecContext == nullptr ||
        outputFrame == nullptr)
    {
        return {
            decodeStatus::ffmpegError,
            AVERROR(EINVAL)
        };
    }

    if (decoderEOF)
    {
        return {
            decodeStatus::endOfFile,
            0
        };
    }

    av_frame_unref(outputFrame);

    while (true)
    {
        // First ask FFmpeg whether it already has a decoded frame ready.
        REX::TRACE("Calling to receive a frame...");
        const int receiveResult =
            avcodec_receive_frame(
                codecContext,
                outputFrame
            );

            REX::TRACE("We've received receiveResult: {}", receiveResult);

        if (receiveResult >= 0)
        {
            currentTimestamp =
                getFrameTimestamp(outputFrame); //Get timestamp of output frame rather than decoder frame.

        const auto dumpGeneration =
            frameDumpGeneration.load(
                std::memory_order_acquire
            );

        if (
            dumpGeneration !=
                handledDecodedFrameDumpGeneration)
        {
            handledDecodedFrameDumpGeneration =
                dumpGeneration;

            std::string basePath;

            {
                std::scoped_lock lock(
                    frameDumpPathMutex
                );

                basePath =
                    frameDumpPath;
            }

            const auto outputPath =
                makeFrameDumpPath(
                    basePath,
                    "decoded"
                );

            if (!dumpDecodedFrame(
                    *outputFrame,
                    outputPath.c_str()))
            {
                REX::WARN(
                    "Decoded frame asset was not dumped."
                );
            }
        }

            return {
                decodeStatus::frameReady,
                0
            };
        }

        if (receiveResult == AVERROR_EOF)
        {
            decoderEOF = true;

            return {
                decodeStatus::endOfFile,
                0
            };
        }

        if (receiveResult != AVERROR(EAGAIN))
        {
            return {
                decodeStatus::ffmpegError,
                receiveResult
            };
        }

        // EAGAIN means FFmpeg needs more compressed data.
        if (decoderDraining)
        {

            decoderEOF = true;

            return{
             decodeStatus::endOfFile,
             0
            };
        }

        bool packetSent = false;

        while (!packetSent)
        {
            REX::TRACE("Calling to read frame...");
            const int readResult =
                av_read_frame(
                    formatContext,
                    packet
                );
                REX::TRACE("We've received readResult: {}", readResult);

            if (readResult < 0)
            {
                // Demuxer reached the end. Flush the decoder.
                const int flushResult =
                    avcodec_send_packet(
                        codecContext,
                        nullptr
                    );

                decoderDraining = true;

                if (
                    flushResult < 0 &&
                    flushResult != AVERROR_EOF)
                {
                    return {
                        decodeStatus::ffmpegError,
                        flushResult
                    };
                }

                break;
            }

            if (packet->stream_index != videoStreamIndex)
            {
                av_packet_unref(packet);
                continue;
            }


                REX::TRACE("Decode worker is now running.");

            const int sendResult =
                avcodec_send_packet(
                    codecContext,
                    packet
                );

                REX::TRACE("We've received sendResult: {}", sendResult);

            av_packet_unref(packet);

            if (sendResult == AVERROR(EAGAIN))
            {
                // Decoder wants us to receive something first.
                break;
            }

            if (sendResult < 0)
            {
                return {
                    decodeStatus::ffmpegError,
                    sendResult
                };
            }

            packetSent = true;
        }
    }
}


bool decoder::initializeVideoDecoder()
{
    if (formatContext == nullptr)
    {
        return false;
    }

    if (codecContext != nullptr)
    {
        avcodec_free_context(&codecContext);
        av_buffer_unref(&hardwareDeviceContext);
    }

    av_packet_free(&packet);

    videoStreamIndex = av_find_best_stream(
        formatContext,
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        &videoCodec,
        0
    );

    if (videoStreamIndex < 0)
    {
        return false;
    }

    codecContext = avcodec_alloc_context3(videoCodec);

    if (codecContext == nullptr)
    {
        return false;
    }

    AVStream* videoStream =
        formatContext->streams[videoStreamIndex];

    if (avcodec_parameters_to_context(
            codecContext,
            videoStream->codecpar) < 0)
    {
        avcodec_free_context(
            &codecContext
        );

        return false;
    }

    const AVCodecHWConfig* hardwareConfig =
        findHardwareConfig(
            videoCodec,
            AV_HWDEVICE_TYPE_VULKAN
        );

    if (hardwareConfig != nullptr)
    {
        hardwareDeviceType =
            AV_HWDEVICE_TYPE_VULKAN;

        hardwarePixelFormat =
            hardwareConfig->pix_fmt;

        REX::TRACE(
            "Codec {} supports Vulkan hardware decoding.",
            videoCodec->name
        );
    }
    else
    {
        hardwareConfig =
            findHardwareConfig(
                videoCodec,
                AV_HWDEVICE_TYPE_D3D11VA
            );

        if (hardwareConfig == nullptr)
        {
            REX::ERROR(
                "Codec {} supports neither Vulkan nor D3D11VA hardware decoding.",
                videoCodec->name
            );

            avcodec_free_context(
                &codecContext
            );

            return false;
        }

        hardwareDeviceType =
            AV_HWDEVICE_TYPE_D3D11VA;

        hardwarePixelFormat =
            hardwareConfig->pix_fmt;

        REX::DEBUG(
            "Codec {} supports D3D11VA hardware decoding.",
            videoCodec->name
        );
    }


    if (!initializeHardwareDevice(
            hardwareDeviceType))
    {
        if (
            hardwareDeviceType !=
            AV_HWDEVICE_TYPE_VULKAN)
        {
            avcodec_free_context(
                &codecContext
            );

            return false;
        }

        REX::ERROR(
            "Vulkan device unavailable; trying D3D11VA."
        );

        hardwareConfig =
            findHardwareConfig(
                videoCodec,
                AV_HWDEVICE_TYPE_D3D11VA
            );

        if (hardwareConfig == nullptr)
        {
            avcodec_free_context(
                &codecContext
            );

            return false;
        }

        hardwareDeviceType =
            AV_HWDEVICE_TYPE_D3D11VA;

        hardwarePixelFormat =
            hardwareConfig->pix_fmt;

        if (!initializeHardwareDevice(
                hardwareDeviceType))
        {
            avcodec_free_context(
                &codecContext
            );

            return false;
        }

    }


    codecContext->opaque =
        &hardwarePixelFormat;

    codecContext->get_format =
        getHardwareFormat;


    codecContext->hw_device_ctx =
        av_buffer_ref(
            hardwareDeviceContext
        );

    if (codecContext->hw_device_ctx == nullptr)
    {
        avcodec_free_context(
            &codecContext
        );

        av_buffer_unref(
            &hardwareDeviceContext
        );

        return false;
    }
            const int openResult =
                avcodec_open2(
                    codecContext,
                    videoCodec,
                    nullptr
                );

            if (openResult < 0)
            {
                REX::ERROR(
                    "Failed to open hardware video decoder: {}",
                    openResult
                );

                avcodec_free_context(
                    &codecContext
                );

                return false;
            }

            packet =
                av_packet_alloc();

            if (packet == nullptr)
            {
                avcodec_free_context(
                    &codecContext
                );

                return false;
            }

            decoderDraining = false;
            decoderEOF = false;

            return true;
        }

        decoder::~decoder()
        {
            close();
        }
        bool decoder::open(const char* path)
    {
        if (formatContext != nullptr)
        {
            close();
        } //If there is a formatContext we need to close it first.


        if (avformat_open_input(
                &formatContext,
                path,
                nullptr,
                nullptr) < 0)
        {
            formatContext = nullptr; //Release formatContext, we failed to open. Maybe have a failure flag?

            return false;
        }

        if (avformat_find_stream_info(
                formatContext,
                nullptr) < 0)
        {
            close();

            return false;
        }

        const auto dumpGeneration =
            frameDumpGeneration.load(
                std::memory_order_acquire
            );

        handledDecodedFrameDumpGeneration =
            dumpGeneration;

        handledProducedFrameDumpGeneration =
            dumpGeneration;

        return true;
    }

    void decoder::close()
    {
        av_packet_free(&packet);

        if (codecContext != nullptr)
        {
            avcodec_free_context(&codecContext);
        }


        av_buffer_unref(&hardwareDeviceContext);

        if (formatContext != nullptr)
        {
            avformat_close_input(&formatContext);
        }

        videoCodec = nullptr;
        videoStreamIndex = -1;

        decoderDraining = false;
        decoderEOF = false;
    }
            //Get ffmpeg logs.
    namespace
    {
        void ffmpegLogCallback(
            void*,
            int level,
            const char* format,
            va_list args)
        {
            if (level > AV_LOG_VERBOSE)
            {
                return;
            }

            char buffer[1024]{};

            vsnprintf(
                buffer,
                sizeof(buffer),
                format,
                args
            );

            REX::TRACE(
                "[FFmpeg] {}",
                buffer
            );
        }
    }


    void initializeFFmpegLogging()
    {
        av_log_set_level(
            AV_LOG_VERBOSE
        );

        av_log_set_callback(
            ffmpegLogCallback
        );
    }
}
