#include "decoder.h"

namespace f4ffmpeg
{

        decoder::~decoder()
        {
            close();
        }
        bool decoder::open(const char* path)
    {
        if (formatContext != nullptr)
        {
            close();
        } //If there is not formatContext provided we can just close out. There's presumably no media being provided.

        REX::INFO("Attempting to open media: {}", path);

        if (avformat_open_input(
                &formatContext,
                path,
                nullptr,
                nullptr) < 0)
        {
            formatContext = nullptr; //Release formatContext, we failed to open.

            REX::ERROR("FFmpeg failed to open media: {}", path);

            return false;
        }

        if (avformat_find_stream_info(
                formatContext,
                nullptr) < 0)
        {
            REX::ERROR("FFmpeg failed to read stream information: {}", path);

            close();

            return false;
        }

        REX::INFO(
            "FFmpeg opened media successfully. Stream count: {}",
            formatContext->nb_streams
        );

        return true;
    }

        void decoder::close()
    {
        if (formatContext != nullptr)
        {
            avformat_close_input(&formatContext);
        }
    }
}
