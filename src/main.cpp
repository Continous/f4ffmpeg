extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
}

#include <mutex>
#include <cmath>

#include <cstdint>
#include "playbackClock.h"
#include "config.h"
#include "decoder.h"
#include "api.h"
#include "graphics.h"
#include "tests.h"
#include "decoderWorker.h"
#include "manager.h"
#include "producerWorker.h"
#include "playbackClockSource.h"

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

    constexpr std::uint32_t debugDecodeKey =
        0x44; // F10

	void onF4SEMessage(F4SE::MessagingInterface::Message* message);
	void initF4ffmpeg();
    void startDebugDecode();
    void registerDebugInput();


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
			case F4SE::MessagingInterface::kInputLoaded:
			{
				registerDebugInput();
				break;
			}

			case F4SE::MessagingInterface::kGameDataReady:
			{
				if (!isGameReady)
				{
					isGameReady = true;

					REX::INFO(
						"F4SE reports game data ready, "
						"begin f4ffmpeg main initialization..."
					);

					initF4ffmpeg();
				}

				break;
			}

			default:
				break;
		}
	}

void initF4ffmpeg()
{
    f4ffmpeg::config::initialize();

    auto& clock =
        f4ffmpeg::playbackClock::get();

	// set mode, validate config, etc.

	if (
		!f4ffmpeg::playbackClockSource::get()
			.start())
	{
		REX::WARN(
			"Playback clock source was already running."
		);
	}
	else
	{
		REX::INFO(
			"Playback clock source initialized."
		);
	}

    const auto clockMode =
        f4ffmpeg::config::clockMode.GetValue();

    if (clock.setMode(clockMode))
    {
        REX::INFO(
            "Playback clock mode: '{}'.",
            clockMode
        );
    }
    else
    {
        REX::WARN(
            "Unknown playback clock mode '{}'. "
            "Defaulting to hybrid.",
            clockMode
        );

        clock.setMode(
            f4ffmpeg::playbackClockMode::hybrid
        );
    }

    const double maxFrameLag =
        f4ffmpeg::config::maxFrameLag.GetValue();

    if (
        !std::isfinite(maxFrameLag) ||
        maxFrameLag < 0.0)
    {
        REX::WARN(
            "MaxFrameLag value '{}' is invalid. "
            "Using 0.250 seconds.",
            maxFrameLag
        );
    }
    else
    {
        REX::INFO(
            "Max frame lag: {:.3f}s.",
            maxFrameLag
        );
    }

		f4ffmpeg::initializeFFmpegLogging();
		REX::INFO("Beginning f4ffmpeg graphics initialization");

		// Check current FFMPEG configuration
		REX::DEBUG(
			"FFmpeg libavcodec configuration: {}",
			avcodec_configuration()
		);
		REX::DEBUG(
			"FFmpeg compiled hardware device types:"
		);

		AVHWDeviceType type =
			AV_HWDEVICE_TYPE_NONE;

		while (
			(
				type =
					av_hwdevice_iterate_types(type)
			) != AV_HWDEVICE_TYPE_NONE)
		{
			const char* name =
				av_hwdevice_get_type_name(type);

			REX::INFO(
				"Hardware device: {}",
				name ? name : "unknown"
			);
		}

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

		REX::DEBUG(
			"Hardware decoding advertised: {}",
			reportedResults.hardwareDecodeAdvertised
		);

		for (const auto& codec : reportedResults.reportedCodecs)
		{
			REX::DEBUG(
				"Advertised: {} via {}",
				codec.codec,
				codec.backend
			);
		}

		REX::DEBUG(
			"Hardware decoding available: {}",
			hardwareResults.hardwareDecodeAvailable
		);

		for (const auto& codec : hardwareResults.codecs)
		{
			REX::DEBUG(
				"Available: {} via {}",
				codec.codec,
				codec.backend
			);
		}
}

void startDebugDecode()
{
    if (
        !isGameReady ||
        !isF4ffmpegGraphics)
    {
        REX::WARN(
            "Debug decode requested before "
            "f4ffmpeg was graphics ready."
        );

        return;
    }

    if (testManager)
    {
        REX::DEBUG(
            "Stopping existing debug decode."
        );

        testManager->stop();
        testManager.reset();
    }

    REX::INFO(
        "Starting debug decode."
    );

    testManager =
        f4ffmpeg::createManager(
            "Data/Video/f4ffmpeg/test.mp4",
            false
        );

    if (testManager)
    {
        REX::INFO(
            "Debug decode manager started."
        );
    }
    else
    {
        REX::ERROR(
            "Failed to start debug decode manager."
        );
    }
}

class DebugInputSink final :
    public RE::BSTEventSink<RE::InputEvent*>
{
public:
    static DebugInputSink* GetSingleton()
    {
        static DebugInputSink instance;
        return &instance;
    }

	RE::BSEventNotifyControl ProcessEvent(
		RE::InputEvent* const& eventList,
		RE::BSTEventSource<RE::InputEvent*>*
	) override
	{
		if (eventList == nullptr)
		{
			return RE::BSEventNotifyControl::kContinue;
		}

		for (
			auto* event = eventList;
			event != nullptr;
			event = event->next)
		{
			if (
				*event->eventType !=
					RE::INPUT_EVENT_TYPE::kButton ||
				*event->device !=
					RE::INPUT_DEVICE::kKeyboard)
			{
				continue;
			}

			auto* button =
				event->As<RE::ButtonEvent>();

			if (
				button == nullptr ||
				button->idCode != debugDecodeKey ||
				!button->JustPressed())
			{
				continue;
			}

			REX::DEBUG(
				"Debug decode hotkey pressed."
			);

			startDebugDecode();

			break;
		}

		return RE::BSEventNotifyControl::kContinue;
	}

private:
    DebugInputSink() = default;
};


void registerDebugInput()
{
    auto* inputManager =
        RE::BSInputDeviceManager::GetSingleton();

    if (inputManager == nullptr)
    {
        REX::ERROR(
            "Failed to register debug decode hotkey: "
            "input manager unavailable."
        );

        return;
    }

    inputManager->AddEventSink(
        DebugInputSink::GetSingleton()
    );

    REX::DEBUG(
        "Debug decode hotkey registered: F10."
    );
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
