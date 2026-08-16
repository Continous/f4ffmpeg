#include "api.h"

namespace f4ffmpeg
{
    class apiImpl : public api
    {
    public:
        std::uint32_t getVersion() const override
        {
            return apiVersion;
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

        void testHardwareDevices(
            decoder* decoderInstance
        ) override
        {
            if (decoderInstance != nullptr)
            {
                decoderInstance->testHardwareDevices();
            }
        }

        bool hasHardwareDecoder(
            decoder* decoderInstance
        ) const override
        {
            if (decoderInstance == nullptr)
            {
                return false;
            }

            return decoderInstance->hasHardwareDecoder();
        }

        const std::vector<hardwareCodec>& getPreferredCodecs(
            decoder* decoderInstance
        ) const override
        {
            return decoderInstance->getHardwareCodecs();
        }
    };

    api* getApi()
    {
        static apiImpl instance;
        return &instance;
    }
}
