#pragma once

#include <vector>
#include <string>

extern "C"
{
#include <libavformat/avformat.h>
}

namespace f4ffmpeg

{
    struct hardwareCodec //Construct hardwareCodec to show both backend and codec.
    {
        std::string backend;
        std::string codec;
    };

    class decoder
    {
    public:
        ~decoder();

        bool open(const char* path);
        void close();
        void testHardwareDevices();

        bool hasHardwareDecoder() const; //If there is a hardware decoder.
        const std::vector<hardwareCodec>& getHardwareCodecs() const;

    private:
        AVFormatContext* formatContext = nullptr;

        bool hardwareDecoder = false;
        std::vector<hardwareCodec> hardwareCodecs;
    };
}
