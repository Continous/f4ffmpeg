#pragma once

#include <cstdint>
#include <memory>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace REX::W32
{
    struct ID3D11Texture2D;
    struct ID3D11ShaderResourceView;
}

namespace f4ffmpeg
{
    void initializeFFmpegLogging();

    void frameDump(
        const char* outputPath
    );

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

    // Canonical producer output. Regardless of whether FFmpeg decoded with
    // Vulkan or D3D11VA, a successful frameProduce() returns a Fallout-device
    // RGBA texture and its ready-to-bind SRV.
    struct producedFrame
    {
        producedFrame() = default;

        producedFrame(
            const producedFrame&
        ) = delete;

        producedFrame& operator=(
            const producedFrame&
        ) = delete;

        REX::W32::ID3D11Texture2D* texture = nullptr;
        REX::W32::ID3D11ShaderResourceView* resourceView = nullptr;

        std::uint32_t width = 0;
        std::uint32_t height = 0;

        double timestamp = -1.0;

        ~producedFrame();
    };

    class decoder
    {
    public:
        ~decoder();

        bool open(const char* path);
        void close();

        std::shared_ptr<producedFrame>
        frameProduce(const AVFrame* frame);

        bool initializeVideoDecoder();

        decodeResult decodeNextFrame(
            AVFrame* outputFrame
        );

        double getFrameTimestamp(
            const AVFrame* frame
        ) const;

        double getCurrentTimestamp() const;
        double getDuration() const;

        bool seek(
            double timestamp
        );

    private:
        bool frameProduceVulkan(
            const AVFrame* frame,
            producedFrame& output
        );

        bool frameProduceD3D11(
            const AVFrame* frame,
            producedFrame& output
        );

        bool initializeHardwareDevice(
            AVHWDeviceType deviceType
        );

        bool ensureHardwareDevice(
            AVHWDeviceType deviceType
        );

        void closeSource();

        AVFormatContext* formatContext = nullptr;
        AVCodecContext* codecContext = nullptr;

        AVBufferRef* hardwareDeviceContext = nullptr;

        AVHWDeviceType hardwareDeviceType =
            AV_HWDEVICE_TYPE_NONE;

        AVHWDeviceType initializedHardwareDeviceType =
            AV_HWDEVICE_TYPE_NONE;

        AVPixelFormat hardwarePixelFormat =
            AV_PIX_FMT_NONE;

        const AVCodec* videoCodec = nullptr;

        AVPacket* packet = nullptr;

        int videoStreamIndex = -1;

        bool decoderDraining = false;
        bool decoderEOF = false;
        double currentTimestamp = -1.0;

        std::uint64_t handledDecodedFrameDumpGeneration = 0;
        std::uint64_t handledProducedFrameDumpGeneration = 0;
    };
}
