extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include "decoder.h"

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
    F4SE::Init(a_f4se);

    REX::INFO("Hello World! I am the (F)allout (4) (FFMPEG) plugin.");
    REX::INFO("FFmpeg version: {}", av_version_info());

    f4ffmpeg::decoder testDecoder;

    if (testDecoder.open("Data/Video/MainMenuLoop.bk2"))
    {
        REX::INFO("Successfully opened intro video.");
    }
    else
    {
        REX::ERROR("Failed to open intro video. FFMPEG is presumed non-functional.");
    }

    return true;
}
