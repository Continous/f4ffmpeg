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


bool decoder::initializeVideoDecoder()
{
    if (formatContext == nullptr)
    {
        return false;
    }
    if (codecContext != nullptr)
    {
        avcodec_free_context(&codecContext);
    }
    videoStreamIndex = av_find_best_stream(
        formatContext,
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        &videoCodec,
        0
    );

    if (videoStreamIndex < 0)
    {
        return false;
    }

    codecContext = avcodec_alloc_context3(videoCodec);

    if (codecContext == nullptr)
    {
        return false;
    }

    AVStream* videoStream =
        formatContext->streams[videoStreamIndex];

    if (avcodec_parameters_to_context(
            codecContext,
            videoStream->codecpar) < 0)
    {
        avcodec_free_context(&codecContext);
        return false;
    }

    if (avcodec_open2(
            codecContext,
            videoCodec,
            nullptr) < 0)
    {
        avcodec_free_context(&codecContext);
        return false;
    }

    return true;
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
        if (codecContext != nullptr)
        {
            avcodec_free_context(&codecContext);
        }
        if (formatContext != nullptr)
        {
            avformat_close_input(&formatContext);
        }
        videoCodec = nullptr;
        videoStreamIndex = -1;
    }

}
