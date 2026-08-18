#include "decoder.h"
#include "graphics.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>
#include "pch.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
#include <libavutil/log.h>
}

namespace f4ffmpeg
{

namespace {



    AVPixelFormat getD3D11Format(
        AVCodecContext*,
        const AVPixelFormat* formats)
    {
        if (formats == nullptr)
        {
            REX::ERROR(
                "FFmpeg supplied no pixel formats."
            );

            return AV_PIX_FMT_NONE;
        }

        REX::INFO(
            "FFmpeg requested pixel format selection."
        );

        for (const AVPixelFormat* format = formats;
            *format != AV_PIX_FMT_NONE;
            ++format)
        {
            const char* formatName =
                av_get_pix_fmt_name(*format);

            const AVPixFmtDescriptor* descriptor =
                av_pix_fmt_desc_get(*format);

            const bool hardwareFormat =
                descriptor != nullptr &&
                (descriptor->flags &
                AV_PIX_FMT_FLAG_HWACCEL);

            REX::INFO(
                "Offered pixel format: {} ({}) hardware={}",
                formatName ? formatName : "unknown",
                static_cast<int>(*format),
                hardwareFormat
            );

            if (*format == AV_PIX_FMT_D3D11)
            {
                REX::INFO(
                    "Selecting D3D11 pixel format."
                );

                return *format;
            }
        }

        REX::ERROR(
            "FFmpeg did not offer a D3D11 pixel format."
        );

        return AV_PIX_FMT_NONE;
    }

