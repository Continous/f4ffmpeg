#include "api.h"
#include "pch.h"

namespace f4ffmpeg
{
    class apiImpl : public api
    {
    public:
        std::uint32_t getVersion() const override
        {
            REX::INFO("API version reported as {}", apiVersion);
            return apiVersion;
        }

        std::size_t supportedCodecCount() const override
        {
            const auto count = getSupportedCodecCount();

            REX::INFO(
                "[API] reports {} supported hardware codecs",
                count
            );

            return count;
        }

        const char* supportedCodecName(
            std::size_t index) const override
        {
            const auto* codec = getSupportedCodec(index);

            if (codec == nullptr)
            {
                return nullptr;
            }

            return codec->codec.c_str();
        }


        // Here out of convenience and for information, not a guarantee that the API will use a specific backend. Fallback events can happen for unknowable reasons.
        const char* supportedCodecBackend(
            std::size_t index) const override
        {
            const auto* codec = getSupportedCodec(index);

            if (codec == nullptr)
            {
                return nullptr;
            }

            return codec->backend.c_str();
        }

        decoder* createDecoder() override
        {
            return new decoder();
        }

        void destroyDecoder(decoder* decoderInstance) override
        {
            delete decoderInstance;
        }

        bool open(
            decoder* decoderInstance,
            const char* path
        ) override
        {
            if (decoderInstance == nullptr)
            {
                return false;
            }

            return decoderInstance->open(path);
        }

        void close(
            decoder* decoderInstance
        ) override
        {
            if (decoderInstance != nullptr)
            {
                decoderInstance->close();
            }
        }
    };

    api* getApi()
    {
        static apiImpl instance;
        return &instance;
    }
}
