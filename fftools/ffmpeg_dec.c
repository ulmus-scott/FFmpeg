/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avassert.h"
#include "libavutil/dict.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/timestamp.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/codec.h"

#include "libavfilter/buffersrc.h"

#include "ffmpeg.h"

static void check_decode_result(InputStream *ist, int got_output, int ret)
{
    if (ret < 0)
        ist->decode_errors++;

    if (ret < 0 && exit_on_error)
        exit_program(1);

    if (got_output && ist->dec_ctx->codec_type != AVMEDIA_TYPE_SUBTITLE) {
        if (ist->decoded_frame->decode_error_flags || (ist->decoded_frame->flags & AV_FRAME_FLAG_CORRUPT)) {
            av_log(ist, exit_on_error ? AV_LOG_FATAL : AV_LOG_WARNING,
                   "corrupt decoded frame\n");
            if (exit_on_error)
                exit_program(1);
        }
    }
}

static int send_frame_to_filters(InputStream *ist, AVFrame *decoded_frame)
{
    int i, ret;

    av_assert1(ist->nb_filters > 0); /* ensure ret is initialized */
    for (i = 0; i < ist->nb_filters; i++) {
        ret = ifilter_send_frame(ist->filters[i], decoded_frame, i < ist->nb_filters - 1);
        if (ret == AVERROR_EOF)
            ret = 0; /* ignore */
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Failed to inject frame into filter network: %s\n", av_err2str(ret));
            break;
        }
    }
    return ret;
}

static AVRational audio_samplerate_update(InputStream *ist, const AVFrame *frame)
{
    const int prev = ist->last_frame_tb.den;
    const int sr   = frame->sample_rate;

    AVRational tb_new;
    int64_t gcd;

    if (frame->sample_rate == ist->last_frame_sample_rate)
        goto finish;

    gcd  = av_gcd(prev, sr);

    if (prev / gcd >= INT_MAX / sr) {
        av_log(ist, AV_LOG_WARNING,
               "Audio timestamps cannot be represented exactly after "
               "sample rate change: %d -> %d\n", prev, sr);

        // LCM of 192000, 44100, allows to represent all common samplerates
        tb_new = (AVRational){ 1, 28224000 };
    } else
        tb_new = (AVRational){ 1, prev / gcd * sr };

    // keep the frame timebase if it is strictly better than
    // the samplerate-defined one
    if (frame->time_base.num == 1 && frame->time_base.den > tb_new.den &&
        !(frame->time_base.den % tb_new.den))
        tb_new = frame->time_base;

    if (ist->last_frame_pts != AV_NOPTS_VALUE)
        ist->last_frame_pts = av_rescale_q(ist->last_frame_pts,
                                           ist->last_frame_tb, tb_new);
    ist->last_frame_duration_est = av_rescale_q(ist->last_frame_duration_est,
                                                ist->last_frame_tb, tb_new);

    ist->last_frame_tb          = tb_new;
    ist->last_frame_sample_rate = frame->sample_rate;

finish:
    return ist->last_frame_tb;
}

static void audio_ts_process(InputStream *ist, AVFrame *frame)
{
    AVRational tb_filter = (AVRational){1, frame->sample_rate};
    AVRational tb;
    int64_t pts_pred;

    // on samplerate change, choose a new internal timebase for timestamp
    // generation that can represent timestamps from all the samplerates
    // seen so far
    tb = audio_samplerate_update(ist, frame);
    pts_pred = ist->last_frame_pts == AV_NOPTS_VALUE ? 0 :
               ist->last_frame_pts + ist->last_frame_duration_est;

    if (frame->pts == AV_NOPTS_VALUE) {
        frame->pts = pts_pred;
        frame->time_base = tb;
    } else if (ist->last_frame_pts != AV_NOPTS_VALUE &&
               frame->pts > av_rescale_q_rnd(pts_pred, tb, frame->time_base,
                                             AV_ROUND_UP)) {
        // there was a gap in timestamps, reset conversion state
        ist->filter_in_rescale_delta_last = AV_NOPTS_VALUE;
    }

    frame->pts = av_rescale_delta(frame->time_base, frame->pts,
                                  tb, frame->nb_samples,
                                  &ist->filter_in_rescale_delta_last, tb);

    ist->last_frame_pts          = frame->pts;
    ist->last_frame_duration_est = av_rescale_q(frame->nb_samples,
                                                tb_filter, tb);

    // finally convert to filtering timebase
    frame->pts       = av_rescale_q(frame->pts, tb, tb_filter);
    frame->duration  = frame->nb_samples;
    frame->time_base = tb_filter;
}

