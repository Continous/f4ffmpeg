#include "decoderWorker.h"

namespace f4ffmpeg
{


bool decodeWorker::start(const char* path)
{
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
        workerDecoder.close();
        return false;
    }

    stopRequested = false;
    running = true;

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
    AVFrame* frame = av_frame_alloc();

    if (frame == nullptr)
    {
        lastResult = {
            decodeStatus::ffmpegError,
            AVERROR(ENOMEM)
        };

        running = false;
        return;
    }

    while (!stopRequested)
    {
        const auto result =
            workerDecoder.decodeNextFrame(frame);

        switch (result.status)
        {
        case decodeStatus::frameReady:
        {
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
        case decodeStatus::stopped:
        case decodeStatus::ffmpegError:
            lastResult = result;

            av_frame_free(&frame);
            running = false;
            return;
        }
    }

    lastResult = {
        decodeStatus::stopped,
        0
    };

    av_frame_free(&frame);
    running = false;
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
