#include "producerWorker.h"

namespace f4ffmpeg
{

bool producerWorker::start(const char* path)
{
    if (running)
    {
        return false;
    }

    if (workerThread.joinable())
    {
        workerThread.join();
    }

    if (path == nullptr)
    {
        return false;
    }

    outputPath = path;

    pendingFrame.store(
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

        producer.frameProduce(
            frame->frame.get(),
            outputPath.c_str()
        );
    }

    running = false;
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
