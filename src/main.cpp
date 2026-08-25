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
#include "nifHandler.h"
#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

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
    void startDebugDecode();
    void registerDebugInput();
	void dumpDebugFrames();

	void dumpDebugFrames()
	{
		if (
			!isGameReady ||
			!isF4ffmpegGraphics)
		{
			REX::WARN(
				"Frame dump requested before "
				"f4ffmpeg was graphics ready."
			);

			return;
		}

		const auto outputPath =
			f4ffmpeg::config::
				debugFrameDumpPath.GetValue();

		f4ffmpeg::frameDump(
			outputPath.c_str()
		);
		REX::INFO("Frame dump requested to {}", outputPath);
	}


    bool InitPlugin(const F4SE::LoadInterface* a_f4se)
    {

        if (isInit)
            return true;

        static std::once_flag once;
        std::call_once(once, [&]() {
            F4SE::Init(a_f4se);

			auto* messaging = F4SE::GetMessagingInterface();

            REX::INFO("f4ffmpeg plugin initialized. Waiting for game to load... ");
            f4ffmpeg::initializeNifHandler();

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
			{
				if (!isGameReady)
				{
					isGameReady = true;

					REX::INFO(
						"F4SE reports game data ready, "
						"begin f4ffmpeg main initialization..."
					);

					initF4ffmpeg();
					registerDebugInput();
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


RE::BS_BUTTON_CODE debugDecodeKey =
	RE::BS_BUTTON_CODE::kF10;

RE::BS_BUTTON_CODE debugFrameDumpKey =
	RE::BS_BUTTON_CODE::kF11;

class DebugInputHandler final :
    public RE::PlayerInputHandler
{
public:
    explicit DebugInputHandler(
        RE::PlayerControlsData& data) :
        RE::PlayerInputHandler(data)
    {}

    bool ShouldHandleEvent(
        const RE::InputEvent* event) override
    {
        return
            event != nullptr &&
            *event->eventType ==
                RE::INPUT_EVENT_TYPE::kButton &&
            *event->device ==
                RE::INPUT_DEVICE::kKeyboard;
    }

void OnButtonEvent(
    const RE::ButtonEvent* button) override
{
    if (
        button == nullptr ||
        !button->QJustPressed())
    {
        return;
    }

    const auto key =
        button->GetBSButtonCode();

    if (key == debugDecodeKey)
    {
        REX::DEBUG(
            "Debug decode hotkey pressed."
        );

        startDebugDecode();
        return;
    }

    if (key == debugFrameDumpKey)
    {
        REX::DEBUG(
            "Debug frame dump hotkey pressed."
        );

        dumpDebugFrames();
        return;
    }
}
};


	std::optional<RE::BS_BUTTON_CODE>
	parseDebugKey(std::string_view key)
	{
		static constexpr std::array mappings{
			std::pair{"F1",  RE::BS_BUTTON_CODE::kF1},
			std::pair{"F2",  RE::BS_BUTTON_CODE::kF2},
			std::pair{"F3",  RE::BS_BUTTON_CODE::kF3},
			std::pair{"F4",  RE::BS_BUTTON_CODE::kF4},
			std::pair{"F5",  RE::BS_BUTTON_CODE::kF5},
			std::pair{"F6",  RE::BS_BUTTON_CODE::kF6},
			std::pair{"F7",  RE::BS_BUTTON_CODE::kF7},
			std::pair{"F8",  RE::BS_BUTTON_CODE::kF8},
			std::pair{"F9",  RE::BS_BUTTON_CODE::kF9},
			std::pair{"F10", RE::BS_BUTTON_CODE::kF10},
			std::pair{"F11", RE::BS_BUTTON_CODE::kF11},
			std::pair{"F12", RE::BS_BUTTON_CODE::kF12}
		};

		std::string normalized{key};

		std::transform(
			normalized.begin(),
			normalized.end(),
			normalized.begin(),
			[](unsigned char c)
			{
				return static_cast<char>(std::toupper(c));
			}
		);

		for (const auto& [name, code] : mappings)
		{
			if (normalized == name)
			{
				return code;
			}
		}

		return std::nullopt;
	}

void registerDebugInput()
{
    const auto configuredDecodeKey =
        f4ffmpeg::config::debugDecodeKey.GetValue();

    if (const auto key =
            parseDebugKey(configuredDecodeKey))
    {
        debugDecodeKey = *key;
    }
    else
    {
        REX::WARN(
            "Unknown Debug.DecodeKey '{}'; using F10.",
            configuredDecodeKey
        );
    }

    const auto configuredDumpKey =
        f4ffmpeg::config::debugFrameDumpKey.GetValue();

    if (const auto key =
            parseDebugKey(configuredDumpKey))
    {
        debugFrameDumpKey = *key;
    }
    else
    {
        REX::WARN(
            "Unknown Debug.FrameDumpKey '{}'; using F11.",
            configuredDumpKey
        );
    }

    auto* playerControls =
        RE::PlayerControls::GetSingleton();

    if (playerControls == nullptr)
    {
        REX::ERROR(
            "Failed to register debug decode hotkey: "
            "player controls unavailable."
        );

        return;
    }

    static DebugInputHandler debugInput{
        playerControls->data
    };

    playerControls->RegisterHandler(
        &debugInput
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
