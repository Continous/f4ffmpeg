#include "producerWorker.h"
#include "pch.h"

namespace f4ffmpeg
{

bool producerWorker::start()
{
    if (running)
    {
        return false;
    }

    if (workerThread.joinable())
    {
        workerThread.join();
    }

    pendingFrame.store(
        nullptr,
        std::memory_order_release
    );

    latestFrame.store(
        nullptr,
        std::memory_order_release
    );

    stopRequested = false;
    running = true;

    workerThread = std::thread(
        &producerWorker::run,
        this
    );

    return true;
}

void producerWorker::submitFrame(
    std::shared_ptr<const decodeWorker::decodedFrame> frame,
    std::uint64_t sourceGeneration)
{
    if (!frame || !running)
    {
        return;
    }

    auto submission =
        std::make_shared<frameSubmission>();

    submission->frame = std::move(frame);
    submission->sourceGeneration = sourceGeneration;

    pendingFrame.store(
        std::move(submission),
        std::memory_order_release
    );

    wakeCondition.notify_one();
}

void producerWorker::run()
{
    while (!stopRequested)
    {
        {
            std::unique_lock<std::mutex> lock(
                wakeMutex
            );

            wakeCondition.wait(
                lock,
                [this]()
                {
                    return
                        stopRequested ||
                        pendingFrame.load(
                            std::memory_order_acquire
                        ) != nullptr;
                }
            );
        }

        if (stopRequested)
        {
            break;
        }

        auto submission =
            pendingFrame.exchange(
                nullptr,
                std::memory_order_acq_rel
            );

        if (
            !submission ||
            !submission->frame ||
            !submission->frame->frame)
        {
            continue;
        }

        auto produced =
            producer.frameProduce(
                submission->frame->frame.get()
            );

        if (!produced)
        {
            continue;
        }

        produced->timestamp =
            submission->frame->timestamp;

        produced->sourceGeneration =
            submission->sourceGeneration;

        latestFrame.store(
            std::move(produced),
            std::memory_order_release
        );
    }

    running = false;
}

std::shared_ptr<const producedFrame>
producerWorker::getLatestFrame() const
{
    return latestFrame.load(
        std::memory_order_acquire
    );
}

void producerWorker::stop()
{
    stopRequested = true;

    wakeCondition.notify_one();

    if (workerThread.joinable())
    {
        workerThread.join();
    }

    running = false;
}

producerWorker::~producerWorker()
{
    stop();
}

}
