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
#include "tests.h"
#include "decoderWorker.h"
#include "manager.h"
#include "producerWorker.h"


extern "C"
__declspec(dllexport)
f4ffmpeg::api* f4ffmpegGetApi()
{
    return f4ffmpeg::getApi();
}


namespace Main
{
    static bool isInit = false;
	static bool isGameReady = false;
	static bool isF4ffmpegGraphics = false;

	static std::shared_ptr<f4ffmpeg::manager>
    testManager;

	void onF4SEMessage(F4SE::MessagingInterface::Message* message);
	void initF4ffmpeg();


    bool InitPlugin(const F4SE::LoadInterface* a_f4se)
    {
        if (isInit)
            return true;

        static std::once_flag once;
        std::call_once(once, [&]() {
            F4SE::Init(a_f4se);

			auto* messaging = F4SE::GetMessagingInterface();

            REX::INFO("f4ffmpeg plugin initialized. Waiting for game to load... ");

			if (messaging != nullptr)
			{
				if (!messaging->RegisterListener(onF4SEMessage))
				{
					REX::WARN(
						"f4ffmpeg failed to register F4SE messaging listener. "
						"Consider functionality heavily degraded. "
					);
				}
			}
			else
			{
				REX::WARN(
					"F4SE messaging interface is unavailable for ffmpeg. "
					"Consider functionality heavily degraded. "
				);
			}

            isInit = true;
        });

        return isInit;
    }

    void onF4SEMessage(F4SE::MessagingInterface::Message* message)
	{
		if (message == nullptr)
			return;

		switch (message->type)
		{
			case F4SE::MessagingInterface::kGameDataReady:
				if (!isGameReady)
				{
					isGameReady = true;
					REX::INFO("F4SE reports game data ready, begin f4ffmpeg main initialization...");

					initF4ffmpeg();
					break;
				}
			default:
				break;
		}
	}

	void initF4ffmpeg()
	{
		REX::INFO("Beginning f4ffmpeg graphics initialization");

		if(!f4ffmpeg::initializeGraphics())
		{
			REX::ERROR(
				"Failed to initialize graphics. f4ffmpeg is initialized, but without graphics. "
				"Consider functionality heavily degraded."
			);
			return;
		}
		isF4ffmpegGraphics = true;
		REX::INFO("f4ffmpeg is graphics ready!");

		auto reportedResults =
			f4ffmpeg::testHardwareDevices();

		auto hardwareResults =
			f4ffmpeg::testHardwareCodecs(reportedResults);

		REX::INFO(
			"Hardware decoding advertised: {}",
			reportedResults.hardwareDecodeAdvertised
		);

		for (const auto& codec : reportedResults.reportedCodecs)
		{
			REX::INFO(
				"Advertised: {} via {}",
				codec.codec,
				codec.backend
			);
		}

		REX::INFO(
			"Hardware decoding available: {}",
			hardwareResults.hardwareDecodeAvailable
		);

		for (const auto& codec : hardwareResults.codecs)
		{
			REX::INFO(
				"Available: {} via {}",
				codec.codec,
				codec.backend
			);
		}
		testManager =
		f4ffmpeg::createManager(
			"Data/Video/f4ffmpeg/test.mp4",
			f4ffmpeg::producerOutput::d3d11Texture,
			nullptr,
			true
		);
		if (testManager)
		{
			REX::INFO(
				"Test manager started successfully with looping disabled."
			);
		}
		else
		{
			REX::ERROR(
				"Failed to start test manager."
			);
		}
	}
    F4SE_PLUGIN_QUERY(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
    {
        if (const auto data = F4SE::PluginVersionData::GetSingleton())
        {
            a_info->infoVersion = F4SE::PluginInfo::kVersion;
            a_info->name = data->GetPluginName().data();
            a_info->version = data->GetPluginVersion().pack();
        }

        const auto ver = a_f4se->RuntimeVersion();
        if (ver < REL::Version(F4SE::RUNTIME_1_10_163))
            return false;

        return true;
    }

    F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
	{
        // OG does not support PreLoading
		return InitPlugin(a_f4se);
	}

    F4SE_PLUGIN_PRELOAD(const F4SE::LoadInterface* a_f4se)
    {
        return InitPlugin(a_f4se);
    }
}