static int decode_audio(InputStream *ist, AVFrame *decoded_frame)
{
    int ret, err = 0;

    ist->samples_decoded += decoded_frame->nb_samples;
    ist->frames_decoded++;

    audio_ts_process(ist, decoded_frame);

    ist->nb_samples = decoded_frame->nb_samples;
    err = send_frame_to_filters(ist, decoded_frame);

    av_frame_unref(decoded_frame);
    return err < 0 ? err : ret;
}

static int64_t video_duration_estimate(const InputStream *ist, const AVFrame *frame)
{
    const InputFile   *ifile = input_files[ist->file_index];
    int64_t codec_duration = 0;

    // XXX lavf currently makes up frame durations when they are not provided by
    // the container. As there is no way to reliably distinguish real container
    // durations from the fake made-up ones, we use heuristics based on whether
    // the container has timestamps. Eventually lavf should stop making up
    // durations, then this should be simplified.

    // prefer frame duration for containers with timestamps
    if (frame->duration > 0 && (!ifile->format_nots || ist->framerate.num))
        return frame->duration;

    if (ist->dec_ctx->framerate.den && ist->dec_ctx->framerate.num) {
        int fields = frame->repeat_pict + 2;
        AVRational field_rate = av_mul_q(ist->dec_ctx->framerate,
                                         (AVRational){ 2, 1 });
        codec_duration = av_rescale_q(fields, av_inv_q(field_rate),
                                      frame->time_base);
    }

    // prefer codec-layer duration for containers without timestamps
    if (codec_duration > 0 && ifile->format_nots)
        return codec_duration;

    // when timestamps are available, repeat last frame's actual duration
    // (i.e. pts difference between this and last frame)
    if (frame->pts != AV_NOPTS_VALUE && ist->last_frame_pts != AV_NOPTS_VALUE &&
        frame->pts > ist->last_frame_pts)
        return frame->pts - ist->last_frame_pts;

    // try frame/codec duration
    if (frame->duration > 0)
        return frame->duration;
    if (codec_duration > 0)
        return codec_duration;

    // try average framerate
    if (ist->st->avg_frame_rate.num && ist->st->avg_frame_rate.den) {
        int64_t d = av_rescale_q(1, av_inv_q(ist->st->avg_frame_rate),
                                 frame->time_base);
        if (d > 0)
            return d;
    }

    // last resort is last frame's estimated duration, and 1
    return FFMAX(ist->last_frame_duration_est, 1);
}

