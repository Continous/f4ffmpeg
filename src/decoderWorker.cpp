#include "decoderWorker.h"
#include "pch.h"
#include "config.h"
#include "playbackClock.h"

#include <algorithm>
#include <cmath>


extern "C"
{
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
}

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
    stopRequested.store(
        true,
        std::memory_order_release
    );

    playbackClock::get()
        .notifyWaiters();

    if (workerThread.joinable())
    {
        workerThread.join();
    }
}

namespace
{
    constexpr double lagDebugThreshold =
        0.100;

    constexpr double lagInfoThreshold =
        0.250;

    constexpr double lagWarnThreshold =
        0.500;

    constexpr double lagErrorThreshold =
        1.000;


    void logDecoderLag(
        double frameTimestamp,
        double clockTime,
        double lag)
    {
        if (lag >= lagErrorThreshold)
        {
            REX::ERROR(
                "Decoder returned frame time {:.3f}, "
                "but clock time is {:.3f} "
                "(lag: {:.3f}s). "
                "Did the decoder fall behind?",
                frameTimestamp,
                clockTime,
                lag
            );

            return;
        }

        if (lag >= lagWarnThreshold)
        {
            REX::WARN(
                "Decoder returned frame time {:.3f}, "
                "but clock time is {:.3f} "
                "(lag: {:.3f}s). "
                "Did the decoder fall behind?",
                frameTimestamp,
                clockTime,
                lag
            );

            return;
        }

        if (lag >= lagInfoThreshold)
        {
            REX::INFO(
                "Decoder returned frame time {:.3f}, "
                "but clock time is {:.3f} "
                "(lag: {:.3f}s). "
                "Did the decoder fall behind?",
                frameTimestamp,
                clockTime,
                lag
            );

            return;
        }

        if (lag >= lagDebugThreshold)
        {
            REX::DEBUG(
                "Decoder returned frame time {:.3f}, "
                "but clock time is {:.3f} "
                "(lag: {:.3f}s). "
                "Did the decoder fall behind?",
                frameTimestamp,
                clockTime,
                lag
            );
        }
    }
}


