#pragma once

struct AVFrame;

namespace f4ffmpeg
{
    struct producedFrame;

    // Generic decoded-frame -> canonical presentation-frame boundary.
    //
    // Every decoder backend passes its AVFrame here. The implementation handles
    // hardware-frame normalization, colorspace conversion, HDR -> SDR mapping,
    // chroma reconstruction, dithering/scaling, and creation of the canonical
    // Fallout-device RGBA8 D3D11 texture/SRV.
    bool convertFrameForPresentation(
        const AVFrame& frame,
        producedFrame& output
    );
}
