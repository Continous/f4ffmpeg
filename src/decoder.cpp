#include "decoder.h"
#include "frameConverter.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include "pch.h"
#include "graphics.h"
#include <cmath>
#include <filesystem>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

#ifdef ERROR
#undef ERROR
#endif

namespace f4ffmpeg
{




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

        // Do not silently drop into software here. Returning NONE makes the
        // active backend fail cleanly so decoder::fallbackBackend() can try
        // every remaining hardware implementation before software.
        REX::WARN(
            "FFmpeg did not offer the selected hardware pixel format; "
            "requesting decoder-backend fallback."
        );

        return AV_PIX_FMT_NONE;
    }

    const char* backendLabel(
        decoderBackendKind backend)
    {
        switch (backend)
        {
            case decoderBackendKind::nvdec:
                return "NVDEC/CUDA";
            case decoderBackendKind::qsv:
                return "Intel QSV";
            case decoderBackendKind::amf:
                return "AMD AMF";
            case decoderBackendKind::vulkan:
                return "Vulkan Video";
            case decoderBackendKind::d3d11va:
                return "D3D11VA";
            case decoderBackendKind::d3d12va:
                return "D3D12VA";
            case decoderBackendKind::dxva2:
                return "DXVA2";
            case decoderBackendKind::software:
                return "software";
        }

        return "unknown";
    }

    const char* backendDeviceName(
        decoderBackendKind backend)
    {
        switch (backend)
        {
            case decoderBackendKind::nvdec:
                // FFmpeg exposes NVDEC through AV_HWDEVICE_TYPE_CUDA.
                return "cuda";
            case decoderBackendKind::qsv:
                return "qsv";
            case decoderBackendKind::amf:
                return "amf";
            case decoderBackendKind::vulkan:
                return "vulkan";
            case decoderBackendKind::d3d11va:
                return "d3d11va";
            case decoderBackendKind::d3d12va:
                return "d3d12va";
            case decoderBackendKind::dxva2:
                return "dxva2";
            case decoderBackendKind::software:
                return nullptr;
        }

        return nullptr;
    }

    const AVCodec* resolveBackendCodec(
        decoderBackendKind backend,
        AVCodecID codecId,
        const AVCodec* defaultCodec)
    {
        if (defaultCodec == nullptr)
        {
            return nullptr;
        }

        if (backend == decoderBackendKind::software)
        {
            return avcodec_find_decoder(
                codecId
            );
        }

        if (backend == decoderBackendKind::qsv)
        {
            // QSV decoder names do not always match avcodec_get_name() +
            // "_qsv" (notably MPEG-2 is mpeg2_qsv, not mpeg2video_qsv).
            const char* qsvDecoder = nullptr;

            switch (codecId)
            {
                case AV_CODEC_ID_H264:
                    qsvDecoder = "h264_qsv";
                    break;
                case AV_CODEC_ID_HEVC:
                    qsvDecoder = "hevc_qsv";
                    break;
                case AV_CODEC_ID_AV1:
                    qsvDecoder = "av1_qsv";
                    break;
                case AV_CODEC_ID_VP9:
                    qsvDecoder = "vp9_qsv";
                    break;
                case AV_CODEC_ID_VP8:
                    qsvDecoder = "vp8_qsv";
                    break;
                case AV_CODEC_ID_MPEG2VIDEO:
                    qsvDecoder = "mpeg2_qsv";
                    break;
                case AV_CODEC_ID_MJPEG:
                    qsvDecoder = "mjpeg_qsv";
                    break;
                default:
                    break;
            }

            return qsvDecoder != nullptr
                ? avcodec_find_decoder_by_name(
                    qsvDecoder
                )
                : nullptr;
        }

        if (backend == decoderBackendKind::amf)
        {
            // FFmpeg versions with AMF decoding expose codec-specific
            // *_amf decoders. Older builds simply return nullptr here.
            const char* baseName =
                avcodec_get_name(codecId);

            if (baseName == nullptr)
            {
                return nullptr;
            }

            const std::string decoderName =
                std::string(baseName) + "_amf";

            return avcodec_find_decoder_by_name(
                decoderName.c_str()
            );
        }

        // NVDEC/CUDA and the generic hardware APIs are exposed as hardware
        // configurations of FFmpeg's normal codec decoder.
        return defaultCodec;
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

    const auto frameFormat =
        static_cast<AVPixelFormat>(
            frame->format
        );

    const char* formatName =
        av_get_pix_fmt_name(
            frameFormat
        );

    const AVPixFmtDescriptor* descriptor =
        av_pix_fmt_desc_get(
            frameFormat
        );

    const bool hardwareFrame =
        descriptor != nullptr &&
        (
            descriptor->flags &
            AV_PIX_FMT_FLAG_HWACCEL
        ) != 0;

    // Every decoder backend converges on one presentation conversion boundary.
    // The converter normalizes hardware/software AVFrames, performs color/scale
    // conversion, and always emits our canonical Fallout-device RGBA8 D3D11
    // texture + SRV. decoder::frameProduce() therefore never needs to know which
    // conversion implementation (libplacebo or fallback) handled the frame.
    const bool produced =
        convertFrameForPresentation(
            *frame,
            *output
        );

    if (produced)
    {
        REX::TRACE(
            "Produced {} frame '{}' as Fallout D3D11-ready RGBA: {}x{}.",
            hardwareFrame
                ? "hardware-decoded"
                : "software-decoded",
            formatName
                ? formatName
                : "unknown",
            output->width,
            output->height
        );
    }
    else
    {
        REX::WARN(
            "Failed to produce decoded frame format '{}' ({}).",
            formatName
                ? formatName
                : "unknown",
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
                    formatName
                        ? formatName
                        : (
                            hardwareFrame
                                ? "hardware"
                                : "software"
                        )
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

bool decoder::frameDump(
    const producedFrame& frame,
    const char* outputPath)
{
    return dumpProducedFrame(
        frame,
        outputPath
    );
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
            if (
                activeBackend == decoderBackendKind::software &&
                receiveResult == AVERROR_INVALIDDATA)
            {
                // Software is the terminal backend. For damaged bitstreams,
                // discard the decoder's corrupt state and keep reading rather
                // than terminating playback.
                REX::WARN(
                    "Software decoder rejected corrupt frame data; flushing and continuing."
                );

                avcodec_flush_buffers(
                    codecContext
                );

                decoderDraining = false;

                av_frame_unref(
                    outputFrame
                );

                continue;
            }

            if (fallbackBackend(
                    receiveResult,
                    "avcodec_receive_frame"))
            {
                av_frame_unref(
                    outputFrame
                );

                return decodeNextFrame(
                    outputFrame
                );
            }

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

            if (readResult == AVERROR(EAGAIN))
            {
                continue;
            }

            if (readResult == AVERROR_EOF)
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
                    if (fallbackBackend(
                            flushResult,
                            "decoder drain"))
                    {
                        return decodeNextFrame(
                            outputFrame
                        );
                    }

                    return {
                        decodeStatus::ffmpegError,
                        flushResult
                    };
                }

                break;
            }

            if (readResult == AVERROR_INVALIDDATA)
            {
                // Demuxers commonly report isolated corrupt packets this way.
                // Usually they advance past the damaged region, but cap the
                // retry streak so a broken demuxer/input cannot spin forever.
                ++consecutiveDemuxErrors;

                if (consecutiveDemuxErrors <= 16)
                {
                    REX::WARN(
                        "FFmpeg demuxer skipped invalid packet data (retry {}/16).",
                        consecutiveDemuxErrors
                    );

                    continue;
                }

                REX::ERROR(
                    "FFmpeg demuxer exceeded recoverable invalid-data retry budget."
                );

                return {
                    decodeStatus::ffmpegError,
                    readResult
                };
            }

            if (readResult < 0)
            {
                // Genuine container/I/O failures are not decoder-backend
                // failures. At this point there is no alternate decoder API
                // that can repair an unreadable input source.
                return {
                    decodeStatus::ffmpegError,
                    readResult
                };
            }

            consecutiveDemuxErrors = 0;

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
                if (
                    activeBackend == decoderBackendKind::software &&
                    sendResult == AVERROR_INVALIDDATA)
                {
                    REX::WARN(
                        "Software decoder skipped an invalid compressed packet."
                    );

                    // packet was already unreferenced above; read the next one.
                    continue;
                }

                if (fallbackBackend(
                        sendResult,
                        "avcodec_send_packet"))
                {
                    return decodeNextFrame(
                        outputFrame
                    );
                }

                return {
                    decodeStatus::ffmpegError,
                    sendResult
                };
            }

            packetSent = true;
        }
    }
}