void decodeWorker::run()
{
    AVFrame* frame =
        av_frame_alloc();

    if (frame == nullptr)
    {
        lastStatus =
            decodeStatus::ffmpegError;

        lastFfmpegError =
            AVERROR(ENOMEM);

        running = false;

        return;
    }

    auto& clock =
        playbackClock::get();

    std::uint64_t lastClockUpdate =
        clock.getUpdateCount();

    std::uint64_t lastDiscontinuity =
        clock.getDiscontinuityCount();

    bool timelineInitialized =
        false;

    double clockOrigin =
        0.0;

    double firstFrameTimestamp =
        0.0;

    while (!stopRequested.load(
        std::memory_order_acquire))
    {
        // Do not even ask FFmpeg for another frame
        // until Fallout has issued another playback
        // clock update.
        if (!clock.waitForUpdate(
                lastClockUpdate,
                stopRequested))

        {
            break;
        }



        double seekTargetTimestamp = -1.0;
        bool recoveringFromSeek = false;
        const auto currentDiscontinuity =
            clock.getDiscontinuityCount();

        if (
            currentDiscontinuity !=
            lastDiscontinuity)
        {
            lastDiscontinuity =
                currentDiscontinuity;

            if (timelineInitialized)
            {
                const double elapsed =
                    clock.now() -
                    clockOrigin;

                const double duration =
                    workerDecoder
                        .getDuration();

                if (
                    duration > 0.0 &&
                    elapsed >= duration)
                {
                    REX::DEBUG(
                        "Playback clock advanced to {:.3f}s, "
                        "beyond video duration {:.3f}s. "
                        "Reporting logical EOF.",
                        elapsed,
                        duration
                    );

                    lastStatus =
                        decodeStatus::endOfFile;

                    lastFfmpegError = 0;

                    av_frame_free(
                        &frame
                    );

                    running = false;

                    return;
                }

                seekTargetTimestamp =
                    firstFrameTimestamp +
                    elapsed;

                if (workerDecoder.seek(
                        seekTargetTimestamp))
                {
                    recoveringFromSeek =
                        true;

                    REX::DEBUG(
                        "Playback clock discontinuity. "
                        "Seeking decoder toward frame time {:.3f}.",
                        seekTargetTimestamp
                    );
                }
                else
                {
                    REX::WARN(
                        "Failed to seek decoder after playback clock discontinuity. "
                        "Falling back to normal frame dropping."
                    );
                }
            }
        }

        decodeResult result{};
        double frameTimestamp = -1.0;

        while (!stopRequested.load(
            std::memory_order_acquire))
        {
            result =
                workerDecoder.decodeNextFrame(
                    frame
                );

            if (
                result.status !=
                decodeStatus::frameReady)
            {
                break;
            }

            frameTimestamp =
                workerDecoder
                    .getFrameTimestamp(
                        frame
                    );

            if (!std::isfinite(
                    frameTimestamp))
            {
                REX::WARN(
                    "Decoder returned a frame "
                    "without a usable timestamp."
                );

                continue;
            }

            if (
                recoveringFromSeek &&
                frameTimestamp <
                    seekTargetTimestamp)
            {
                // av_seek_frame() seeks to an earlier
                // keyframe. Decode forward without
                // publishing obsolete frames.
                continue;
            }

            break;
        }


        switch (result.status)
        {
            case decodeStatus::frameReady:
            {

                if (!std::isfinite(
                        frameTimestamp))
                {
                    REX::WARN(
                        "Decoder returned a frame "
                        "without a usable timestamp."
                    );

                    break;
                }

                // The first decoded frame establishes
                // this stream's relationship to the
                // global Fallout playback clock.
                if (!timelineInitialized)
                {
                    firstFrameTimestamp =
                        frameTimestamp;

                    clockOrigin =
                        clock.now();

                    timelineInitialized =
                        true;
                }

                const double targetClockTime =
                    clockOrigin +
                    (
                        frameTimestamp -
                        firstFrameTimestamp
                    );
                if (
                    clock.getDiscontinuityCount() !=
                    lastDiscontinuity)
                {
                    continue;
                }

                // The frame may have been decoded ahead
                // of presentation time. Hold it here
                // until Fallout advances sufficiently.
                if (!clock.waitUntil(
                        targetClockTime,
                        stopRequested))
                {
                    break;
                }

                if (
                    clock.getDiscontinuityCount() !=
                    lastDiscontinuity)
                {
                    continue;
                }
                // waitUntil() may have crossed several
                // Fallout updates. Consume all of them
                // so the next iteration cannot decode
                // another frame during the same update.
                lastClockUpdate =
                    clock.getUpdateCount();

                // Convert the global playback clock back
                // into this video's timestamp domain.
                const double clockTime =
                    firstFrameTimestamp +
                    (
                        clock.now() -
                        clockOrigin
                    );

                const double lag =
                    clockTime -
                    frameTimestamp;

                if (lag >= lagDebugThreshold)
                {
                    logDecoderLag(
                        frameTimestamp,
                        clockTime,
                        lag
                    );
                }

                double maxFrameLag =
                    config::maxFrameLag.GetValue();

                if (
                    !std::isfinite(maxFrameLag) ||
                    maxFrameLag < 0.0)
                {
                    maxFrameLag = 0.250;
                }

                if (lag > maxFrameLag)
                {
                    REX::TRACE(
                        "Dropping frame {:.3f}: "
                        "lag {:.3f}s exceeds "
                        "MaxFrameLag {:.3f}s.",
                        frameTimestamp,
                        lag,
                        maxFrameLag
                    );

                    break;
                }

                AVFrame* clonedFrame =
                    av_frame_clone(frame);

                if (clonedFrame == nullptr)
                {
                    break;
                }

                std::shared_ptr<AVFrame>
                    framePtr(
                        clonedFrame,
                        [](AVFrame* frame)
                        {
                            av_frame_free(
                                &frame
                            );
                        }
                    );

                auto published =
                    std::make_shared<
                        decodedFrame
                    >();

                published->frame =
                    std::move(framePtr);

                published->timestamp =
                    frameTimestamp;

                latestFrame.store(
                    std::move(published),
                    std::memory_order_release
                );

                break;
            }

            case decodeStatus::endOfFile:
            case decodeStatus::stopped:
            case decodeStatus::ffmpegError:
            {
                lastStatus =
                    result.status;

                lastFfmpegError =
                    result.ffmpegError;

                av_frame_free(
                    &frame
                );

                running = false;

                return;
            }
        }
    }

    lastStatus =
        decodeStatus::stopped;

    lastFfmpegError =
        0;

    av_frame_free(
        &frame
    );

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
