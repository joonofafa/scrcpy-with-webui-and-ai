#include "screenshot.h"

#include <stdlib.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "util/base64.h"
#include "util/log.h"

// Max dimension for AI screenshots. VLMs internally resize images, so sending
// at native resolution (e.g. 2400x1080) causes coordinate mismatches.
// Downscaling to 1024px ensures the AI's coordinate space matches what it sees.
#define AI_SCREENSHOT_MAX_DIM 1024

bool
sc_ai_screenshot_encode(struct sc_ai_screenshot *ss, const AVFrame *frame) {
    ss->png_data = NULL;
    ss->base64_data = NULL;
    ss->png_size = 0;
    ss->base64_size = 0;

    int src_w = frame->width;
    int src_h = frame->height;
    int dst_w = src_w;
    int dst_h = src_h;

    // Downscale if larger than max dimension
    int max_dim = src_w > src_h ? src_w : src_h;
    if (max_dim > AI_SCREENSHOT_MAX_DIM) {
        float scale = (float) AI_SCREENSHOT_MAX_DIM / max_dim;
        dst_w = (int)(src_w * scale);
        dst_h = (int)(src_h * scale);
        // Ensure even dimensions for codec compatibility
        dst_w = (dst_w + 1) & ~1;
        dst_h = (dst_h + 1) & ~1;
        if (dst_w < 2) dst_w = 2;
        if (dst_h < 2) dst_h = 2;
        LOGI("AI screenshot: downscale %dx%d -> %dx%d",
             src_w, src_h, dst_w, dst_h);
    }

    ss->width = (uint16_t) dst_w;
    ss->height = (uint16_t) dst_h;

    bool ok = false;

    // Convert to YUVJ420P (and downscale if needed) for JPEG encoding
    AVFrame *enc_frame = av_frame_alloc();
    if (!enc_frame) {
        LOGE("Could not allocate encoder frame");
        return false;
    }

    enc_frame->format = AV_PIX_FMT_YUVJ420P;
    enc_frame->width = dst_w;
    enc_frame->height = dst_h;

    if (av_image_alloc(enc_frame->data, enc_frame->linesize,
                       dst_w, dst_h, AV_PIX_FMT_YUVJ420P, 1) < 0) {
        LOGE("Could not allocate encoder image");
        av_frame_free(&enc_frame);
        return false;
    }

    struct SwsContext *sws = sws_getContext(
        src_w, src_h, (enum AVPixelFormat) frame->format,
        dst_w, dst_h, AV_PIX_FMT_YUVJ420P,
        SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) {
        LOGE("Could not create swscale context");
        goto free_enc;
    }

    sws_scale(sws, (const uint8_t *const *) frame->data, frame->linesize,
              0, frame->height, enc_frame->data, enc_frame->linesize);
    sws_freeContext(sws);

    // Encode to JPEG (much faster and smaller than PNG)
    const AVCodec *jpg_codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!jpg_codec) {
        LOGE("MJPEG encoder not found");
        goto free_enc;
    }

    AVCodecContext *enc = avcodec_alloc_context3(jpg_codec);
    if (!enc) {
        LOGE("Could not allocate MJPEG encoder context");
        goto free_enc;
    }

    enc->pix_fmt = AV_PIX_FMT_YUVJ420P;
    enc->width = dst_w;
    enc->height = dst_h;
    enc->time_base = (AVRational){1, 1};
    // Quality: 2-31 (lower = better), 5 is good balance
    enc->global_quality = 5 * FF_QP2LAMBDA;
    enc->flags |= AV_CODEC_FLAG_QSCALE;

    if (avcodec_open2(enc, jpg_codec, NULL) < 0) {
        LOGE("Could not open MJPEG encoder");
        avcodec_free_context(&enc);
        goto free_enc;
    }

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        LOGE("Could not allocate packet");
        avcodec_free_context(&enc);
        goto free_enc;
    }

    int ret = avcodec_send_frame(enc, enc_frame);
    if (ret < 0) {
        LOGE("Error sending frame to PNG encoder: %d", ret);
        goto free_pkt;
    }

    ret = avcodec_receive_packet(enc, pkt);
    if (ret < 0) {
        LOGE("Error receiving PNG packet: %d", ret);
        goto free_pkt;
    }

    // Copy JPEG data
    ss->png_data = malloc(pkt->size);
    if (!ss->png_data) {
        LOGE("Could not allocate JPEG data buffer");
        goto free_pkt;
    }
    memcpy(ss->png_data, pkt->data, pkt->size);
    ss->png_size = pkt->size;

    // Encode to base64
    size_t b64_len = sc_base64_encode_len(ss->png_size);
    ss->base64_data = malloc(b64_len);
    if (!ss->base64_data) {
        LOGE("Could not allocate base64 buffer");
        free(ss->png_data);
        ss->png_data = NULL;
        ss->png_size = 0;
        goto free_pkt;
    }
    ss->base64_size = sc_base64_encode(ss->png_data, ss->png_size,
                                        ss->base64_data);

    ok = true;

free_pkt:
    av_packet_free(&pkt);
    avcodec_free_context(&enc);
free_enc:
    av_freep(&enc_frame->data[0]);
    av_frame_free(&enc_frame);
    return ok;
}

void
sc_ai_screenshot_destroy(struct sc_ai_screenshot *ss) {
    free(ss->png_data);
    ss->png_data = NULL;
    free(ss->base64_data);
    ss->base64_data = NULL;
}
