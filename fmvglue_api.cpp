#include "fmvglue_api.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/imgutils.h"
#include "libswscale/swscale.h"
}

#define STB_SPRINTF_IMPLEMENTATION
#include "stb_sprintf.h"

#include "scope.h"

namespace {
// see the diagram in fmvglue_update()
enum class UpdateStateEnum : uint8_t {
    Done = 0,
    ReadFrame,
    SendPacket,
    ReceiveFrame,
    OutputFrame,
    UnrefFrame,
    UnrefPacket,
    FlushingSendPacket,
    FlushingReceiveFrame,
    FlushingOutputFrame,
    FlushingUnrefFrame,
};

struct FmvglueContextInternal {
    AVFormatContext* FormatContext = nullptr;
    AVCodecContext* CodecContext = nullptr;
    AVFrame* Frame = nullptr;
    AVPacket* Packet = nullptr;
    unsigned int VideoStreamIndex = 0;

    enum AVPixelFormat TargetImagePixelFormat = AV_PIX_FMT_NONE;

    fmvglue_video_frame_callback VideoCallback = nullptr;
    void* VideoCallbackUserData = nullptr;

    uint64_t NanosecondsElapsed = 0;
    UpdateStateEnum UpdateState = UpdateStateEnum::Done;

    // for av_image_copy2()
    bool ImageAllocated = false;
    int ImageLength;
    int ImageWidth;
    int ImageHeight;
    enum AVPixelFormat ImagePixelFormat;
    uint8_t* ImageData[4];
    int ImageLinesizes[4];

    SwsContext* ConversionContext = nullptr;
    AVFrame* ConversionFrame = nullptr;
};
} // namespace

static bool HoldsValidFrame(UpdateStateEnum updateState) {
    return updateState == UpdateStateEnum::OutputFrame || updateState == UpdateStateEnum::UnrefFrame
           || updateState == UpdateStateEnum::FlushingOutputFrame
           || updateState == UpdateStateEnum::FlushingUnrefFrame;
}
static bool HoldsValidPacket(UpdateStateEnum updateState) {
    return updateState == UpdateStateEnum::SendPacket
           || updateState == UpdateStateEnum::ReceiveFrame
           || updateState == UpdateStateEnum::OutputFrame
           || updateState == UpdateStateEnum::UnrefFrame
           || updateState == UpdateStateEnum::UnrefPacket;
}

static fmvglue_image_format_t ConvertAVPixelFormatToImageFormatEnum(enum AVPixelFormat e) {
    switch (e) {
        case AV_PIX_FMT_RGB24: return FMVGLUE_IMAGE_FORMAT_RGB24;
        case AV_PIX_FMT_BGR24: return FMVGLUE_IMAGE_FORMAT_BGR24;
        case AV_PIX_FMT_ARGB: return FMVGLUE_IMAGE_FORMAT_ARGB32;
        case AV_PIX_FMT_RGBA: return FMVGLUE_IMAGE_FORMAT_RGBA32;
        case AV_PIX_FMT_ABGR: return FMVGLUE_IMAGE_FORMAT_ABGR32;
        case AV_PIX_FMT_BGRA: return FMVGLUE_IMAGE_FORMAT_BGRA32;
        case AV_PIX_FMT_YUV410P: return FMVGLUE_IMAGE_FORMAT_YUV410P;
        case AV_PIX_FMT_YUV411P: return FMVGLUE_IMAGE_FORMAT_YUV411P;
        case AV_PIX_FMT_YUV420P: return FMVGLUE_IMAGE_FORMAT_YUV420P;
        case AV_PIX_FMT_YUV422P: return FMVGLUE_IMAGE_FORMAT_YUV422P;
        case AV_PIX_FMT_YUV444P: return FMVGLUE_IMAGE_FORMAT_YUV444P;
        case AV_PIX_FMT_YUYV422: return FMVGLUE_IMAGE_FORMAT_YUYV422;
        default: return FMVGLUE_IMAGE_FORMAT_UNKNOWN;
    }
}

