extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
}

#include "decoder.h"
#include "api.h"


extern "C"
__declspec(dllexport)
f4ffmpeg::api* f4ffmpegGetApi()
{
	return f4ffmpeg::getApi();
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
    F4SE::Init(a_f4se);

	//Check Address Library pointer
	REL::Relocation<std::uintptr_t> d3d11DeviceLocation{
	REL::ID(633829)
	};

	REX::INFO(
		"D3D11 device relocation resolved to 0x{:X}",
		d3d11DeviceLocation.address()
	);

	auto deviceValue =
		*reinterpret_cast<std::uintptr_t*>(
			d3d11DeviceLocation.address()
		);

	REX::INFO(
		"D3D11 device relocation contains 0x{:X}",
		deviceValue
	);


    REX::INFO("Hello World! I am the (F)allout (4) (FFMPEG) plugin.");
    REX::INFO("FFmpeg version: {}", av_version_info());

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

	REX::INFO("D3D11va is preferred, other backends may experience performance degradation.");
	REX::INFO("Preferred codecs:");

	for (const auto& preferredCodec : testDecoder.getPreferredCodecs())
	{
		REX::INFO("Backend: {} Codec: {}", preferredCodec.backend, preferredCodec.codec);
	}
		REX::INFO("f4ffmpeg api initialized, with version {}",
			f4ffmpeg::apiVersion);
    return true;
}
