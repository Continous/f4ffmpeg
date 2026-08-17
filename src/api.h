#pragma once

#include <cstdint>
#include <cstddef>

#include "decoder.h"
#include "tests.h"

namespace f4ffmpeg
{
    inline constexpr std::uint32_t apiVersion = 1;

    class api
    {
    public:
        virtual ~api() = default;

        virtual std::uint32_t getVersion() const = 0;

        virtual std::size_t supportedCodecCount() const = 0;

        virtual const char* supportedCodecName(
            std::size_t index
        ) const = 0;

        // Here out of convenience and for information, not a guarantee
        // that the API will use a specific backend.
        // Fallback events can happen for unknowable reasons.
        virtual const char* supportedCodecBackend(
            std::size_t index
        ) const = 0;

        virtual decoder* createDecoder() = 0;
        virtual void destroyDecoder(decoder* decoderInstance) = 0;

        virtual bool open(
            decoder* decoderInstance,
            const char* path
        ) = 0;

        virtual void close(
            decoder* decoderInstance
        ) = 0;
    };

    api* getApi();
}