static enum AVPixelFormat ConvertImageFormatEnumToAVPixelFormat(fmvglue_image_format_t e) {
    switch (e) {
        case FMVGLUE_IMAGE_FORMAT_RGB24: return AV_PIX_FMT_RGB24;
        case FMVGLUE_IMAGE_FORMAT_BGR24: return AV_PIX_FMT_BGR24;
        case FMVGLUE_IMAGE_FORMAT_ARGB32: return AV_PIX_FMT_ARGB;
        case FMVGLUE_IMAGE_FORMAT_RGBA32: return AV_PIX_FMT_RGBA;
        case FMVGLUE_IMAGE_FORMAT_ABGR32: return AV_PIX_FMT_ABGR;
        case FMVGLUE_IMAGE_FORMAT_BGRA32: return AV_PIX_FMT_BGRA;
        case FMVGLUE_IMAGE_FORMAT_YUV410P: return AV_PIX_FMT_YUV410P;
        case FMVGLUE_IMAGE_FORMAT_YUV411P: return AV_PIX_FMT_YUV411P;
        case FMVGLUE_IMAGE_FORMAT_YUV420P: return AV_PIX_FMT_YUV420P;
        case FMVGLUE_IMAGE_FORMAT_YUV422P: return AV_PIX_FMT_YUV422P;
        case FMVGLUE_IMAGE_FORMAT_YUV444P: return AV_PIX_FMT_YUV444P;
        case FMVGLUE_IMAGE_FORMAT_YUYV422: return AV_PIX_FMT_YUYV422;
        default: return AV_PIX_FMT_NONE;
    }
}

int fmvglue_create_context(fmvglue_context_t* context) {
    if (context == nullptr) {
        return FMVGLUE_ERROR_GENERIC;
    }
    FmvglueContextInternal* internal = new (std::nothrow) FmvglueContextInternal();
    context->Internal = internal;
    if (internal == nullptr) {
        return FMVGLUE_ERROR_GENERIC;
    }
    return FMVGLUE_ERROR_OK;
}

int fmvglue_destroy_context(fmvglue_context_t* context) {
    if (context == nullptr) {
        return FMVGLUE_ERROR_GENERIC;
    }
    FmvglueContextInternal* internal = static_cast<FmvglueContextInternal*>(context->Internal);
    if (internal == nullptr) {
        return FMVGLUE_ERROR_GENERIC;
    }

    if (internal->ImageAllocated) {
        av_freep(&internal->ImageData[0]);
        internal->ImageAllocated = false;
    }
    if (internal->ConversionFrame != nullptr) {
        av_frame_free(&internal->ConversionFrame);
        internal->ConversionFrame = nullptr;
    }
    if (internal->ConversionContext != nullptr) {
        sws_free_context(&internal->ConversionContext);
        internal->ConversionContext = nullptr;
    }

    UpdateStateEnum updateState = internal->UpdateState;
    if (HoldsValidFrame(updateState)) {
        av_frame_unref(internal->Frame);
    }
    if (HoldsValidPacket(updateState)) {
        av_packet_unref(internal->Packet);
    }

    internal->UpdateState = UpdateStateEnum::Done;
    av_packet_free(&internal->Packet);
    av_frame_free(&internal->Frame);
    avcodec_free_context(&internal->CodecContext);
    avformat_close_input(&internal->FormatContext);
    delete internal;

    context->Internal = nullptr;
    return FMVGLUE_ERROR_OK;
}

