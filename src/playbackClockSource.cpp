#include "pch.h"

#include "playbackClockSource.h"
#include "playbackClock.h"

#include <chrono>

namespace f4ffmpeg
{
    playbackClockSource&
    playbackClockSource::get()
    {
        static playbackClockSource instance;
        return instance;
    }


    bool playbackClockSource::start()
    {
        if (running.load(
                std::memory_order_acquire))
        {
            return false;
        }

        if (workerThread.joinable())
        {
            workerThread.join();
        }

        stopRequested.store(
            false,
            std::memory_order_release
        );

        running.store(
            true,
            std::memory_order_release
        );

        workerThread =
            std::thread(
                &playbackClockSource::run,
                this
            );

        return true;
    }


    void playbackClockSource::run()
    {
        bool baselineValid = false;


        std::uint64_t lastTimerTime =
            0;


        while (!stopRequested.load(
            std::memory_order_acquire))
        {
            auto* timer =
                RE::BSTimer::GetSingleton();

            if (timer != nullptr)
            {
                const auto timerTime =
                    timer->lastTime;

                if (!baselineValid)
                {
                    lastTimerTime =
                        timerTime;

                    baselineValid =
                        true;

                    REX::TRACE(
                        "Playback clock source "
                        "established BSTimer baseline: {}.",
                        timerTime
                    );
                }
                else if (
                    timerTime !=
                    lastTimerTime)
                {

                    REX::TRACE(
                        "BSTimer update observed: {} -> {}.",
                        lastTimerTime,
                        timerTime
                    );

                    if (
                        playbackClock::get()
                        .update(timerTime))
                    {
                        lastTimerTime =
                        timerTime;
                    }
                }
            }
            else
            {
                baselineValid =
                    false;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(2)
            );
        }

        running.store(
            false,
            std::memory_order_release
        );
    }


    void playbackClockSource::stop()
    {
        stopRequested.store(
            true,
            std::memory_order_release
        );

        if (workerThread.joinable())
        {
            workerThread.join();
        }

        running.store(
            false,
            std::memory_order_release
        );
    }


    bool playbackClockSource::isRunning() const
    {
        return running.load(
            std::memory_order_acquire
        );
    }


    playbackClockSource::~playbackClockSource()
    {
        stop();
    }
}
