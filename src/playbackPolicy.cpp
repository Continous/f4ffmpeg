#include "playbackPolicy.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace f4ffmpeg
{
namespace
{
    std::string normalizedMethodName(
        std::string_view value)
    {
        std::string result;
        result.reserve(value.size());

        for (const unsigned char character : value)
        {
            if (
                character == ' ' ||
                character == '\t' ||
                character == '-' ||
                character == '_')
            {
                continue;
            }

            result.push_back(
                static_cast<char>(
                    std::tolower(character)
                )
            );
        }

        return result;
    }
}

std::optional<transitionMethod>
parseTransitionMethod(
    std::string_view value,
    bool allowImage)
{
    const auto normalized =
        normalizedMethodName(value);

    if (
        normalized == "vanilladds" ||
        normalized == "vanilla" ||
        normalized == "dds")
    {
        return transitionMethod::vanillaDDS;
    }

    if (
        normalized == "holdlastframe" ||
        normalized == "hold" ||
        normalized == "lastframe")
    {
        return transitionMethod::holdLastFrame;
    }

    if (
        normalized == "blackframe" ||
        normalized == "black" ||
        normalized == "null" ||
        normalized == "nullframe")
    {
        return transitionMethod::blackFrame;
    }

    if (
        allowImage &&
        (normalized == "image" ||
         normalized == "playlistimage"))
    {
        return transitionMethod::image;
    }

    return std::nullopt;
}

const char* transitionMethodName(
    transitionMethod method) noexcept
{
    switch (method)
    {
        case transitionMethod::vanillaDDS:
            return "VanillaDDS";

        case transitionMethod::holdLastFrame:
            return "HoldLastFrame";

        case transitionMethod::blackFrame:
            return "BlackFrame";

        case transitionMethod::image:
            return "Image";
    }

    return "VanillaDDS";
}
}
