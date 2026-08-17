#include "manager.h"

#include <chrono>

namespace f4ffmpeg
{

bool manager::start(
    const char* inputPath,
    const char* outputPath)
{
    if (running)
    {
        return false;
    }

    if (managerThread.joinable())
    {
        managerThread.join();
    }

    if (!decoderWorker.start(inputPath))
    {
        return false;
    }

    if (!producerWorker.start(outputPath))
    {
        decoderWorker.stop();
        return false;
    }

    lastSubmittedFrame = nullptr;

    stopRequested = false;
    running = true;

    managerThread = std::thread(
        &manager::run,
        this
    );

    return true;
}
void manager::run()
{
    while (!stopRequested)
    {
        auto frame =
            decoderWorker.getLatestFrame();

        if (frame && frame != lastSubmittedFrame)
        {
            producerWorker.submitFrame(frame);

            lastSubmittedFrame = frame;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(1)
        );
    }

    running = false;
}
void manager::stop()
{
    stopRequested = true;

    if (managerThread.joinable())
    {
        managerThread.join();
    }

    decoderWorker.stop();
    producerWorker.stop();

    running = false;
}

manager::~manager()
{
    stop();
}

std::shared_ptr<manager> createManager(
    const char* inputPath,
    const char* outputPath)
{
    auto newManager =
        std::make_shared<manager>();

    if (!newManager->start(
            inputPath,
            outputPath))
    {
        return nullptr;
    }

    return newManager;
}

}
