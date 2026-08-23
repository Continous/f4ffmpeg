// config.h
#pragma once

#include <string>
#include <REX/TTomlSetting.h>

namespace f4ffmpeg::config
{
    inline REX::TTomlSetting<std::string> logLevel{
        "Logging",
        "Level",
        "info"
    };

    void initialize();
}
