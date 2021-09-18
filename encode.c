#include "encode.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

typedef struct {
    AVCodecContext *encodec_ctx;
    AVFilterContext *fil_src_ctx;
    AVFilterContext *fil_swp_ctx;
    AVFilterGraph *fil_graph;
    AVPacket packet;
    AVFrame *sframe;
    AVFrame *fframe;
    int filter_flush;
    int encode_flush;

    FILE *outfile;
} encoder_config_t;


static int encoder_filter_init(encoder_config_t *config, const char *filters) {
    assert(config);

    int ret = 0;
    char args[512];
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *bufferswp = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs    = avfilter_inout_alloc();
    AVFilterInOut *inputs     = avfilter_inout_alloc();
    AVRational time_base      = config->encodec_ctx->time_base;
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };

    config->fil_graph = avfilter_graph_alloc();
    if(!outputs || !inputs || !config->fil_graph){
        ret = AVERROR(ENOMEM);
        goto END;
    }

    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             config->encodec_ctx->width, config->encodec_ctx->height,
             AV_PIX_FMT_YUYV422,
             time_base.num, time_base.den,
             config->encodec_ctx->sample_aspect_ratio.num,
             config->encodec_ctx->sample_aspect_ratio.den);


    if((ret = avfilter_graph_create_filter(&config->fil_src_ctx, buffersrc, "in",
                                           args, NULL, config->fil_graph)) < 0){
        fprintf(stderr, "Could not create buffer source\n");
        goto END;
    }

    if((ret = avfilter_graph_create_filter(&config->fil_swp_ctx, bufferswp, "out",
                                           NULL, NULL, config->fil_graph)) < 0){
        fprintf(stderr, "Could not create buffer sink\n");
        goto END;
    }

    if((ret = av_opt_set_int_list(config->fil_swp_ctx, "pix_fmts", pix_fmts,
                                  AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0){
        fprintf(stderr, "Could not set output pixel format\n");
        goto END;
    }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = config->fil_src_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = config->fil_swp_ctx;
    inputs->pad_idx     = 0;
    inputs->next        = NULL;

    if((ret = avfilter_graph_parse_ptr(config->fil_graph, filters,
                                       &inputs, &outputs, NULL)) < 0){
        goto END;
    }

    if((ret = avfilter_graph_config(config->fil_graph, NULL)) < 0){
        goto END;
    }

END:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    (void) ret;
    return 0;
}

static int encoder_encode_frame(encoder_config_t *config) {
    assert(config);

    int ret;
    AVFrame *frame = !config->encode_flush ? config->fframe : NULL;
    ret = avcodec_send_frame(config->encodec_ctx, frame);
    if(ret < 0) {
       fprintf(stderr, "Error sending a frame for encoding.\n");
       return 1;
    }

    while(ret >=0) {
        ret = avcodec_receive_packet(config->encodec_ctx, &config->packet);
        if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
            return 0;
        } else if(ret < 0) {
            fprintf(stderr, "Error during encoding(%s)\n", av_err2str(ret));
            return 1;
        }

        fwrite(config->packet.data, 1, config->packet.size, config->outfile);
        av_packet_unref(&config->packet);
    }

    return 0;
}

static int encoder_filter_frame(encoder_config_t *config) {
    assert(config);

    int ret;
    AVFrame *frame = !config->filter_flush ? config->sframe : NULL;
    if((ret = av_buffersrc_add_frame_flags(config->fil_src_ctx, frame,
                                           AV_BUFFERSRC_FLAG_KEEP_REF)) < 0){
        return ret;
    }

    while(1){
        ret = av_buffersink_get_frame(config->fil_swp_ctx, config->fframe);
        if(ret == AVERROR_EOF || ret == AVERROR(EAGAIN)){
            break;
        }else if(ret < 0){
            fprintf(stderr, "filter void frame error(%s)\n", av_err2str(ret));
            return ret;
        }

        if((ret = encoder_encode_frame(config)) < 0){
            return ret;
        }

        av_frame_unref(config->fframe);
    }

    return 0;
}


int encoder_setup(encoder_t *encoder, const char *filters, int ofd) {

    encoder_config_t *config = (encoder_config_t *)malloc(sizeof(encoder_config_t));
    memset(config, 0, sizeof(encoder_config_t));
    encoder->priv = config;

    int ret;
    // init encodec context
    const AVCodec *encodec = avcodec_find_encoder_by_name("libx264");
    if(!encodec){
        fprintf(stderr, "Codec '%s' not found.\n", "libx264");
        return 1;
    }

    config->encodec_ctx = avcodec_alloc_context3(encodec);
    if(!config->encodec_ctx){
        fprintf(stderr, "Could not allocate video codec context.\n");
        return 1;
    }

    config->encodec_ctx->bit_rate  = 800000;
    config->encodec_ctx->width     = 1280;
    config->encodec_ctx->height    = 720;
    config->encodec_ctx->time_base = (AVRational){1, 10};
    config->encodec_ctx->framerate = (AVRational){10, 1};
    config->encodec_ctx->gop_size  = 10;
    config->encodec_ctx->max_b_frames = 1;
    config->encodec_ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
    if(encodec->id == AV_CODEC_ID_H264){
        av_opt_set(config->encodec_ctx->priv_data, "preset", "slow", 0);
    }

    ret = avcodec_open2(config->encodec_ctx, encodec, NULL);
    if(ret < 0){
        fprintf(stderr, "Could not open codec: %s\n", av_err2str(ret));
        return 1;
    }

    av_packet_unref(&config->packet);

    config->sframe = av_frame_alloc();
    config->fframe = av_frame_alloc();
    if (!config->sframe || !config->fframe) {
        fprintf(stderr, "Could not allocate video frame\n");
        return 1;
    }

    config->outfile = fdopen(ofd, "wb");
    if(!config->outfile){
        fprintf(stderr, "Could not open outfile.\n");
        return 1;
    }

    ret = encoder_filter_init(config, filters);
    if(ret != 0){
        fprintf(stderr, "Could not init video filter.\n");
        return 1;
    }

    config->sframe->format = AV_PIX_FMT_YUYV422;
    config->sframe->width  = config->encodec_ctx->width;
    config->sframe->height = config->encodec_ctx->height;
    ret = av_frame_get_buffer(config->sframe, 0);
    if(ret < 0) {
        fprintf(stderr, "Could not allocate the video frame data\n");
        return 1;
    }


    return 0;
}

int encoder_frame(encoder_t *encoder, void *data, size_t len, int64_t pts) {
    assert(encoder);
    encoder_config_t *config = (encoder_config_t *)encoder->priv;
    assert(config);

    int ret;

    memcpy(config->sframe->data[0], data, len);
    config->sframe->pts = pts;

    ret = encoder_filter_frame(config);
    if(ret != 0) {
        fprintf(stderr, "filter frame error(%s :pts: %ld)\n", av_err2str(ret), pts);
        return ret;
    }

    return 0;
}

int encoder_release(encoder_t *encoder) {
    assert(encoder);
    encoder_config_t *config = (encoder_config_t *)encoder->priv;
    assert(config);

    config->filter_flush = 1;
    encoder_filter_frame(config);

    config->encode_flush = 1;
    encoder_encode_frame(config);

    fflush(config->outfile);
    fclose(config->outfile);

    avfilter_graph_free(&config->fil_graph);
    avcodec_free_context(&config->encodec_ctx);
    av_frame_free(&config->sframe);
    av_frame_free(&config->fframe);
    av_packet_unref(&config->packet);

    free(config);
    encoder->priv = NULL;

    return 0;
}