int fmvglue_open_file(fmvglue_context_t* context, const char* filename) {
    if (context == nullptr) {
        return FMVGLUE_ERROR_GENERIC;
    }
    FmvglueContextInternal* internal = static_cast<FmvglueContextInternal*>(context->Internal);
    if (internal == nullptr) {
        return FMVGLUE_ERROR_GENERIC;
    }

    char buffer[2048];
    AVFormatContext* format_context = nullptr;
    stbsp_snprintf(buffer, sizeof(buffer), "file:%s", filename);

    int result = avformat_open_input(&format_context, buffer, nullptr, nullptr);
    if (result < 0) {
        av_strerror(result, buffer, sizeof(buffer));
        printf("avformat_open_input() error: %s\n", buffer);
        return FMVGLUE_ERROR_GENERIC;
    }
    auto format_context_guard =
        HyoutaUtils::MakeDisposableScopeGuard([&]() { avformat_close_input(&format_context); });

    result = avformat_find_stream_info(format_context, nullptr);
    if (result < 0) {
        av_strerror(result, buffer, sizeof(buffer));
        printf("avformat_find_stream_info() error: %s\n", buffer);
        return FMVGLUE_ERROR_GENERIC;
    }

    const AVCodec* codec = nullptr;
    result = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (result < 0) {
        av_strerror(result, buffer, sizeof(buffer));
        printf("av_find_best_stream() error: %s\n", buffer);
        return FMVGLUE_ERROR_GENERIC;
    }

    unsigned int stream_index = static_cast<unsigned int>(result);
    AVCodecContext* codec_context = avcodec_alloc_context3(codec);
    if (codec_context == nullptr) {
        printf("avcodec_alloc_context3() failed\n");
        return FMVGLUE_ERROR_GENERIC;
    }
    auto codec_context_guard =
        HyoutaUtils::MakeDisposableScopeGuard([&]() { avcodec_free_context(&codec_context); });

    result = avcodec_parameters_to_context(codec_context,
                                           format_context->streams[stream_index]->codecpar);
    if (result < 0) {
        av_strerror(result, buffer, sizeof(buffer));
        printf("avcodec_parameters_to_context() error: %s\n", buffer);
        return FMVGLUE_ERROR_GENERIC;
    }

    result = avcodec_open2(codec_context, codec, nullptr);
    if (result < 0) {
        av_strerror(result, buffer, sizeof(buffer));
        printf("avcodec_open2() error: %s\n", buffer);
        return FMVGLUE_ERROR_GENERIC;
    }

    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        printf("av_frame_alloc() failed\n");
        return FMVGLUE_ERROR_GENERIC;
    }
    auto frame_guard = HyoutaUtils::MakeDisposableScopeGuard([&]() { av_frame_free(&frame); });

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        printf("av_packet_alloc() failed\n");
        return FMVGLUE_ERROR_GENERIC;
    }
    auto packet_guard = HyoutaUtils::MakeDisposableScopeGuard([&]() { av_packet_free(&packet); });

    internal->FormatContext = format_context;
    internal->CodecContext = codec_context;
    internal->Frame = frame;
    internal->Packet = packet;
    internal->VideoStreamIndex = stream_index;
    internal->UpdateState = UpdateStateEnum::ReadFrame;
    format_context_guard.Dispose();
    codec_context_guard.Dispose();
    frame_guard.Dispose();
    packet_guard.Dispose();
    return FMVGLUE_ERROR_OK;
}

int fmvglue_get_video_info(fmvglue_context_t* context, fmvglue_video_info_t* info) {
    if (context == nullptr) {
        return FMVGLUE_ERROR_GENERIC;
    }
    FmvglueContextInternal* internal = static_cast<FmvglueContextInternal*>(context->Internal);
    if (internal == nullptr) {
        return FMVGLUE_ERROR_GENERIC;
    }
    if (!HoldsValidFrame(internal->UpdateState)) {
        return FMVGLUE_ERROR_GENERIC;
    }

    AVStream* stream = internal->FormatContext->streams[internal->VideoStreamIndex];
    AVFrame* frame = internal->Frame;
    info->FramerateNumerator = stream->avg_frame_rate.num;
    info->FramerateDenominator = stream->avg_frame_rate.den;
    info->Width = frame->width;
    info->Height = frame->height;
    info->ImageFormat =
        ConvertAVPixelFormatToImageFormatEnum(static_cast<AVPixelFormat>(frame->format));
    return FMVGLUE_ERROR_OK;
}

int fmvglue_set_video_output_format(fmvglue_context_t* context,
                                    fmvglue_image_format_t imageFormat) {
    if (context == nullptr) {
        return FMVGLUE_ERROR_GENERIC;
    }
    FmvglueContextInternal* internal = static_cast<FmvglueContextInternal*>(context->Internal);
    if (internal == nullptr) {
        return FMVGLUE_ERROR_GENERIC;
    }

    if (imageFormat == FMVGLUE_IMAGE_FORMAT_UNKNOWN) {
        // turn off the output format conversion, just pass the image as-is
        internal->TargetImagePixelFormat = AV_PIX_FMT_NONE;
        return FMVGLUE_ERROR_OK;
    }

    enum AVPixelFormat ffmpegFormat = ConvertImageFormatEnumToAVPixelFormat(imageFormat);
    if (ffmpegFormat == AV_PIX_FMT_NONE) {
        return FMVGLUE_ERROR_GENERIC;
    }

    internal->TargetImagePixelFormat = ffmpegFormat;
    return FMVGLUE_ERROR_OK;
}

