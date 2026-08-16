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
        void testHardwareDevices();

    private:
        AVFormatContext* formatContext = nullptr;
    };
}
