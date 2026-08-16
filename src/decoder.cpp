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
        if (formatContext != nullptr)
        {
            avformat_close_input(&formatContext);
        }
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
