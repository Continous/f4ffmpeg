#include "manager.h"
#include "pch.h"

#include <chrono>

namespace f4ffmpeg
{

bool manager::start(
    const char* inputPath)
{
    if (inputPath == nullptr)
    {
        return false;
    }

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

    if (!producerWorker.start())
    {
        decoderWorker.stop();
        return false;
    }

    this->inputPath = inputPath;

    lastSubmittedFrame = nullptr;

    stopRequested = false;
    running = true;

    managerThread = std::thread(
        &manager::run,
        this
    );

    return true;
}

std::shared_ptr<const producedFrame>
manager::getLatestFrame() const
{
    return producerWorker.getLatestFrame();
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

        if (!decoderWorker.isRunning())
        {
            const auto result =
                decoderWorker.getLastResult();

            switch (result.status)
            {
                case decodeStatus::endOfFile:
                    if (looping)
                    {
                        lastSubmittedFrame = nullptr;

                        if (decoderWorker.start(
                                inputPath.c_str()))
                        {
                            REX::TRACE(
                                "Manager sent loop request."
                            );
                        }
                        else
                        {
                            REX::ERROR(
                                "Manager reported failed loop request."
                            );

                            stopRequested = true;
                        }
                    }
                    else
                    {
                        stopRequested = true;
                    }

                    break;

            case decodeStatus::ffmpegError:
                stopRequested = true;
                break;

            case decodeStatus::stopped:
                stopRequested = true;
                break;

            case decodeStatus::frameReady:
                break;
            }
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
    bool looping)
{
    auto newManager =
        std::make_shared<manager>();

    newManager->setLooping(looping);

    if (!newManager->start(inputPath))
    {
        return nullptr;
    }

    return newManager;
}

}