void decoder::buildBackendOrder()
{
    backendCount = 0;
    activeBackendIndex =
        maxDecoderBackends;

    const auto pushUnique =
        [this](decoderBackendKind backend)
        {
            for (
                std::size_t index = 0;
                index < backendCount;
                ++index)
            {
                if (backendOrder[index] == backend)
                {
                    return;
                }
            }

            if (backendCount < maxDecoderBackends)
            {
                backendOrder[backendCount++] =
                    backend;
            }
        };

    // Prefer each vendor's native decoder when FFmpeg was built with it.
    // Unsupported/compiled-out backends are skipped cheaply at runtime.
    pushUnique(decoderBackendKind::nvdec);
    pushUnique(decoderBackendKind::qsv);
    pushUnique(decoderBackendKind::amf);

    // Preserve a working generic device across playlist/shuffle transitions
    // before probing the other cross-vendor APIs.
    if (initializedHardwareDeviceType != AV_HWDEVICE_TYPE_NONE)
    {
        const char* currentName =
            av_hwdevice_get_type_name(
                initializedHardwareDeviceType
            );

        if (currentName != nullptr)
        {
            if (std::string_view(currentName) == "vulkan")
                pushUnique(decoderBackendKind::vulkan);
            else if (std::string_view(currentName) == "d3d11va")
                pushUnique(decoderBackendKind::d3d11va);
            else if (std::string_view(currentName) == "d3d12va")
                pushUnique(decoderBackendKind::d3d12va);
            else if (std::string_view(currentName) == "dxva2")
                pushUnique(decoderBackendKind::dxva2);
        }
    }

    pushUnique(decoderBackendKind::vulkan);
    pushUnique(decoderBackendKind::d3d11va);
    pushUnique(decoderBackendKind::d3d12va);
    pushUnique(decoderBackendKind::dxva2);

    // Software decode is deliberately last and is always present.
    pushUnique(decoderBackendKind::software);
}