int fmvglue_set_video_frame_callback(fmvglue_context_t* context,
                                     fmvglue_video_frame_callback callback,
                                     void* userdata) {
    if (context == nullptr) {
        return FMVGLUE_ERROR_GENERIC;
    }
    FmvglueContextInternal* internal = static_cast<FmvglueContextInternal*>(context->Internal);
    if (internal == nullptr) {
        return FMVGLUE_ERROR_GENERIC;
    }

    internal->VideoCallback = callback;
    internal->VideoCallbackUserData = userdata;
    return FMVGLUE_ERROR_OK;
}

static void UpdateVideoTime(fmvglue_context_t* context, uint64_t nanoseconds) {
    FmvglueContextInternal* internal = static_cast<FmvglueContextInternal*>(context->Internal);
    uint64_t elapsed = internal->NanosecondsElapsed;
    if (elapsed > (std::numeric_limits<uint64_t>::max() - nanoseconds)) {
        elapsed = std::numeric_limits<uint64_t>::max();
    } else {
        elapsed += nanoseconds;
    }
    internal->NanosecondsElapsed = elapsed;
}

// returns 1 if we should continue to the next state
// returns 0 if we should stay where we are
// returns -1 on error
static int OutputVideoFrame(fmvglue_context_t* context) {
    int result;
    char buffer[256];

    // frame->pts holds info when to show frame
    FmvglueContextInternal* internal = static_cast<FmvglueContextInternal*>(context->Internal);
    AVFrame* frame = internal->Frame;
    assert(frame != nullptr);
    AVStream* stream = internal->FormatContext->streams[internal->VideoStreamIndex];
    assert(stream != nullptr);

    const uint64_t currentTimeInNanoseconds = internal->NanosecondsElapsed;
    const int64_t frameTimestamp = frame->pts;
    const int frameTimeBaseNumerator = stream->time_base.num;
    const int frameTimeBaseDenominator = stream->time_base.den;
    if (frameTimestamp <= 0 || frameTimeBaseNumerator <= 0 || frameTimeBaseDenominator <= 0) {
        // broken frame timestamp. skip the frame and hope the next one's better.
        return 1;
    }

    const uint64_t ftsUnsigned = static_cast<uint64_t>(frameTimestamp);
    const unsigned int numUnsigned = static_cast<unsigned int>(frameTimeBaseNumerator);
    const unsigned int denUnsigned = static_cast<unsigned int>(frameTimeBaseDenominator);
    const uint64_t ftsMultiplied = ((ftsUnsigned * numUnsigned) * 1000000000u);
    const uint64_t ftsNanoseconds = (ftsMultiplied / denUnsigned);
    const uint64_t ftsNanosecondsRest = (ftsMultiplied % denUnsigned);
    if ((ftsNanoseconds > currentTimeInNanoseconds)
        || (ftsNanoseconds == currentTimeInNanoseconds && ftsNanosecondsRest > 0)) {
        // frame is after current time, stay on this frame for now
        return 0;
    }

    enum AVPixelFormat copySourceFormat = ((enum AVPixelFormat)frame->format);
    AVFrame* copySourceFrame = frame;
    enum AVPixelFormat convertFormat = internal->TargetImagePixelFormat;
    if (convertFormat != AV_PIX_FMT_NONE && convertFormat != copySourceFormat) {
        SwsContext* convertContext = internal->ConversionContext;
        if (convertContext == nullptr) {
            convertContext = sws_alloc_context();
            if (convertContext == nullptr) {
                return -1;
            }
            internal->ConversionContext = convertContext;
        }

        AVFrame* convertFrame = internal->ConversionFrame;
        if (convertFrame != nullptr
            && (convertFrame->format != convertFormat || convertFrame->width != frame->width
                || convertFrame->height != frame->height)) {
            // doesn't match, deallocate and allocate a new one
            av_frame_free(&convertFrame);
            convertFrame = nullptr;
            internal->ConversionFrame = nullptr;
        }

        if (convertFrame == nullptr) {
            convertFrame = av_frame_alloc();
            if (convertFrame == nullptr) {
                return -1;
            }
            convertFrame->width = frame->width;
            convertFrame->height = frame->height;
            convertFrame->format = convertFormat;
            internal->ConversionFrame = convertFrame;
        }

        result = sws_scale_frame(convertContext, convertFrame, frame);
        if (result < 0) {
            av_strerror(result, buffer, sizeof(buffer));
            printf("sws_scale_frame() error: %s\n", buffer);
            return -1;
        }

        copySourceFrame = convertFrame;
        copySourceFormat = convertFormat;
    }

    if (!internal->ImageAllocated || internal->ImageWidth != copySourceFrame->width
        || internal->ImageHeight != copySourceFrame->height
        || internal->ImagePixelFormat != copySourceFormat) {
        if (internal->ImageAllocated) {
            av_freep(&internal->ImageData[0]);
            internal->ImageAllocated = false;
        }
        result = av_image_alloc(internal->ImageData,
                                internal->ImageLinesizes,
                                copySourceFrame->width,
                                copySourceFrame->height,
                                copySourceFormat,
                                1);
        if (result < 0) {
            av_strerror(result, buffer, sizeof(buffer));
            printf("av_image_alloc() error: %s\n", buffer);
            return -1;
        }
        internal->ImageLength = result;
        internal->ImageWidth = copySourceFrame->width;
        internal->ImageHeight = copySourceFrame->height;
        internal->ImagePixelFormat = copySourceFormat;
        internal->ImageAllocated = true;
    }

    av_image_copy2(internal->ImageData,
                   internal->ImageLinesizes,
                   copySourceFrame->data,
                   copySourceFrame->linesize,
                   copySourceFormat,
                   copySourceFrame->width,
                   copySourceFrame->height);

    if (internal->VideoCallback) {
        fmvglue_video_frame_t f;
        f.Data = internal->ImageData[0];
        f.TimestampNanoseconds = ftsNanoseconds;
        f.DataLength = internal->ImageLength;
        f.FrameWidth = internal->ImageWidth;
        f.FrameHeight = internal->ImageHeight;
        f.FrameImageFormat = ConvertAVPixelFormatToImageFormatEnum(internal->ImagePixelFormat);
        internal->VideoCallback(context, &f, internal->VideoCallbackUserData);
    }

    return 1;
}

