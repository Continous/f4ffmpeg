#pragma once

#include <string>
#include <vector>
#include <REX/TTomlSetting.h>

namespace f4ffmpeg::config
{
    inline REX::TTomlSetting<std::string> logLevel{
        "Logging",
        "Level",
        "info"
    };
    inline REX::TTomlSetting<std::string> clockMode{
        "Playback",
        "Clock",
        "hybrid"
    };

    inline REX::TTomlSetting<double> maxFrameLag{
        "Playback",
        "MaxFrameLag",
        0.250
    };

    // Decoded-frame presentation conversion policy. The optional libplacebo
    // companion is tried first when installed; metadata-aware libswscale remains
    // the always-available compatibility fallback in the core plugin.
    // cheapest/balanced/quality select the corresponding conversion profile.
    inline REX::TTomlSetting<std::string> conversionQuality{
        "Playback",
        "ConversionQuality",
        "balanced"
    };

    // Decoder-gap fallback used when no permitted playlist override applies.
    // Supported global values: VanillaDDS, HoldLastFrame, BlackFrame.
    // Image is deliberately playlist-only.
    inline REX::TTomlSetting<std::string> fallbackTransitionMethod{
        "Transitions",
        "FallbackTransitionMethod",
        "VanillaDDS"
    };

    // If false, playlist-side transition Method/Image metadata is ignored.
    inline REX::TTomlSetting<bool> usePlaylistTransitionMethod{
        "Transitions",
        "UsePlaylistTransitionMethod",
        true
    };

    // Workshop-TV presentation effects. nifHandler scopes these to scene
    // instances whose screen texture is actually handled by f4ffmpeg; vanilla
    // TVs that are not video targets are left untouched.
    inline REX::TTomlSetting<bool> disableWorkshopTVRasterScan{
        "Textures",
        "DisableWorkshopTVRasterScan",
        false
    };

    inline REX::TTomlSetting<bool> disableWorkshopTVStatic{
        "Textures",
        "DisableWorkshopTVStatic",
        false
    };

    inline REX::TTomlSetting<bool> disableWorkshopTVWarp{
        "Textures",
        "DisableWorkshopTVWarp",
        false
    };

    // Optional Nuka-World commercial-screen spawning for configured machine
    // base-form editor IDs. This is intentionally opt-in because the source
    // form is only compatible with selected machine models.
    inline REX::TTomlSetting<bool> enableNukaColaMachineScreens{
        "Extra",
        "EnableNukaColaMachineScreens",
        false
    };

    inline REX::TTomlSetting<std::vector<std::string>> nukaColaMachineScreenTargets{
        "Extra",
        "NukaColaMachineScreenTargets",
        {}
    };

    // The DLC04 base form spawned beside each configured machine.
    inline REX::TTomlSetting<std::string> nukaColaMachineScreenSourceForm{
        "Extra",
        "NukaColaMachineScreenSourceForm",
        "NukaColaMachineCommercialFxDLC04"
    };

    inline REX::TTomlSetting<std::string> debugDecodeKey{
        "Debug",
        "DecodeKey",
        "F10"
    };

    inline REX::TTomlSetting<std::string> debugFrameDumpKey{
        "Debug",
        "FrameDumpKey",
        "F11"
    };

    inline REX::TTomlSetting<std::string> debugFrameDumpPath{
        "Debug",
        "FrameDumpPath",
        "Data/f4ffmpeg/f4ffmpeg_framedump.tga"
    };

    void initialize();
}