bool decoder::openBackend(
    std::size_t backendIndex)
{
    if (
        formatContext == nullptr ||
        videoStreamIndex < 0 ||
        videoStreamIndex >=
            static_cast<int>(
                formatContext->nb_streams
            ) ||
        videoCodec == nullptr ||
        backendIndex >= backendCount)
    {
        return false;
    }

    const auto backend =
        backendOrder[backendIndex];

    AVStream* videoStream =
        formatContext->streams[
            videoStreamIndex
        ];

    if (videoStream == nullptr)
    {
        return false;
    }

    const AVCodecID codecId =
        videoStream->codecpar->codec_id;

    const AVCodec* candidateCodec =
        resolveBackendCodec(
            backend,
            codecId,
            videoCodec
        );

    if (candidateCodec == nullptr)
    {
        REX::TRACE(
            "Decoder backend {} has no codec implementation for {}.",
            backendLabel(backend),
            avcodec_get_name(codecId)
        );

        return false;
    }

    AVHWDeviceType candidateDeviceType =
        AV_HWDEVICE_TYPE_NONE;

    const AVCodecHWConfig* hardwareConfig =
        nullptr;

    // A newly-created device is held locally until the codec has opened.
    // This makes probing/fallback transactional: a candidate backend cannot
    // destroy the currently-working device merely because device creation
    // succeeded and avcodec_open2() failed afterward.
    AVBufferRef* candidateDevice = nullptr;
    AVBufferRef* deviceForCodec = nullptr;
    bool reusingDevice = false;

    if (backend != decoderBackendKind::software)
    {
        const char* deviceName =
            backendDeviceName(backend);

        candidateDeviceType =
            deviceName != nullptr
                ? av_hwdevice_find_type_by_name(
                    deviceName
                )
                : AV_HWDEVICE_TYPE_NONE;

        if (candidateDeviceType == AV_HWDEVICE_TYPE_NONE)
        {
            REX::TRACE(
                "Decoder backend {} is not compiled into this FFmpeg build.",
                backendLabel(backend)
            );

            return false;
        }

        hardwareConfig =
            findHardwareConfig(
                candidateCodec,
                candidateDeviceType
            );

        if (hardwareConfig == nullptr)
        {
            REX::TRACE(
                "Decoder backend {} is not advertised for codec {}.",
                backendLabel(backend),
                candidateCodec->name
            );

            return false;
        }

        if (
            hardwareDeviceContext != nullptr &&
            initializedHardwareDeviceType ==
                candidateDeviceType)
        {
            deviceForCodec =
                hardwareDeviceContext;

            reusingDevice = true;
        }
        else
        {
            const char* deviceArgument =
                deviceName != nullptr &&
                std::string_view(deviceName) == "qsv"
                    ? "auto"
                    : nullptr;

            REX::DEBUG(
                "Trying FFmpeg hardware device for {}: {}",
                backendLabel(backend),
                deviceName
                    ? deviceName
                    : "unknown"
            );

            const int deviceResult =
                av_hwdevice_ctx_create(
                    &candidateDevice,
                    candidateDeviceType,
                    deviceArgument,
                    nullptr,
                    0
                );

            if (deviceResult < 0)
            {
                av_buffer_unref(
                    &candidateDevice
                );

                REX::TRACE(
                    "Decoder backend {} device creation failed: {}.",
                    backendLabel(backend),
                    deviceResult
                );

                return false;
            }

            deviceForCodec =
                candidateDevice;
        }
    }

    AVCodecContext* candidateContext =
        avcodec_alloc_context3(
            candidateCodec
        );

    if (candidateContext == nullptr)
    {
        av_buffer_unref(
            &candidateDevice
        );

        return false;
    }

    if (avcodec_parameters_to_context(
            candidateContext,
            videoStream->codecpar) < 0)
    {
        avcodec_free_context(
            &candidateContext
        );

        av_buffer_unref(
            &candidateDevice
        );

        return false;
    }

    AVPixelFormat candidateHardwareFormat =
        AV_PIX_FMT_NONE;

    if (backend != decoderBackendKind::software)
    {
        candidateHardwareFormat =
            hardwareConfig->pix_fmt;

        // get_format can run inside avcodec_open2(). Point opaque at a local
        // value while probing, then repoint it at the persistent member once
        // this candidate becomes the active decoder.
        candidateContext->opaque =
            &candidateHardwareFormat;

        candidateContext->get_format =
            getHardwareFormat;

        candidateContext->hw_device_ctx =
            av_buffer_ref(
                deviceForCodec
            );

        if (candidateContext->hw_device_ctx == nullptr)
        {
            avcodec_free_context(
                &candidateContext
            );

            av_buffer_unref(
                &candidateDevice
            );

            return false;
        }
    }

    // Be permissive about damaged bitstreams. Backend failure/recovery is
    // handled by f4ffmpeg; FFmpeg should decode through recoverable corruption
    // whenever it can instead of escalating minor damage into a hard stop.
    candidateContext->err_recognition =
        AV_EF_IGNORE_ERR;

    REX::INFO(
        "Trying {} decoder '{}' for codec {}.",
        backendLabel(backend),
        candidateCodec->name,
        avcodec_get_name(codecId)
    );

    const int openResult =
        avcodec_open2(
            candidateContext,
            candidateCodec,
            nullptr
        );

    if (openResult < 0)
    {
        REX::WARN(
            "Decoder backend {} could not open codec {}: {}. Trying fallback.",
            backendLabel(backend),
            avcodec_get_name(codecId),
            openResult
        );

        avcodec_free_context(
            &candidateContext
        );

        av_buffer_unref(
            &candidateDevice
        );

        return false;
    }

    AVPacket* candidatePacket =
        av_packet_alloc();

    if (candidatePacket == nullptr)
    {
        avcodec_free_context(
            &candidateContext
        );

        av_buffer_unref(
            &candidateDevice
        );

        return false;
    }

    // Commit only after device + codec + packet have all initialized.
    if (backend != decoderBackendKind::software)
    {
        hardwarePixelFormat =
            candidateHardwareFormat;

        candidateContext->opaque =
            &hardwarePixelFormat;

        if (!reusingDevice)
        {
            av_buffer_unref(
                &hardwareDeviceContext
            );

            hardwareDeviceContext =
                candidateDevice;

            candidateDevice =
                nullptr;

            initializedHardwareDeviceType =
                candidateDeviceType;

            REX::INFO(
                "FFmpeg hardware device committed for decoder backend {}.",
                backendLabel(backend)
            );
        }
        else
        {
            REX::TRACE(
                "Reusing persistent FFmpeg hardware device for decoder backend {}.",
                backendLabel(backend)
            );
        }
    }

    av_packet_free(
        &packet
    );

    if (codecContext != nullptr)
    {
        avcodec_free_context(
            &codecContext
        );
    }

    codecContext =
        candidateContext;

    packet =
        candidatePacket;

    activeCodec =
        candidateCodec;

    activeBackend =
        backend;

    activeBackendIndex =
        backendIndex;

    hardwareDeviceType =
        candidateDeviceType;

    if (backend == decoderBackendKind::software)
    {
        hardwareDeviceType =
            AV_HWDEVICE_TYPE_NONE;

        hardwarePixelFormat =
            AV_PIX_FMT_NONE;
    }

    decoderDraining = false;
    decoderEOF = false;

    REX::INFO(
        "Decoder backend active: {} (codec '{}').",
        backendLabel(activeBackend),
        activeCodec->name
    );

    return true;
}