int fmvglue_update(fmvglue_context_t* context, int64_t nanoseconds) {
    // state machine goes like this:
    //
    //  +-------- av_read_frame() <------------+
    //  |               v                      |
    //  |         avcodec_send_packet()        |
    //  |               v                      |
    //  |     +-- avcodec_receive_frame() <-+  |
    // EOF    |         v                   |  |
    //  |     |   VideoCallback()           |  |
    //  |  EAGAIN       v                   |  |
    //  |     |   av_frame_unref() ---------+  |
    //  |     |                                |
    //  |     +-> av_packet_unref() -----------+
    //  |
    //  +-------> avcodec_send_packet(NULL)
    //                  v
    //        +-- avcodec_receive_frame() <-+
    //        |         v                   |
    //        |   VideoCallback()           |
    //       EOF        v                   |
    //        |   av_frame_unref() ---------+
    //        |
    //        +-> done
    //
    // before each VideoCallback() we check our current time against the video timestamp
    // and return from the function if the video timestamp is larger than the current time

    char buffer[256];
    FmvglueContextInternal* internal = static_cast<FmvglueContextInternal*>(context->Internal);
    if (internal == nullptr) {
        return FMVGLUE_ERROR_GENERIC;
    }
    AVFormatContext* format_context = internal->FormatContext;
    AVCodecContext* codec_context = internal->CodecContext;
    AVPacket* packet = internal->Packet;
    AVFrame* frame = internal->Frame;
    unsigned int stream_index = internal->VideoStreamIndex;
    UpdateStateEnum updateState = internal->UpdateState;
    int result;

    if (nanoseconds > 0) {
        UpdateVideoTime(context, static_cast<uint64_t>(nanoseconds));
    }

    while (true) {
        switch (updateState) {
            case UpdateStateEnum::Done: {
                internal->UpdateState = updateState;
                return FMVGLUE_ERROR_DONE;
            }
            case UpdateStateEnum::ReadFrame: {
                result = av_read_frame(format_context, packet);
                if (result < 0) {
                    if (result == AVERROR_EOF) {
                        updateState = UpdateStateEnum::FlushingSendPacket;
                        break;
                    }

                    av_strerror(result, buffer, sizeof(buffer));
                    printf("av_read_frame() error: %s\n", buffer);
                    internal->UpdateState = updateState;
                    return FMVGLUE_ERROR_GENERIC;
                }
                if (!(packet->stream_index >= 0
                      && static_cast<unsigned int>(packet->stream_index) == stream_index)) {
                    // not a packet we're interested in
                    updateState = UpdateStateEnum::UnrefPacket;
                    break;
                }
                updateState = UpdateStateEnum::SendPacket;
                break;
            }
            case UpdateStateEnum::SendPacket: {
                result = avcodec_send_packet(codec_context, packet);
                if (result < 0) {
                    av_strerror(result, buffer, sizeof(buffer));
                    printf("avcodec_send_packet() error: %s\n", buffer);
                    internal->UpdateState = updateState;
                    return FMVGLUE_ERROR_GENERIC;
                }
                updateState = UpdateStateEnum::ReceiveFrame;
                break;
            }
            case UpdateStateEnum::ReceiveFrame: {
                result = avcodec_receive_frame(codec_context, frame);
                if (result < 0) {
                    if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) {
                        // need to call av_read_frame() again
                        updateState = UpdateStateEnum::UnrefPacket;
                        break;
                    }
                    av_strerror(result, buffer, sizeof(buffer));
                    printf("avcodec_send_packet() error: %s\n", buffer);
                    internal->UpdateState = updateState;
                    return FMVGLUE_ERROR_GENERIC;
                }
                updateState = UpdateStateEnum::OutputFrame;
                break;
            }
            case UpdateStateEnum::OutputFrame: {
                if (nanoseconds < 0) {
                    internal->UpdateState = updateState;
                    return FMVGLUE_ERROR_OK;
                }
                int outputVideoFrameResult = OutputVideoFrame(context);
                if (outputVideoFrameResult == 0) {
                    internal->UpdateState = updateState;
                    return FMVGLUE_ERROR_OK;
                }
                if (outputVideoFrameResult < 0) {
                    internal->UpdateState = updateState;
                    return FMVGLUE_ERROR_GENERIC;
                }
                updateState = UpdateStateEnum::UnrefFrame;
                break;
            }
            case UpdateStateEnum::UnrefFrame: {
                av_frame_unref(frame);
                updateState = UpdateStateEnum::ReceiveFrame;
                break;
            }
            case UpdateStateEnum::UnrefPacket: {
                av_packet_unref(packet);
                updateState = UpdateStateEnum::ReadFrame;
                break;
            }
            case UpdateStateEnum::FlushingSendPacket: {
                result = avcodec_send_packet(codec_context, nullptr);
                if (result < 0) {
                    av_strerror(result, buffer, sizeof(buffer));
                    printf("avcodec_send_packet() error: %s\n", buffer);
                    internal->UpdateState = updateState;
                    return FMVGLUE_ERROR_GENERIC;
                }
                updateState = UpdateStateEnum::FlushingReceiveFrame;
                break;
            }
            case UpdateStateEnum::FlushingReceiveFrame: {
                result = avcodec_receive_frame(codec_context, frame);
                if (result < 0) {
                    if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) {
                        updateState = UpdateStateEnum::Done;
                        break;
                    }
                    av_strerror(result, buffer, sizeof(buffer));
                    printf("avcodec_send_packet() error: %s\n", buffer);
                    internal->UpdateState = updateState;
                    return FMVGLUE_ERROR_GENERIC;
                }
                updateState = UpdateStateEnum::FlushingOutputFrame;
                break;
            }
            case UpdateStateEnum::FlushingOutputFrame: {
                if (nanoseconds < 0) {
                    internal->UpdateState = updateState;
                    return FMVGLUE_ERROR_OK;
                }
                int outputVideoFrameResult = OutputVideoFrame(context);
                if (outputVideoFrameResult == 0) {
                    internal->UpdateState = updateState;
                    return FMVGLUE_ERROR_OK;
                }
                if (outputVideoFrameResult < 0) {
                    internal->UpdateState = updateState;
                    return FMVGLUE_ERROR_GENERIC;
                }
                updateState = UpdateStateEnum::FlushingUnrefFrame;
                break;
            }
            case UpdateStateEnum::FlushingUnrefFrame: {
                av_frame_unref(frame);
                updateState = UpdateStateEnum::FlushingReceiveFrame;
                break;
            }
            default: {
                break;
            }
        }
    }

    // should be unreachable
    assert(0);
    return FMVGLUE_ERROR_GENERIC;
}
