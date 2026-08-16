#pragma once

#include <vector>
#include <string>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace f4ffmpeg

{
    struct hardwareCodec //Construct hardwareCodec to show both backend and codec.
    {
        std::string backend;
        std::string codec;
    };
        struct preferredCodec //Construct hardwareCodec to show both backend and codec.
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


        bool initializeVideoDecoder(); //Where the cake gets baked.
        bool decodeTestFrame(); //Where we check that we're actually baking cake

        bool hasHardwareDecoder() const; //If there is a hardware decoder.
        const std::vector<hardwareCodec>& getHardwareCodecs() const;
        const std::vector<preferredCodec>& getPreferredCodecs() const;

    private:
        void buildPreferredCodecs();

        AVFormatContext* formatContext = nullptr;

        bool hardwareDecoder = false;

        std::vector<hardwareCodec> hardwareCodecs;
        std::vector<preferredCodec> preferredCodecs;

        AVCodecContext* codecContext = nullptr;
        const AVCodec* videoCodec = nullptr;

        int videoStreamIndex = -1;
    };
}
