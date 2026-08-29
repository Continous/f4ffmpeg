#pragma once

struct AVFrame;

namespace f4ffmpeg
{
    struct producedFrame;

    // Generic decoded-frame -> canonical presentation-frame boundary.
    //
    // Every decoder backend passes its AVFrame here. The implementation
    // normalizes hardware frames to system memory when required, applies
    // metadata-aware libswscale conversion, and creates the canonical
    // Fallout-device RGBA8 D3D11 texture/SRV.
    //
    // HDR tone/gamut mapping is intentionally not part of the current
    // conversion contract. HDR inputs remain playable through compatibility
    // conversion, but their appearance is not guaranteed to be color-correct.
    bool convertFrameForPresentation(
        const AVFrame& frame,
        producedFrame& output
    );
}
