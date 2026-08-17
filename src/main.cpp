extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
}

#include <mutex>

#include "decoder.h"
#include "api.h"
#include "graphics.h"


extern "C"
__declspec(dllexport)
f4ffmpeg::api* f4ffmpegGetApi()
{
    return f4ffmpeg::getApi();
}


namespace Main
{
    static bool isInit = false;
    static bool isRuntimeInit = false;


    bool InitPlugin(const F4SE::LoadInterface* a_f4se)
    {
        if (isInit)
        {
            return true;
        }

        static std::once_flag once;

        std::call_once(once, [&]() {
            F4SE::Init(a_f4se);

            REX::INFO("Hello World! I am the (F)allout (4) (FFMPEG) plugin.");
            REX::INFO("FFmpeg version: {}", av_version_info());

            isInit = true;
        });

        return isInit;
    }


    bool InitRuntime()
    {
        if (isRuntimeInit)
        {
            return true;
        }

        static std::once_flag once;

        std::call_once(once, [&]() {

            // Initialize Fallout's graphics stack.
            if (!f4ffmpeg::initializeGraphics())
            {
                REX::ERROR("Failed to initialize graphics.");
                return;
            }

            REX::INFO(
                "Graphics initialized. Device: 0x{:X}, Context: 0x{:X}",
                reinterpret_cast<std::uintptr_t>(
                    f4ffmpeg::getD3D11Device()
                ),
                reinterpret_cast<std::uintptr_t>(
                    f4ffmpeg::getD3D11DeviceContext()
                )
            );


            // Debug decoder instance.
            f4ffmpeg::decoder testDecoder;


            // Hardware capability test.
            testDecoder.testHardwareDevices();

            if (testDecoder.hasHardwareDecoder())
            {
                REX::INFO("Hardware decoding is available.");

                for (const auto& hardwareCodec : testDecoder.getHardwareCodecs())
                {
                    REX::INFO(
                        "Backend: {}, Codec: {}",
                        hardwareCodec.backend,
                        hardwareCodec.codec
                    );
                }
            }
            else
            {
                REX::INFO("No hardware decoding available.");
            }


            // Basic FFmpeg sanity test using a Fallout video.
            if (testDecoder.open("Data/Video/MainMenuLoop.bk2"))
            {
                REX::INFO(
                    "Successfully opened main menu video. "
                    "FFMPEG is presumed functional."
                );
            }
            else
            {
                REX::ERROR(
                    "Failed to open main menu video. "
                    "FFMPEG is presumed non-functional."
                );
            }


            // Debug video frame decode test.
            if (testDecoder.open("Data/Video/f4ffmpeg/test.mp4"))
            {
                REX::INFO("Test media opened.");

                if (testDecoder.initializeVideoDecoder())
                {
                    REX::INFO("Video decoder initialized.");

                    if (testDecoder.decodeTestFrame())
                    {
                        REX::INFO("Successfully decoded a video frame.");
                    }
                    else
                    {
                        REX::ERROR("Failed to decode a video frame.");
                    }
                }
                else
                {
                    REX::ERROR("Failed to initialize video decoder.");
                }
            }
            else
            {
                REX::ERROR("Failed to open video decode test media.");
            }


            REX::INFO(
                "D3D11VA is preferred, other backends may experience "
                "performance degradation."
            );

            REX::INFO("Preferred codecs:");

            for (const auto& preferredCodec : testDecoder.getPreferredCodecs())
            {
                REX::INFO(
                    "Backend: {}, Codec: {}",
                    preferredCodec.backend,
                    preferredCodec.codec
                );
            }


            isRuntimeInit = true;
        });

        return isRuntimeInit;
    }


    F4SE_PLUGIN_QUERY(
        const F4SE::QueryInterface* a_f4se,
        F4SE::PluginInfo* a_info)
    {
        if (const auto data = F4SE::PluginVersionData::GetSingleton())
        {
            a_info->infoVersion = F4SE::PluginInfo::kVersion;
            a_info->name = data->GetPluginName().data();
            a_info->version = data->GetPluginVersion().pack();
        }

        const auto ver = a_f4se->RuntimeVersion();

        if (ver < REL::Version(F4SE::RUNTIME_1_10_163))
        {
            return false;
        }

        return true;
    }


    F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
    {
        // Preload may already have initialized F4SE.
        if (!InitPlugin(a_f4se))
        {
            return false;
        }

        // Graphics-dependent initialization belongs here, not PRELOAD.
        if (!InitRuntime())
        {
            return false;
        }

        REX::INFO(
            "f4ffmpeg api initialized, with version {}",
            f4ffmpeg::apiVersion
        );

        return true;
    }


    F4SE_PLUGIN_PRELOAD(const F4SE::LoadInterface* a_f4se)
    {
        // Keep preload graphics-independent.
        return InitPlugin(a_f4se);
    }
}
