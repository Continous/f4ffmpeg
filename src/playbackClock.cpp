#include "pch.h"

#include "playbackClock.h"
#include <cmath>

namespace f4ffmpeg
{
    namespace
    {
        constexpr double secondsPerHour =
            3600.0;

        constexpr double timeScaleEpsilon =
            0.001;

        double sanitizeElapsed(
            double value)
        {
            if (
                !std::isfinite(value) ||
                value <= 0.0)
            {
                return 0.0;
            }

            return value;
        }
    }


    playbackClock&
    playbackClock::get()
    {
        static playbackClock instance;
        return instance;
    }

    std::uint64_t
    playbackClock::getDiscontinuityCount() const
    {
        std::scoped_lock lock(mutex);

        return discontinuityCount;
    }

    void playbackClock::setMode(
        playbackClockMode newMode)
    {
        {
            std::scoped_lock lock(mutex);

            mode = newMode;
        }

        condition.notify_all();
    }


    bool playbackClock::setMode(
        std::string_view newMode)
    {
        if (
            newMode == "realtime" ||
            newMode == "real")
        {
            setMode(
                playbackClockMode::realtime
            );

            return true;
        }

        if (
            newMode == "game" ||
            newMode == "gametime")
        {
            setMode(
                playbackClockMode::game
            );

            return true;
        }

        if (newMode == "hybrid")
        {
            setMode(
                playbackClockMode::hybrid
            );

            return true;
        }

        return false;
    }


    playbackClockMode
    playbackClock::getMode() const
    {
        std::scoped_lock lock(mutex);

        return mode;
    }


    std::uint64_t
    playbackClock::getUpdateCount() const
    {
        std::scoped_lock lock(mutex);

        return updateCount;
    }


    bool playbackClock::waitForUpdate(
        std::uint64_t& lastUpdate,
        const std::atomic<bool>& stopRequested)
    {
        std::unique_lock lock(mutex);

        condition.wait(
            lock,
            [&]()
            {
                return
                    stopRequested.load(
                        std::memory_order_acquire
                    ) ||
                    updateCount != lastUpdate;
            }
        );

        if (stopRequested.load(
                std::memory_order_acquire))
        {
            return false;
        }

        lastUpdate = updateCount;

        return true;
    }

    void playbackClock::update(
        double realSecondsElapsed,
        double gameHoursElapsed,
        double timeScale,
        bool discontinuity)
    {
        const double realAdvance =
            sanitizeElapsed(
                realSecondsElapsed
            );

        const double gameHours =
            sanitizeElapsed(
                gameHoursElapsed
            );

        const double worldSeconds =
            gameHours *
            secondsPerHour;

        const bool timeScaleValid =
            std::isfinite(timeScale) &&
            std::abs(timeScale) >
                timeScaleEpsilon;

        double normalGameAdvance = 0.0;

        if (
            worldSeconds > 0.0 &&
            timeScaleValid)
        {
            normalGameAdvance =
                worldSeconds /
                std::abs(timeScale);
        }

        bool clockAdvanced = false;
        bool hybridDiscontinuity = false;

        double advance = 0.0;

        {
            std::scoped_lock lock(mutex);

            switch (mode)
            {
                case playbackClockMode::realtime:
                {
                    advance =
                        realAdvance;

                    break;
                }

                case playbackClockMode::game:
                {
                    /*
                    * Game mode follows ordinary Calendar
                    * progression, normalized by timescale.
                    *
                    * Explicit Calendar discontinuities such
                    * as wait/sleep/fast travel are ignored.
                    */
                    if (!discontinuity)
                    {
                        advance =
                            normalGameAdvance;
                    }

                    break;
                }

                case playbackClockMode::hybrid:
                {
                    if (
                        discontinuity &&
                        worldSeconds > 0.0)
                    {
                        /*
                        * Hybrid mode considers discontinuous
                        * world-time advancement to have
                        * elapsed for playback.
                        */
                        advance =
                            worldSeconds;

                        hybridDiscontinuity =
                            true;

                        ++discontinuityCount;
                    }
                    else
                    {
                        advance =
                            normalGameAdvance;
                    }

                    break;
                }
            }

            if (
                std::isfinite(advance) &&
                advance > 0.0)
            {
                clockTime +=
                    advance;

                ++updateCount;

                clockAdvanced =
                    true;
            }
        }

        if (hybridDiscontinuity)
        {
            REX::DEBUG(
                "Playback clock accepted Calendar "
                "discontinuity of {:.3f} game hours "
                "({:.3f}s world time, "
                "timescale {:.3f}).",
                gameHours,
                worldSeconds,
                timeScale
            );
        }

        if (clockAdvanced)
        {
            condition.notify_all();
        }
    }


    double playbackClock::now() const
    {
        std::scoped_lock lock(mutex);

        return clockTime;
    }


    bool playbackClock::waitUntil(
        double targetTime,
        const std::atomic<bool>& stopRequested)
    {
        if (!std::isfinite(targetTime))
        {
            return false;
        }

        std::unique_lock lock(mutex);

        condition.wait(
            lock,
            [&]()
            {
                return
                    stopRequested.load(
                        std::memory_order_acquire
                    ) ||
                    clockTime >= targetTime;
            }
        );

        return
            !stopRequested.load(
                std::memory_order_acquire
            );
    }


    void playbackClock::notifyWaiters()
    {
        condition.notify_all();
    }
}