bool decoder::fallbackBackend(
    int ffmpegError,
    const char* stage)
{
    if (activeBackendIndex >= backendCount)
    {
        return false;
    }

    if (activeBackend == decoderBackendKind::software)
    {
        return false;
    }

    const double resumeTimestamp =
        std::isfinite(currentTimestamp) &&
        currentTimestamp >= 0.0
            ? currentTimestamp
            : 0.0;

    REX::WARN(
        "Decoder backend {} failed during {} with FFmpeg error {}. "
        "Falling through to the next backend.",
        backendLabel(activeBackend),
        stage ? stage : "decode",
        ffmpegError
    );

    for (
        std::size_t index =
            activeBackendIndex + 1;
        index < backendCount;
        ++index)
    {
        if (!openBackend(index))
        {
            continue;
        }

        // The demuxer may already have consumed the packet that exposed the
        // failed backend. Rewind to the last successfully presented timestamp
        // (or zero before the first frame) before resuming on the new backend.
        if (!seek(resumeTimestamp))
        {
            REX::WARN(
                "Decoder backend {} opened, but seek to {:.3f}s after fallback failed. "
                "Continuing from the demuxer's current position.",
                backendLabel(activeBackend),
                resumeTimestamp
            );

            decoderDraining = false;
            decoderEOF = false;
        }

        REX::INFO(
            "Decoder recovered from backend failure using {}.",
            backendLabel(activeBackend)
        );

        return true;
    }

    return false;
}

