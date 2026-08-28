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

    // CPU-side YUV -> RGBA conversion policy.
    // cheapest: legacy SWS_BILINEAR path with libswscale defaults.
    // balanced: metadata-aware bicubic/full-chroma reconstruction.
    // quality: metadata-aware Lanczos/full-chroma reconstruction with
    //          error-diffusion dithering for >8-bit sources.
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
