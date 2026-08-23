// config.cpp
#include "config.h"

#include <spdlog/spdlog.h>
#include <FTomlSettingStore.h>

namespace f4ffmpeg::config
{
    static void applyLogLevel()
    {
        const auto level = logLevel.GetValue();

        if (level == "trace") {
            spdlog::default_logger()->set_level(spdlog::level::trace);
        } else if (level == "debug") {
            spdlog::default_logger()->set_level(spdlog::level::debug);
        } else if (level == "warning" || level == "warn") {
            spdlog::default_logger()->set_level(spdlog::level::warn);
        } else if (level == "error") {
            spdlog::default_logger()->set_level(spdlog::level::err);
        } else if (level == "critical") {
            spdlog::default_logger()->set_level(spdlog::level::critical);
        } else {
            spdlog::default_logger()->set_level(spdlog::level::info);
        }
    }

    void initialize()
    {
        auto* settings = REX::FTomlSettingStore::GetSingleton();

        settings->Init(
            "Data/F4SE/Plugins/f4ffmpeg.toml",
            "Data/F4SE/Plugins/f4ffmpeg.user.toml"
        );

        settings->Load();
        applyLogLevel();
    }
}