    bool writeBmp(
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

        constexpr std::uint32_t fileHeaderSize = 14;
        constexpr std::uint32_t infoHeaderSize = 40;
        constexpr std::uint32_t pixelOffset =
            fileHeaderSize + infoHeaderSize;

        const std::uint32_t pixelDataSize =
            static_cast<std::uint32_t>(
                width * height * 4
            );

        const std::uint32_t fileSize =
            pixelOffset + pixelDataSize;

        // BITMAPFILEHEADER

        file.put('B');
        file.put('M');

        file.write(
            reinterpret_cast<const char*>(&fileSize),
            sizeof(fileSize)
        );

        const std::uint16_t reserved = 0;

        file.write(
            reinterpret_cast<const char*>(&reserved),
            sizeof(reserved)
        );

        file.write(
            reinterpret_cast<const char*>(&reserved),
            sizeof(reserved)
        );

        file.write(
            reinterpret_cast<const char*>(&pixelOffset),
            sizeof(pixelOffset)
        );


        // BITMAPINFOHEADER

        file.write(
            reinterpret_cast<const char*>(&infoHeaderSize),
            sizeof(infoHeaderSize)
        );

        const std::int32_t bmpWidth = width;

        // Negative height means rows are stored top-to-bottom.
        const std::int32_t bmpHeight = -height;

        file.write(
            reinterpret_cast<const char*>(&bmpWidth),
            sizeof(bmpWidth)
        );

        file.write(
            reinterpret_cast<const char*>(&bmpHeight),
            sizeof(bmpHeight)
        );

        const std::uint16_t planes = 1;
        const std::uint16_t bitsPerPixel = 32;

        file.write(
            reinterpret_cast<const char*>(&planes),
            sizeof(planes)
        );

        file.write(
            reinterpret_cast<const char*>(&bitsPerPixel),
            sizeof(bitsPerPixel)
        );

        const std::uint32_t compression = 0;

        file.write(
            reinterpret_cast<const char*>(&compression),
            sizeof(compression)
        );

        file.write(
            reinterpret_cast<const char*>(&pixelDataSize),
            sizeof(pixelDataSize)
        );

        const std::int32_t pixelsPerMeter = 0;

        file.write(
            reinterpret_cast<const char*>(&pixelsPerMeter),
            sizeof(pixelsPerMeter)
        );

        file.write(
            reinterpret_cast<const char*>(&pixelsPerMeter),
            sizeof(pixelsPerMeter)
        );

        const std::uint32_t colorsUsed = 0;
        const std::uint32_t importantColors = 0;

        file.write(
            reinterpret_cast<const char*>(&colorsUsed),
            sizeof(colorsUsed)
        );

        file.write(
            reinterpret_cast<const char*>(&importantColors),
            sizeof(importantColors)
        );


        // Pixel data.
        // BGRA matches 32-bit BMP byte order nicely.

        for (int y = 0; y < height; ++y)
        {
            file.write(
                reinterpret_cast<const char*>(
                    data + y * stride
                ),
                width * 4
            );
        }

        return file.good();
    }
}

bool decoder::initializeD3D11Device()
{
    av_buffer_unref(
        &hardwareDeviceContext
    );

    const int result =
        av_hwdevice_ctx_create(
            &hardwareDeviceContext,
            AV_HWDEVICE_TYPE_D3D11VA,
            nullptr,
            nullptr,
            0
        );

    if (result < 0)
    {
        hardwareDeviceContext = nullptr;

        REX::ERROR(
            "Failed to create FFmpeg D3D11VA device: {}",
            result
        );

        return false;
    }

    REX::INFO(
        "FFmpeg D3D11VA device initialized."
    );

    return true;
}

bool decoder::frameProduce(
    const AVFrame* frame,
    frameProduceMethod method,
    const char* outputPath)
{
    if (frame == nullptr)
    {
        return false;
    }

    switch (method)
    {
    case frameProduceMethod::bitmap:
        return frameProduceBitmap(
            frame,
            outputPath
        );

    case frameProduceMethod::d3d11Texture:
        return frameProduceD3D11Texture(
            frame
        );

    default:
        return false;
    }
}

bool decoder::frameProduceD3D11Texture(
    const AVFrame* frame)
{
    (void)frame; //Currently unimplemented.
    return false;
}


bool decoder::frameProduceBitmap(
    const AVFrame* frame,
    const char* outputPath)
{
    if (frame == nullptr || outputPath == nullptr)
    {
        return false;
    }

    const int width = frame->width;
    const int height = frame->height;
    const int stride = width * 4;

    std::vector<std::uint8_t> bgraBuffer(
        static_cast<std::size_t>(stride) * height
    );

    SwsContext* swsContext = sws_getContext(
        width,
        height,
        static_cast<AVPixelFormat>(frame->format),
        width,
        height,
        AV_PIX_FMT_BGRA,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr
    );

    if (swsContext == nullptr)
    {
        return false;
    }

    std::uint8_t* outputData[4]{
        bgraBuffer.data(),
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

    const int convertedHeight = sws_scale(
        swsContext,
        frame->data,
        frame->linesize,
        0,
        height,
        outputData,
        outputLinesize
    );

    sws_freeContext(swsContext);

    if (convertedHeight != height)
    {
        return false;
    }

    if (!writeBmp(
        outputPath,
        bgraBuffer.data(),
        width,
        height,
        stride))
    {
        return false;
    }


    return true;
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
        REX::INFO("Calling to receive a frame...");
        const int receiveResult =
            avcodec_receive_frame(
                codecContext,
                outputFrame
            );

            REX::INFO("We've received receiveResult: {}", receiveResult);

        if (receiveResult >= 0)
        {
            currentTimestamp =
                getFrameTimestamp(outputFrame); //Get timestamp of output frame rather than decoder frame.

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
            REX::INFO("Calling to read frame...");
            const int readResult =
                av_read_frame(
                    formatContext,
                    packet
                );
                REX::INFO("We've received readResult: {}", readResult);

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


                REX::INFO("Decode worker is now running.");

            const int sendResult =
                avcodec_send_packet(
                    codecContext,
                    packet
                );

                REX::INFO("We've received sendResult: {}", sendResult);

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
    codecContext->get_format =
        getD3D11Format;

    if (!initializeD3D11Device())
    {
        avcodec_free_context(
            &codecContext
        );

        return false;
    }

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

    if (avcodec_open2(
            codecContext,
            videoCodec,
            nullptr) < 0)
    {
        avcodec_free_context(&codecContext);
        return false;
    }
    packet = av_packet_alloc();

    if (packet == nullptr)
    {
        avcodec_free_context(&codecContext);
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

            REX::INFO(
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