static int decode_video(InputStream *ist, AVFrame *frame)
{
    int ret = 0, err = 0;

    // The following line may be required in some cases where there is no parser
    // or the parser does not has_b_frames correctly
    if (ist->par->video_delay < ist->dec_ctx->has_b_frames) {
        if (ist->dec_ctx->codec_id == AV_CODEC_ID_H264) {
            ist->par->video_delay = ist->dec_ctx->has_b_frames;
        } else
            av_log(ist->dec_ctx, AV_LOG_WARNING,
                   "video_delay is larger in decoder than demuxer %d > %d.\n"
                   "If you want to help, upload a sample "
                   "of this file to https://streams.videolan.org/upload/ "
                   "and contact the ffmpeg-devel mailing list. (ffmpeg-devel@ffmpeg.org)\n",
                   ist->dec_ctx->has_b_frames,
                   ist->par->video_delay);
    }

        if (ist->dec_ctx->width  != frame->width ||
            ist->dec_ctx->height != frame->height ||
            ist->dec_ctx->pix_fmt != frame->format) {
            av_log(NULL, AV_LOG_DEBUG, "Frame parameters mismatch context %d,%d,%d != %d,%d,%d\n",
                frame->width,
                frame->height,
                frame->format,
                ist->dec_ctx->width,
                ist->dec_ctx->height,
                ist->dec_ctx->pix_fmt);
        }

    if(ist->top_field_first>=0)
        frame->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;

    ist->frames_decoded++;

    if (ist->hwaccel_retrieve_data && frame->format == ist->hwaccel_pix_fmt) {
        err = ist->hwaccel_retrieve_data(ist->dec_ctx, frame);
        if (err < 0)
            goto fail;
    }

    frame->pts = frame->best_effort_timestamp;

    // forced fixed framerate
    if (ist->framerate.num) {
        frame->pts       = AV_NOPTS_VALUE;
        frame->duration  = 1;
        frame->time_base = av_inv_q(ist->framerate);
    }

    // no timestamp available - extrapolate from previous frame duration
    if (frame->pts == AV_NOPTS_VALUE)
        frame->pts = ist->last_frame_pts == AV_NOPTS_VALUE ? 0 :
                     ist->last_frame_pts + ist->last_frame_duration_est;

    // update timestamp history
    ist->last_frame_duration_est = video_duration_estimate(ist, frame);
    ist->last_frame_pts          = frame->pts;
    ist->last_frame_tb           = frame->time_base;

    if (debug_ts) {
        av_log(ist, AV_LOG_INFO,
               "decoder -> pts:%s pts_time:%s "
               "pkt_dts:%s pkt_dts_time:%s "
               "duration:%s duration_time:%s "
               "keyframe:%d frame_type:%d time_base:%d/%d\n",
               av_ts2str(frame->pts),
               av_ts2timestr(frame->pts, &frame->time_base),
               av_ts2str(frame->pkt_dts),
               av_ts2timestr(frame->pkt_dts, &frame->time_base),
               av_ts2str(frame->duration),
               av_ts2timestr(frame->duration, &frame->time_base),
               !!(frame->flags & AV_FRAME_FLAG_KEY), frame->pict_type,
               frame->time_base.num, frame->time_base.den);
    }

    if (ist->st->sample_aspect_ratio.num)
        frame->sample_aspect_ratio = ist->st->sample_aspect_ratio;

    err = send_frame_to_filters(ist, frame);

fail:
    av_frame_unref(frame);
    return err < 0 ? err : ret;
}

static void sub2video_flush(InputStream *ist)
{
    int i;
    int ret;

    if (ist->sub2video.end_pts < INT64_MAX)
        sub2video_update(ist, INT64_MAX, NULL);
    for (i = 0; i < ist->nb_filters; i++) {
        ret = av_buffersrc_add_frame(ist->filters[i]->filter, NULL);
        if (ret != AVERROR_EOF && ret < 0)
            av_log(NULL, AV_LOG_WARNING, "Flush the frame error.\n");
    }
}

int process_subtitle(InputStream *ist, AVSubtitle *subtitle, int *got_output)
{
    int ret = 0;
    int free_sub = 1;

    if (ist->fix_sub_duration) {
        int end = 1;
        if (ist->prev_sub.got_output) {
            end = av_rescale(subtitle->pts - ist->prev_sub.subtitle.pts,
                             1000, AV_TIME_BASE);
            if (end < ist->prev_sub.subtitle.end_display_time) {
                av_log(NULL, AV_LOG_DEBUG,
                       "Subtitle duration reduced from %"PRId32" to %d%s\n",
                       ist->prev_sub.subtitle.end_display_time, end,
                       end <= 0 ? ", dropping it" : "");
                ist->prev_sub.subtitle.end_display_time = end;
            }
        }
        FFSWAP(int,        *got_output, ist->prev_sub.got_output);
        FFSWAP(int,        ret,         ist->prev_sub.ret);
        FFSWAP(AVSubtitle, *subtitle,   ist->prev_sub.subtitle);
        if (end <= 0)
            goto out;
    }

    if (!*got_output)
        return ret;

    if (ist->sub2video.frame) {
        sub2video_update(ist, INT64_MIN, subtitle);
    } else if (ist->nb_filters) {
        if (!ist->sub2video.sub_queue)
            ist->sub2video.sub_queue = av_fifo_alloc2(8, sizeof(AVSubtitle), AV_FIFO_FLAG_AUTO_GROW);
        if (!ist->sub2video.sub_queue)
            report_and_exit(AVERROR(ENOMEM));

        ret = av_fifo_write(ist->sub2video.sub_queue, subtitle, 1);
        if (ret < 0)
            exit_program(1);
        free_sub = 0;
    }

    if (!subtitle->num_rects)
        goto out;

    for (int oidx = 0; oidx < ist->nb_outputs; oidx++) {
        OutputStream *ost = ist->outputs[oidx];
        if (!ost->enc || ost->type != AVMEDIA_TYPE_SUBTITLE)
            continue;

        enc_subtitle(output_files[ost->file_index], ost, subtitle);
    }

out:
    if (free_sub)
        avsubtitle_free(subtitle);
    return ret;
}

