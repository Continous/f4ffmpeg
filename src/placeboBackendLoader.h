#pragma once

#include <cstdint>

struct AVFrame;

namespace f4ffmpeg
{
    struct producedFrame;

    bool tryPlaceboBackendConvert(
        const AVFrame& frame,
        std::uint32_t quality,
        producedFrame& output
    );

    bool placeboBackendAvailable();
}
