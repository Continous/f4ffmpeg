#pragma once

#include <cstdint>
#include "decoder.h"

namespace f4ffmpeg
{
    inline constexpr std::uint32_t apiVersion = 1;

    class api
    {
    public:
        virtual ~api() = default;

        virtual std::uint32_t getVersion() const = 0;

        virtual decoder* createDecoder() = 0;
        virtual void destroyDecoder(decoder* decoderInstance) = 0;
    };

    api* getApi();
}