static int transcode_subtitles(InputStream *ist, const AVPacket *pkt)
{
    AVSubtitle subtitle;
    int got_output;
    int ret = avcodec_decode_subtitle2(ist->dec_ctx,
                                       &subtitle, &got_output, pkt);

    if (ret < 0) {
        av_log(ist, AV_LOG_ERROR, "Error decoding subtitles: %s\n",
                av_err2str(ret));
        if (exit_on_error)
            exit_program(1);
    }

    check_decode_result(ist, got_output, ret);

    if (ret < 0 || !got_output) {
        if (!pkt->size)
            sub2video_flush(ist);
        return ret < 0 ? ret : AVERROR_EOF;
    }

    ist->frames_decoded++;

    return process_subtitle(ist, &subtitle, &got_output);
}

static int send_filter_eof(InputStream *ist)
{
    int i, ret;

    for (i = 0; i < ist->nb_filters; i++) {
        int64_t end_pts = ist->last_frame_pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE :
                          ist->last_frame_pts + ist->last_frame_duration_est;
        ret = ifilter_send_eof(ist->filters[i], end_pts, ist->last_frame_tb);
        if (ret < 0)
            return ret;
    }
    return 0;
}

int dec_packet(InputStream *ist, const AVPacket *pkt, int no_eof)
{
    AVCodecContext *dec = ist->dec_ctx;
    const char *type_desc = av_get_media_type_string(dec->codec_type);
    int ret;

    if (dec->codec_type == AVMEDIA_TYPE_SUBTITLE)
        return transcode_subtitles(ist, pkt ? pkt : ist->pkt);

    // With fate-indeo3-2, we're getting 0-sized packets before EOF for some
    // reason. This seems like a semi-critical bug. Don't trigger EOF, and
    // skip the packet.
    if (pkt && pkt->size == 0)
        return 0;

    ret = avcodec_send_packet(dec, pkt);
    if (ret < 0 && !(ret == AVERROR_EOF && !pkt)) {
        // In particular, we don't expect AVERROR(EAGAIN), because we read all
        // decoded frames with avcodec_receive_frame() until done.
        av_log(ist, AV_LOG_ERROR, "Error submitting %s to decoder: %s\n",
               pkt ? "packet" : "EOF", av_err2str(ret));
        if (exit_on_error)
            exit_program(1);

        if (ret != AVERROR_EOF)
            ist->decode_errors++;

        return ret;
    }

    while (1) {
        AVFrame *frame = ist->decoded_frame;

        update_benchmark(NULL);
        ret = avcodec_receive_frame(dec, frame);
        update_benchmark("decode_%s %d.%d", type_desc,
                         ist->file_index, ist->st->index);

        if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN))
            check_decode_result(ist, ret >= 0, ret);

        if (ret == AVERROR(EAGAIN)) {
            av_assert0(pkt); // should never happen during flushing
            return 0;
        } else if (ret == AVERROR_EOF) {
            /* after flushing, send an EOF on all the filter inputs attached to the stream */
            /* except when looping we need to flush but not to send an EOF */
            if (!no_eof) {
                ret = send_filter_eof(ist);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_FATAL, "Error marking filters as finished\n");
                    exit_program(1);
                }
            }

            return AVERROR_EOF;
        } else if (ret < 0) {
            av_log(ist, AV_LOG_ERROR, "Decoding error: %s\n", av_err2str(ret));
            if (exit_on_error)
                exit_program(1);
            return ret;
        }

        if (ist->want_frame_data) {
            FrameData *fd;

            av_assert0(!frame->opaque_ref);
            frame->opaque_ref = av_buffer_allocz(sizeof(*fd));
            if (!frame->opaque_ref) {
                av_frame_unref(frame);
                report_and_exit(AVERROR(ENOMEM));
            }
            fd      = (FrameData*)frame->opaque_ref->data;
            fd->pts = frame->pts;
            fd->tb  = dec->pkt_timebase;
            fd->idx = dec->frame_num - 1;
        }

        frame->time_base = dec->pkt_timebase;

        ret = dec->codec_type == AVMEDIA_TYPE_AUDIO ?
                decode_audio(ist, frame)            :
                decode_video(ist, frame);

        if (ret < 0) {
            av_log(NULL, AV_LOG_FATAL, "Error while processing the decoded "
                   "data for stream #%d:%d\n", ist->file_index, ist->st->index);
            exit_program(1);
        }
    }
}

