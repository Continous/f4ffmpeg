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
};
