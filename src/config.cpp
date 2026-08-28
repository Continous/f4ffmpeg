// config.cpp
#include "config.h"

#include <spdlog/spdlog.h>
#include <filesystem>

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
        constexpr auto basePath =
            "Data/F4SE/Plugins/f4ffmpeg.toml";

        constexpr auto userPath =
            "Data/F4SE/Plugins/f4ffmpeg.user.toml";

        auto* settings =
            REX::FTomlSettingStore::GetSingleton();

        settings->Init(
            basePath,
            userPath
        );

        if (!std::filesystem::exists(basePath))
        {
            settings->Save();
        }

        settings->Load();

        applyLogLevel();

        spdlog::debug(
            "Playback.ConversionQuality={}",
            conversionQuality.GetValue()
        );

        spdlog::debug(
            "Textures.DisableWorkshopTVRasterScan={}, "
            "Textures.DisableWorkshopTVStatic={}, "
            "Textures.DisableWorkshopTVWarp={}",
            disableWorkshopTVRasterScan.GetValue(),
            disableWorkshopTVStatic.GetValue(),
            disableWorkshopTVWarp.GetValue()
        );
    }
}