static enum AVPixelFormat get_format(AVCodecContext *s, const enum AVPixelFormat *pix_fmts)
{
    InputStream *ist = s->opaque;
    const enum AVPixelFormat *p;
    int ret;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        const AVCodecHWConfig  *config = NULL;
        int i;

        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            break;

        if (ist->hwaccel_id == HWACCEL_GENERIC ||
            ist->hwaccel_id == HWACCEL_AUTO) {
            for (i = 0;; i++) {
                config = avcodec_get_hw_config(s->codec, i);
                if (!config)
                    break;
                if (!(config->methods &
                      AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                    continue;
                if (config->pix_fmt == *p)
                    break;
            }
        }
        if (config && config->device_type == ist->hwaccel_device_type) {
            ret = hwaccel_decode_init(s);
            if (ret < 0) {
                if (ist->hwaccel_id == HWACCEL_GENERIC) {
                    av_log(NULL, AV_LOG_FATAL,
                           "%s hwaccel requested for input stream #%d:%d, "
                           "but cannot be initialized.\n",
                           av_hwdevice_get_type_name(config->device_type),
                           ist->file_index, ist->st->index);
                    return AV_PIX_FMT_NONE;
                }
                continue;
            }

            ist->hwaccel_pix_fmt = *p;
            break;
        }
    }

    return *p;
}

int dec_open(InputStream *ist)
{
    const AVCodec *codec = ist->dec;
    int ret;

    if (!codec) {
        av_log(ist, AV_LOG_ERROR,
               "Decoding requested, but no decoder found for: %s\n",
                avcodec_get_name(ist->dec_ctx->codec_id));
        return AVERROR(EINVAL);
    }

    ist->dec_ctx->opaque                = ist;
    ist->dec_ctx->get_format            = get_format;

    if (ist->dec_ctx->codec_id == AV_CODEC_ID_DVB_SUBTITLE &&
       (ist->decoding_needed & DECODING_FOR_OST)) {
        av_dict_set(&ist->decoder_opts, "compute_edt", "1", AV_DICT_DONT_OVERWRITE);
        if (ist->decoding_needed & DECODING_FOR_FILTER)
            av_log(NULL, AV_LOG_WARNING, "Warning using DVB subtitles for filtering and output at the same time is not fully supported, also see -compute_edt [0|1]\n");
    }

    /* Useful for subtitles retiming by lavf (FIXME), skipping samples in
     * audio, and video decoders such as cuvid or mediacodec */
    ist->dec_ctx->pkt_timebase = ist->st->time_base;

    if (!av_dict_get(ist->decoder_opts, "threads", NULL, 0))
        av_dict_set(&ist->decoder_opts, "threads", "auto", 0);
    /* Attached pics are sparse, therefore we would not want to delay their decoding till EOF. */
    if (ist->st->disposition & AV_DISPOSITION_ATTACHED_PIC)
        av_dict_set(&ist->decoder_opts, "threads", "1", 0);

    ret = hw_device_setup_for_decode(ist);
    if (ret < 0) {
        av_log(ist, AV_LOG_ERROR,
               "Hardware device setup failed for decoder: %s\n",
               av_err2str(ret));
        return ret;
    }

    if ((ret = avcodec_open2(ist->dec_ctx, codec, &ist->decoder_opts)) < 0) {
        if (ret == AVERROR_EXPERIMENTAL)
            exit_program(1);

        av_log(ist, AV_LOG_ERROR, "Error while opening decoder: %s\n",
               av_err2str(ret));
        return ret;
    }
    assert_avoptions(ist->decoder_opts);

    return 0;
}
