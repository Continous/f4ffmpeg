#include "decoder.h"
#include <algorithm>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
}

namespace f4ffmpeg
{

bool decoder::hasHardwareDecoder() const
{
    return hardwareDecoder;
}

const std::vector<hardwareCodec>& decoder::getHardwareCodecs() const
{
    return hardwareCodecs;
}

const std::vector<preferredCodec>& decoder::getPreferredCodecs() const
{
    return preferredCodecs;
}

bool decoder::initializeVideoDecoder()
{
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
        return false;
    }

    if (avcodec_open2(
            codecContext,
            videoCodec,
            nullptr) < 0)
    {
        return false;
    }

    return true;
}

bool decoder::decodeTestFrame()
{
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    if (packet == nullptr || frame == nullptr)
    {
        av_packet_free(&packet);
        av_frame_free(&frame);
        return false;
    }

    bool frameDecoded = false;

    while (av_read_frame(formatContext, packet) >= 0)
    {
        if (packet->stream_index == videoStreamIndex)
        {
            if (avcodec_send_packet(codecContext, packet) >= 0)
            {
                int result =
                    avcodec_receive_frame(codecContext, frame);

                if (result >= 0)
                {
                    frameDecoded = true;
                    av_packet_unref(packet);
                    break;
                }
            }
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    av_frame_free(&frame);

    return frameDecoded;
}

void decoder::testHardwareDevices()
{
    hardwareDecoder = false;
    hardwareCodecs.clear(); //Clear the test so we get new results each time and no duplicates.

    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    bool foundDeviceType = false;

    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
    {
        foundDeviceType = true;

        const char* deviceName = av_hwdevice_get_type_name(type);


        AVBufferRef* deviceContext = nullptr;

        int result = av_hwdevice_ctx_create(
            &deviceContext,
            type,
            nullptr,
            nullptr,
            0
        );

        if (result < 0)
        {
            continue;
        }

        // Capability scan
        void* iterator = nullptr;
        const AVCodec* codec = nullptr;

        while ((codec = av_codec_iterate(&iterator)) != nullptr)
        {
            if (!av_codec_is_decoder(codec))
            {
                continue;
            }

            for (int i = 0;; i++)
            {
                const AVCodecHWConfig* config =
                    avcodec_get_hw_config(codec, i);

                if (config == nullptr)
                {
                    break;
                }

                if (config->device_type == type)
                {
                    hardwareDecoder = true;

                    hardwareCodecs.push_back({
                        deviceName,
                        codec->name
                    });
                    break;
                }
            }
        }

        av_buffer_unref(&deviceContext);
    }

    if (!foundDeviceType)
    {
        hardwareDecoder = false;
    }

    buildPreferredCodecs();
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
        if (codecContext != nullptr)
        {
            avcodec_free_context(&codecContext);
        }
        if (formatContext != nullptr)
        {
            avformat_close_input(&formatContext);
        }
        videoCodec = nullptr;
        videoStreamIndex = -1;
    }
    void decoder::buildPreferredCodecs()
    {
        preferredCodecs.clear();

        for (const auto& hardwareCodec : hardwareCodecs)
        {
            auto existing = std::find_if(
                preferredCodecs.begin(),
                preferredCodecs.end(),
                [&](const preferredCodec& item)
                {
                    return item.codec == hardwareCodec.codec;
                }
            );

            if (existing == preferredCodecs.end())
            {
                preferredCodecs.push_back({
                    hardwareCodec.backend,
                    hardwareCodec.codec
                });

                continue;
            }

            if (hardwareCodec.backend == "d3d11va")
            {
                existing->backend = "d3d11va";
            }
        }
    }
}
