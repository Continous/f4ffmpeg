// config.cpp
#include "config.h"

#include <spdlog/spdlog.h>
#include <filesystem>
#include <toml.hpp>

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

    static void validateTomlFile(const char* path)
    {
        if (!std::filesystem::exists(path))
            return;

        if (!toml::try_parse(path).is_ok())
        {
            spdlog::error(
                "f4ffmpeg could not parse '{}'; its settings were ignored. "
                "Check TOML syntax, especially comma-separated array values.",
                path
            );
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

        validateTomlFile(basePath);
        validateTomlFile(userPath);

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
            "Transitions.FallbackTransitionMethod={}, "
            "Transitions.UsePlaylistTransitionMethod={}",
            fallbackTransitionMethod.GetValue(),
            usePlaylistTransitionMethod.GetValue()
        );

        spdlog::debug(
            "Textures.DisableWorkshopTVRasterScan={}, "
            "Textures.DisableWorkshopTVStatic={}, "
            "Textures.DisableWorkshopTVWarp={}",
            disableWorkshopTVRasterScan.GetValue(),
            disableWorkshopTVStatic.GetValue(),
            disableWorkshopTVWarp.GetValue()
        );

        spdlog::debug(
            "Extra.EnableNukaColaMachineScreens={}, Extra.NukaColaMachineScreenTargets={}, "
            "Extra.NukaColaMachineScreenSourceForm={}",
            enableNukaColaMachineScreens.GetValue(),
            nukaColaMachineScreenTargets.GetValue().size(),
            nukaColaMachineScreenSourceForm.GetValue()
        );
    }
}
