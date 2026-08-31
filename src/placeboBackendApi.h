#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AVFrame;

#define F4FFMPEG_PLACEBO_BACKEND_ABI 1u

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

typedef uint32_t (F4FFMPEG_PLACEBO_CALL *f4ffmpeg_placebo_abi_fn)(void);
typedef int (F4FFMPEG_PLACEBO_CALL *f4ffmpeg_placebo_convert_fn)(
    void* d3d11_device,
    const struct AVFrame* frame,
    uint32_t quality,
    f4ffmpeg_placebo_output* output
);

#ifdef __cplusplus
}
#endif
