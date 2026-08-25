#pragma once

#include <string>
#include <vector>
#include <cstddef>

namespace f4ffmpeg
{
    struct hardwareCodec
    {
        std::string backend;
        std::string codec;
    };

    struct hardwareTestResults
    {
        bool hardwareDecodeAvailable = false;
        std::vector<hardwareCodec> codecs;
    };

    struct reportedCodec
    {
        std::string backend;
        std::string codec;
    };

    struct reportedCodecsResults
    {
        bool hardwareDecodeAdvertised = false;
        std::vector<reportedCodec> reportedCodecs;
    };

    reportedCodecsResults testHardwareDevices();

    hardwareTestResults testHardwareCodecs(
        const reportedCodecsResults& reportedResults
    );

    std::size_t getSupportedCodecCount();

    const hardwareCodec* getSupportedCodec(
    std::size_t index
    );
}
