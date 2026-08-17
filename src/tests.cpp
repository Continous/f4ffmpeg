extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
}

#include <mutex>

#include "tests.h"

namespace f4ffmpeg
{

    namespace
    {
        hardwareTestResults supportedCodecs;
    }

    reportedCodecsResults testHardwareDevices()
    {
        reportedCodecsResults results;

        AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;

        while ((type = av_hwdevice_iterate_types(type))
               != AV_HWDEVICE_TYPE_NONE)
        {
            AVBufferRef* deviceContext = nullptr;

            const int result = av_hwdevice_ctx_create(
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

            const char* deviceName =
                av_hwdevice_get_type_name(type);

            void* iterator = nullptr;
            const AVCodec* codec = nullptr;

            while ((codec = av_codec_iterate(&iterator)) != nullptr)
            {
                if (!av_codec_is_decoder(codec))
                {
                    continue;
                }

                for (int i = 0;; ++i)
                {
                    const AVCodecHWConfig* config =
                        avcodec_get_hw_config(codec, i);

                    if (config == nullptr)
                    {
                        break;
                    }

                    if (config->device_type == type)
                    {
                        results.hardwareDecodeAdvertised = true;

                        results.reportedCodecs.push_back({
                            deviceName ? deviceName : "unknown",
                            codec->name
                        });

                        break;
                    }
                }
            }

            av_buffer_unref(&deviceContext);
        }

        return results;
    }

    hardwareTestResults testHardwareCodecs(
        const reportedCodecsResults& reportedResults)
    {
        hardwareTestResults results;
        for (const auto& reportedCodec : reportedResults.reportedCodecs)
        {
        AVHWDeviceType deviceType =
            av_hwdevice_find_type_by_name(
                reportedCodec.backend.c_str()
            );

        if (deviceType == AV_HWDEVICE_TYPE_NONE)
        {
            continue;
        }

        const AVCodec* codec =
            avcodec_find_decoder_by_name(
                reportedCodec.codec.c_str()
            );

        if (codec == nullptr)
        {
            continue;
        }

        // Find the matching hardware configuration.
        const AVCodecHWConfig* hardwareConfig = nullptr;

        for (int i = 0;; ++i)
        {
            const AVCodecHWConfig* config =
                avcodec_get_hw_config(codec, i);

            if (config == nullptr)
            {
                break;
            }

            if (
                config->device_type == deviceType &&
                (config->methods &
                 AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
            )
            {
                hardwareConfig = config;
                break;
            }
        }

        if (hardwareConfig == nullptr)
        {
            continue;
        }

        AVBufferRef* deviceContext = nullptr;

        if (av_hwdevice_ctx_create(
                &deviceContext,
                deviceType,
                nullptr,
                nullptr,
                0) < 0)
        {
            continue;
        }

        AVCodecContext* codecContext =
            avcodec_alloc_context3(codec);

        if (codecContext == nullptr)
        {
            av_buffer_unref(&deviceContext);
            continue;
        }

        codecContext->hw_device_ctx =
            av_buffer_ref(deviceContext);

        if (codecContext->hw_device_ctx == nullptr)
        {
            avcodec_free_context(&codecContext);
            av_buffer_unref(&deviceContext);
            continue;
        }

        if (avcodec_open2(
                codecContext,
                codec,
                nullptr) < 0)
        {
            avcodec_free_context(&codecContext);
            av_buffer_unref(&deviceContext);
            continue;
        }

        results.hardwareDecodeAvailable = true;

        results.codecs.push_back({
            reportedCodec.backend,
            reportedCodec.codec
        });

        avcodec_free_context(&codecContext);
        av_buffer_unref(&deviceContext);
    }
    supportedCodecs = results;

    return results;
    }


    std::size_t getSupportedCodecCount()
    {
        return supportedCodecs.codecs.size();
    }

    const hardwareCodec* getSupportedCodec(
        std::size_t index)
    {
        if (index >= supportedCodecs.codecs.size())
        {
            return nullptr;
        }

        return &supportedCodecs.codecs[index];
    }

}
