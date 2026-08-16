#pragma once

extern "C"
{
#include <libavformat/avformat.h>
}

namespace f4ffmpeg

{
    class decoder
    {
    public:
        ~decoder();

        bool open(const char* path);
        void close();

        bool supportsHardwareDecode(AVHWDeviceType deviceType);

    private:
        AVFormatContext* formatContext = nullptr;
    };
}
