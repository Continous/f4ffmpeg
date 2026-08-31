#pragma once

struct AVFrame;

namespace f4ffmpeg
{
    struct producedFrame;

    // Generic decoded-frame -> canonical presentation-frame boundary.
    // Every decoder backend passes its AVFrame here. The implementation
    // normalizes hardware frames when required, opportunistically invokes the
    // separately loaded libplacebo companion, and falls back to metadata-aware
    // libswscale before creating Fallout-device RGBA8 D3D11 texture/SRV output.
    bool convertFrameForPresentation(
        const AVFrame& frame,
        producedFrame& output
    );
}