bool decoder::initializeVideoDecoder()
{
    if (formatContext == nullptr)
    {
        return false;
    }

    av_packet_free(
        &packet
    );

    if (codecContext != nullptr)
    {
        avcodec_free_context(
            &codecContext
        );
    }

    videoStreamIndex =
        av_find_best_stream(
            formatContext,
            AVMEDIA_TYPE_VIDEO,
            -1,
            -1,
            &videoCodec,
            0
        );

    if (
        videoStreamIndex < 0 ||
        videoCodec == nullptr)
    {
        return false;
    }

    buildBackendOrder();

    for (
        std::size_t index = 0;
        index < backendCount;
        ++index)
    {
        if (openBackend(index))
        {
            currentTimestamp = -1.0;
            return true;
        }
    }

    REX::ERROR(
        "No FFmpeg decoder implementation could open codec {}.",
        videoCodec->name
    );

    return false;
}

        decoder::~decoder()
        {
            close();
        }
        bool decoder::open(const char* path)
    {
        if (path == nullptr)
        {
            return false;
        }

        // Source changes release only source-specific demuxer/codec state.
        // Keep AVHWDeviceContext alive for compatible playlist/shuffle
        // transitions.
        closeSource();

        if (avformat_open_input(
                &formatContext,
                path,
                nullptr,
                nullptr) < 0)
        {
            formatContext = nullptr;
            return false;
        }

        if (avformat_find_stream_info(
                formatContext,
                nullptr) < 0)
        {
            closeSource();
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

    void decoder::closeSource()
    {
        av_packet_free(&packet);

        if (codecContext != nullptr)
        {
            avcodec_free_context(
                &codecContext
            );
        }

        if (formatContext != nullptr)
        {
            avformat_close_input(
                &formatContext
            );
        }

        videoCodec = nullptr;
        activeCodec = nullptr;
        videoStreamIndex = -1;

        backendCount = 0;
        activeBackendIndex =
            maxDecoderBackends;

        activeBackend =
            decoderBackendKind::software;

        hardwarePixelFormat =
            AV_PIX_FMT_NONE;

        decoderDraining = false;
        decoderEOF = false;
        currentTimestamp = -1.0;
        consecutiveDemuxErrors = 0;
    }

    void decoder::close()
    {
        closeSource();

        av_buffer_unref(
            &hardwareDeviceContext
        );

        hardwareDeviceType =
            AV_HWDEVICE_TYPE_NONE;

        initializedHardwareDeviceType =
            AV_HWDEVICE_TYPE_NONE;
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
