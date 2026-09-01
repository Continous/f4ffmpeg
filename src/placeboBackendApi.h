#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AVFrame;

// ABI 2 changes the conversion contract: core passes the original decoded
// AVFrame to the companion before any hardware -> software transfer. The
// companion may consume AV_PIX_FMT_VULKAN directly and must return a graceful
// fallback result for unsupported/non-Vulkan frames.
#define F4FFMPEG_PLACEBO_BACKEND_ABI 2u

#if defined(_WIN32)
#define F4FFMPEG_PLACEBO_CALL __cdecl
#else
#define F4FFMPEG_PLACEBO_CALL
#endif

typedef struct f4ffmpeg_placebo_output
{
    void* texture;
    void* resource_view;
    uint32_t width;
    uint32_t height;
} f4ffmpeg_placebo_output;

typedef enum f4ffmpeg_placebo_result
{
    // Conversion completed and output owns one texture + SRV reference.
    F4FFMPEG_PLACEBO_SUCCESS = 1,

    // This individual frame is not suitable for the Vulkan/libplacebo path.
    // Core should immediately use its normal libswscale fallback.
    F4FFMPEG_PLACEBO_FALLBACK = 0,

    // The companion itself cannot establish a usable Vulkan/libplacebo
    // backend. Core may stop trying this DLL for the rest of the process.
    F4FFMPEG_PLACEBO_UNAVAILABLE = -1,

    // The current FFmpeg Vulkan device could not be imported. Other decoder
    // devices may still work, so core should fall back for this path but keep
    // the DLL available.
    F4FFMPEG_PLACEBO_DEVICE_UNAVAILABLE = -2
} f4ffmpeg_placebo_result;

typedef uint32_t (F4FFMPEG_PLACEBO_CALL *f4ffmpeg_placebo_abi_fn)(void);
typedef int32_t (F4FFMPEG_PLACEBO_CALL *f4ffmpeg_placebo_convert_fn)(
    void* d3d11_device,
    const struct AVFrame* frame,
    uint32_t quality,
    f4ffmpeg_placebo_output* output
);

#ifdef __cplusplus
}
#endif
