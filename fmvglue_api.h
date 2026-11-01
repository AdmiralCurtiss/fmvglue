#ifndef FMVGLUE_API_H
#define FMVGLUE_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifdef BUILDING_FMVGLUE
#define FMVGLUE_EXPORT_PRE
#define FMVGLUE_EXPORT_MID __declspec(dllexport)
#define FMVGLUE_EXPORT_POST
#else
#ifdef LINKING_FMVGLUE_DLL
#define FMVGLUE_EXPORT_PRE
#define FMVGLUE_EXPORT_MID __declspec(dllimport)
#define FMVGLUE_EXPORT_POST
#else
#define FMVGLUE_EXPORT_PRE typedef
#define FMVGLUE_EXPORT_MID (*
#define FMVGLUE_EXPORT_POST )
#endif
#endif

typedef enum fmvglue_image_format_e {
    FMVGLUE_IMAGE_FORMAT_UNKNOWN = 0,
    FMVGLUE_IMAGE_FORMAT_RGB24 = 1,
    FMVGLUE_IMAGE_FORMAT_BGR24 = 2,
    FMVGLUE_IMAGE_FORMAT_ARGB32 = 3,
    FMVGLUE_IMAGE_FORMAT_RGBA32 = 4,
    FMVGLUE_IMAGE_FORMAT_ABGR32 = 5,
    FMVGLUE_IMAGE_FORMAT_BGRA32 = 6,
    FMVGLUE_IMAGE_FORMAT_YUV410P = 7,
    FMVGLUE_IMAGE_FORMAT_YUV411P = 8,
    FMVGLUE_IMAGE_FORMAT_YUV420P = 9,
    FMVGLUE_IMAGE_FORMAT_YUV422P = 10,
    FMVGLUE_IMAGE_FORMAT_YUV444P = 11,
    FMVGLUE_IMAGE_FORMAT_YUYV422 = 12,
    FMVGLUE_IMAGE_FORMAT_MAX = 0x7fffffff, /* force 32 bits */
} fmvglue_image_format_t;

typedef struct fmvglue_context_s {
    void* Internal;
} fmvglue_context_t;

typedef struct fmvglue_video_info_s {
    unsigned int FramerateNumerator;
    unsigned int FramerateDenominator;
    unsigned int Width;
    unsigned int Height;
    fmvglue_image_format_t ImageFormat;
} fmvglue_video_info_t;

typedef struct fmvglue_video_frame_s {
    uint8_t* Data;
    uint64_t TimestampNanoseconds;
    unsigned int DataLength;
    unsigned int FrameWidth;
    unsigned int FrameHeight;
    fmvglue_image_format_t FrameImageFormat;
} fmvglue_video_frame_t;

typedef void (*fmvglue_video_frame_callback)(fmvglue_context_t* context,
                                             fmvglue_video_frame_t* frame,
                                             void* userdata);

#define FMVGLUE_ERROR_OK (0)
#define FMVGLUE_ERROR_DONE (1)
#define FMVGLUE_ERROR_GENERIC (-1)

// unless otherwise noted, all of these return 0 on success and some negative value on failure

// create a new context
FMVGLUE_EXPORT_PRE int FMVGLUE_EXPORT_MID fmvglue_create_context
    FMVGLUE_EXPORT_POST(fmvglue_context_t* context);

// destroy a previously created context, releasing all resources
FMVGLUE_EXPORT_PRE int FMVGLUE_EXPORT_MID fmvglue_destroy_context
    FMVGLUE_EXPORT_POST(fmvglue_context_t* context);

// open a video file and set up matching decoding structures
FMVGLUE_EXPORT_PRE int FMVGLUE_EXPORT_MID fmvglue_open_file
    FMVGLUE_EXPORT_POST(fmvglue_context_t* context, const char* filename);

// get info about the current video. decoding must have been started!
// to start decoding without outputting a video frame yet, call fmvglue_update(context, -1)
FMVGLUE_EXPORT_PRE int FMVGLUE_EXPORT_MID fmvglue_get_video_info
    FMVGLUE_EXPORT_POST(fmvglue_context_t* context, fmvglue_video_info_t* info);

// set the output image format for video frames. the image will be automatically converted to this
// format if possible before being passed to the callback function.
FMVGLUE_EXPORT_PRE int FMVGLUE_EXPORT_MID fmvglue_set_video_output_format
    FMVGLUE_EXPORT_POST(fmvglue_context_t* context, fmvglue_image_format_t imageFormat);

// set the callback function that will receive the video frames
FMVGLUE_EXPORT_PRE int FMVGLUE_EXPORT_MID fmvglue_set_video_frame_callback
    FMVGLUE_EXPORT_POST(fmvglue_context_t* context,
                        fmvglue_video_frame_callback callback,
                        void* userdata);

// advance the video decoding by the given time.
// may return FMVGLUE_ERROR_DONE (1) if we have advanced past the end of the video.
FMVGLUE_EXPORT_PRE int FMVGLUE_EXPORT_MID fmvglue_update
    FMVGLUE_EXPORT_POST(fmvglue_context_t* context, int64_t nanoseconds);

#undef FMVGLUE_EXPORT_PRE
#undef FMVGLUE_EXPORT_MID
#undef FMVGLUE_EXPORT_POST

#ifdef __cplusplus
}
#endif

#endif
