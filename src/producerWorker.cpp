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
    std::shared_ptr<const decodeWorker::decodedFrame> frame)
{
    if (!frame || !running)
    {
        return;
    }

    pendingFrame.store(
        std::move(frame),
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

        auto frame =
            pendingFrame.exchange(
                nullptr,
                std::memory_order_acq_rel
            );

        if (!frame || !frame->frame)
        {
            continue;
        }

        auto produced =
            producer.frameProduce(
                frame->frame.get()
            );

        if (!produced)
        {
            continue;
        }

        produced->timestamp =
            frame->timestamp;

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
