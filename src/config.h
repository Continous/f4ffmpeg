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

    // Decoded-frame presentation conversion policy. libplacebo is the primary
    // converter for every decoder backend; libswscale is retained only as a
    // compatibility fallback when a source format cannot be mapped/rendered.
    // cheapest: libplacebo fast preset (minimal processing / bilinear-style path).
    // balanced: libplacebo default/recommended renderer preset.
    // quality: libplacebo high-quality preset (HQ scaling, debanding/color mapping).
    inline REX::TTomlSetting<std::string> conversionQuality{
        "Playback",
        "ConversionQuality",
        "balanced"
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
