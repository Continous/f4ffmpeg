#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <cstddef>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace f4ffmpeg
{

    struct producedFrameVulkanState
    {
        // Vulkan alias of Fallout's D3D11 texture.
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;

        // Vulkan-local conversion target.
        VkImage rgbaImage = VK_NULL_HANDLE;
        VkDeviceMemory rgbaMemory = VK_NULL_HANDLE;
        VkImageView rgbaView = VK_NULL_HANDLE;

        ...
    };

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

        std::uint32_t width = 0;
        std::uint32_t height = 0;

        double timestamp = -1.0;

        std::unique_ptr<
        producedFrameVulkanState
        > vulkanState;


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
        std::shared_ptr<producedFrame>
        createProducedFrame(
        int width,
        int height
        );

        std::shared_ptr<producedFrame>
        acquireProducedFrame(
            int width,
            int height
        );

        void clearProducedFrames();

        std::vector<std::shared_ptr<producedFrame>>
            producedFrames;

        std::mutex producedFramesMutex;

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

        AVFormatContext* formatContext = nullptr;
        AVCodecContext* codecContext = nullptr;

        AVBufferRef* hardwareDeviceContext = nullptr;

        AVHWDeviceType hardwareDeviceType =
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
