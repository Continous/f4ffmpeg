#pragma once

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace f4ffmpeg

{
    class decoder
    {
    public:
        ~decoder();

        bool open(const char* path);
        void close();


        bool initializeVideoDecoder(); //Where the cake gets baked.


    private:

        AVFormatContext* formatContext = nullptr;


        AVCodecContext* codecContext = nullptr;
        const AVCodec* videoCodec = nullptr;

        int videoStreamIndex = -1;
    };
}
