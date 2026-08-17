#pragma once

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace f4ffmpeg
{
    enum class decodeStatus
    {
        frameReady,
        endOfFile,
        stopped,
        ffmpegError
    };

    struct decodeResult
    {
        decodeStatus status;
        int ffmpegError = 0;
    };

    class decoder
    {
    public:
        ~decoder();

        bool open(const char* path);
        void close();

        bool frameProduce(
            const AVFrame* frame,
            const char* outputPath
        );

        bool initializeVideoDecoder();

        decodeResult decodeNextFrame(
            AVFrame* outputFrame
        );

        double getFrameTimestamp(
            const AVFrame* frame
        ) const;

    private:
        AVFormatContext* formatContext = nullptr;
        AVCodecContext* codecContext = nullptr;
        const AVCodec* videoCodec = nullptr;

        AVPacket* packet = nullptr;

        int videoStreamIndex = -1;

        bool decoderDraining = false;
        bool decoderEOF = false;
    };
}
