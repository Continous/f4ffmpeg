#include "decoder.h"

namespace f4ffmpeg
{

void decoder::testHardwareDevices()
{
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    bool foundDeviceType = false;

    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
    {
        foundDeviceType = true;

        const char* deviceName = av_hwdevice_get_type_name(type);

        REX::INFO(
            "FFmpeg hardware backend reported available: {}",
            deviceName ? deviceName : "unknown"
        );

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
            REX::INFO(
                "Hardware backend '{}' is advertised, but could not be initialized.",
                deviceName ? deviceName : "unknown"
            );

            continue;
        }

        REX::INFO(
            "Hardware backend '{}' initialized successfully.",
            deviceName ? deviceName : "unknown"
        );

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
                    REX::INFO(
                        "Hardware decoder found: {}",
                        codec->name
                    );

                    break;
                }
            }
        }

        av_buffer_unref(&deviceContext);
    }

    if (!foundDeviceType)
    {
        REX::INFO("FFmpeg reports no hardware device backends.");
    }
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
        } //If there is not formatContext provided we can just close out. There's presumably no media being provided.

        REX::INFO("Attempting to open media: {}", path);

        if (avformat_open_input(
                &formatContext,
                path,
                nullptr,
                nullptr) < 0)
        {
            formatContext = nullptr; //Release formatContext, we failed to open.

            REX::ERROR("FFmpeg failed to open media: {}", path);

            return false;
        }

        if (avformat_find_stream_info(
                formatContext,
                nullptr) < 0)
        {
            REX::ERROR("FFmpeg failed to read stream information: {}", path);

            close();

            return false;
        }

        REX::INFO(
            "FFmpeg opened media successfully. Stream count: {}",
            formatContext->nb_streams
        );

        return true;
    }

        void decoder::close()
    {
        if (formatContext != nullptr)
        {
            avformat_close_input(&formatContext);
        }
    }
}
