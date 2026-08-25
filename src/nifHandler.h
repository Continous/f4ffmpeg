#pragma once

#include <optional>
#include <string>

namespace f4ffmpeg
{
    // Converts one of f4ffmpeg's marker textures into its video path.
    //
    // Example:
    //   Data\\Textures\\f4se_rttv\\terminal_video.dds
    //       -> Data\\Video\\f4se_rttv\\terminal.mov
    //
    // Returns std::nullopt for ordinary textures.
    std::optional<std::string>
    getVideoPathForTexture(
        const char* texturePath
    );

    // Installs the BSShaderTextureSet texture-acquisition interception.
    // Safe to call more than once.
    bool initializeNifHandler();
}
