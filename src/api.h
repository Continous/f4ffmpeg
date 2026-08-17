#pragma once

#include <cstdint>
#include <cstddef>

#include "decoder.h"
#include "tests.h"

namespace f4ffmpeg
{

    virtual std::uint32_t getVersion() const = 0;

    class api
    {
    public:

        virtual std::size_t supportedCodecCount() const = 0;

        virtual const char* supportedCodecName(
            std::size_t index
        ) const = 0;

        virtual const char* supportedCodecBackend(
            std::size_t index
        ) const = 0;

        inline constexpr std::uint32_t apiVersion = 1;


        virtual ~api() = default;

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
