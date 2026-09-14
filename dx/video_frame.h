// Decoder-independent, limited-range BT.601 YUV420P row conversion.
#pragma once
#include <stdint.h>

enum VideoSurfaceType : uint32_t {
    VIDEO_XRGB8888 = 3,
    VIDEO_RGB555 = 9,
    VIDEO_RGB565 = 10,
};

// Each chroma sample covers two horizontal luma samples. The caller selects
// the chroma row for y / 2 and supplies a destination of width * bpp bytes.
void video_frame_convert_row(uint8_t *dest, const uint8_t *y, const uint8_t *u, const uint8_t *v,
                             uint32_t width, VideoSurfaceType type);
