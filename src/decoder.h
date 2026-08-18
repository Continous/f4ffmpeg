#pragma once

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace f4ffmpeg
{

        void initializeFFmpegLogging();

        enum class frameProduceMethod
    {
        bitmap,
        d3d11Texture
    };

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
            frameProduceMethod method,
            const char* outputPath = nullptr
        );

        bool frameProduceBitmap(
            const AVFrame* frame,
            const char* outputPath
        );

        bool frameProduceD3D11Texture(
            const AVFrame* frame
        );

        bool initializeVideoDecoder();

        decodeResult decodeNextFrame(
            AVFrame* outputFrame
        );

        double getFrameTimestamp(
            const AVFrame* frame
        ) const;

        double getCurrentTimestamp() const;

    private:
        AVFormatContext* formatContext = nullptr;
        AVCodecContext* codecContext = nullptr;

        AVBufferRef* hardwareDeviceContext = nullptr;
        bool initializeD3D11Device();

        const AVCodec* videoCodec = nullptr;

        AVPacket* packet = nullptr;

        int videoStreamIndex = -1;

        bool decoderDraining = false;
        bool decoderEOF = false;
        double currentTimestamp = -1.0;
    };
}
