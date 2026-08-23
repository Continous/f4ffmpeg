// config.h
#pragma once

#include <string>
#include <REX/TTomlSetting.h>

namespace f4ffmpeg::config
{
    inline REX::FTomlSetting<std::string> logLevel{
        "Logging",
        "Level",
        "info"
    };

    void initialize();
}
