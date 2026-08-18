#include "decoderWorker.h"
#include "pch.h"

namespace f4ffmpeg
{


bool decodeWorker::start(const char* path)
{
    REX::INFO("Decode worker has been called.");
    if (running)
    {
        return false;
    }

    if (workerThread.joinable())
    {
        workerThread.join();
    }

    latestFrame.store(
        nullptr,
        std::memory_order_release
    );

    if (!workerDecoder.open(path))
    {
        return false;
    }

    if (!workerDecoder.initializeVideoDecoder())
    {
        REX::ERROR("Decode worker closed due to uninitialized Video Decoder.");
        workerDecoder.close();
        return false;
    }

    stopRequested = false;
    running = true;

    REX::INFO("Decode worker start has returned running = true.");

    workerThread = std::thread(
        &decodeWorker::run,
        this
    );

    return true;
}

void decodeWorker::stop()
{
    stopRequested = true;

    if (workerThread.joinable())
    {
        workerThread.join();
    }
}

void decodeWorker::run()
{

    REX::INFO("Decode worker is now running.");
    AVFrame* frame = av_frame_alloc();

    if (frame == nullptr)
    {

        lastStatus = decodeStatus::ffmpegError;
        lastFfmpegError = AVERROR(ENOMEM);

        running = false;
        return;
    }

    bool reportedD3D11Frame = false;

    while (!stopRequested)
    {
        const auto result =
            workerDecoder.decodeNextFrame(frame);

        switch (result.status)
        {
            case decodeStatus::frameReady:
            {
                if (!reportedFirstFrame)
                {
                    REX::INFO(
                        "First decoded frame format: {} "
                        "(AV_PIX_FMT_D3D11 = {})",
                        frame->format,
                        static_cast<int>(AV_PIX_FMT_D3D11)
                    );

                    reportedFirstFrame = true;
                }

                if (
                    !reportedD3D11Frame &&
                    frame->format == AV_PIX_FMT_D3D11)
                {
                    REX::INFO(
                        "Received hardware D3D11 frame: "
                        "texture={}, slice={}",
                        static_cast<void*>(
                            frame->data[0]
                        ),
                        reinterpret_cast<std::intptr_t>(
                            frame->data[1]
                        )
                    );

                    reportedD3D11Frame = true;
                }

            AVFrame* clonedFrame =
                av_frame_clone(frame);

            if (clonedFrame != nullptr)
            {
                std::shared_ptr<AVFrame> framePtr(
                    clonedFrame,
                    [](AVFrame* frame)
                    {
                        av_frame_free(&frame);
                    }
                );

                auto published =
                    std::make_shared<decodedFrame>();

                published->frame =
                    std::move(framePtr);

                published->timestamp =
                    workerDecoder.getFrameTimestamp(frame);

                latestFrame.store(
                    std::move(published),
                    std::memory_order_release
                );
            }

            break;
        }

        case decodeStatus::endOfFile:
            REX::INFO(
                "Decoder thread reached EOF."
            );

            lastStatus = result.status;
            lastFfmpegError = result.ffmpegError;

            av_frame_free(&frame);
            running = false;
            return;

        case decodeStatus::stopped:
            REX::INFO(
                "Decoder thread received stop command."
            );

            lastStatus = result.status;
            lastFfmpegError = result.ffmpegError;

            av_frame_free(&frame);
            running = false;
            return;

        case decodeStatus::ffmpegError:
            REX::ERROR(
                "Decoder thread received an FFmpeg error."
            );

            REX::ERROR(
                "Last status: {}",
                static_cast<int>(result.status)
            );

            REX::ERROR(
                "FFmpeg error: {}",
                result.ffmpegError
            );

            lastStatus = result.status;
            lastFfmpegError = result.ffmpegError;

            av_frame_free(&frame);
            running = false;
            return;
        }
    }

    lastStatus = decodeStatus::stopped;
    lastFfmpegError = 0;

    av_frame_free(&frame);
    running = false;
}

decodeResult decodeWorker::getLastResult() const
{
    return {
        lastStatus.load(),
        lastFfmpegError.load()
    };
}

bool decodeWorker::isRunning() const
{
    return running;
}

decodeWorker::~decodeWorker()
{
    stop();
}

std::shared_ptr<const decodeWorker::decodedFrame>
decodeWorker::getLatestFrame() const
{
    return latestFrame.load(
        std::memory_order_acquire
    );
}

}
