extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
}

#include "decoder.h"
#include "api.h"
#include "graphics.h"


extern "C"
__declspec(dllexport)
f4ffmpeg::api* f4ffmpegGetApi()
{
	return f4ffmpeg::getApi();
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
    F4SE::Init(a_f4se);

    REX::INFO("Hello World! I am the (F)allout (4) (FFMPEG) plugin.");
    REX::INFO("FFmpeg version: {}", av_version_info());

	if (!f4ffmpeg::initializeGraphics())
	{
		REX::ERROR("Failed to initialize graphics.");
		return false;
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

	f4ffmpeg::decoder testDecoder;

	testDecoder.testHardwareDevices();
	if (testDecoder.hasHardwareDecoder())
	{
		REX::INFO("Hardware decoding is available.");
		for (const auto& hardwareCodec : testDecoder.getHardwareCodecs())
		{
			REX::INFO("Backend: {}, Codec: {}",
					hardwareCodec.backend,
					hardwareCodec.codec
			);
		}

	}
	else
	{
		REX::INFO("No hardware decoding available.");
	}

    if (testDecoder.open("Data/Video/MainMenuLoop.bk2"))
    {
        REX::INFO("Successfully opened main menu video. FFMPEG is presumed functional.");
    }
    else
    {
        REX::ERROR("Failed to open main menu video. FFMPEG is presumed non-functional.");
    }

    //Debug video frame test.
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
	}

	REX::INFO("D3D11va is preferred, other backends may experience performance degradation. Any missing codec issues are not to be considered bugs, but rather client-side issues.");
	REX::INFO("Preferred codecs:");

	for (const auto& preferredCodec : testDecoder.getPreferredCodecs())
	{
		REX::INFO("Backend: {} Codec: {}", preferredCodec.backend, preferredCodec.codec);
	}
		REX::INFO("f4ffmpeg api initialized, with version {}",
			f4ffmpeg::apiVersion);
    return true;
}
