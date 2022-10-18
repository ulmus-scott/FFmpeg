/*
 * Muxer/output file setup.
 *
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

#include <string.h>

#include "cmdutils.h"
#include "ffmpeg.h"
#include "ffmpeg_mux.h"
#include "fopen_utf8.h"

#include "libavformat/avformat.h"
#include "libavformat/avio.h"

#include "libavcodec/avcodec.h"

#include "libavfilter/avfilter.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/bprint.h"
#include "libavutil/dict.h"
#include "libavutil/getenv_utf8.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"

#define DEFAULT_PASS_LOGFILENAME_PREFIX "ffmpeg2pass"

static const char *const opt_name_apad[]                      = {"apad", NULL};
static const char *const opt_name_autoscale[]                 = {"autoscale", NULL};
static const char *const opt_name_bits_per_raw_sample[]       = {"bits_per_raw_sample", NULL};
static const char *const opt_name_bitstream_filters[]         = {"bsf", "absf", "vbsf", NULL};
static const char *const opt_name_copy_initial_nonkeyframes[] = {"copyinkf", NULL};
static const char *const opt_name_copy_prior_start[]          = {"copypriorss", NULL};
static const char *const opt_name_disposition[]               = {"disposition", NULL};
static const char *const opt_name_enc_time_bases[]            = {"enc_time_base", NULL};
static const char *const opt_name_filters[]                   = {"filter", "af", "vf", NULL};
static const char *const opt_name_filter_scripts[]            = {"filter_script", NULL};
static const char *const opt_name_fps_mode[]                  = {"fps_mode", NULL};
static const char *const opt_name_force_fps[]                 = {"force_fps", NULL};
static const char *const opt_name_forced_key_frames[]         = {"forced_key_frames", NULL};
static const char *const opt_name_frame_aspect_ratios[]       = {"aspect", NULL};
static const char *const opt_name_intra_matrices[]            = {"intra_matrix", NULL};
static const char *const opt_name_inter_matrices[]            = {"inter_matrix", NULL};
static const char *const opt_name_chroma_intra_matrices[]     = {"chroma_intra_matrix", NULL};
static const char *const opt_name_max_frame_rates[]           = {"fpsmax", NULL};
static const char *const opt_name_max_frames[]                = {"frames", "aframes", "vframes", "dframes", NULL};
static const char *const opt_name_max_muxing_queue_size[]     = {"max_muxing_queue_size", NULL};
static const char *const opt_name_muxing_queue_data_threshold[] = {"muxing_queue_data_threshold", NULL};
static const char *const opt_name_pass[]                      = {"pass", NULL};
static const char *const opt_name_passlogfiles[]              = {"passlogfile", NULL};
static const char *const opt_name_presets[]                   = {"pre", "apre", "vpre", "spre", NULL};
static const char *const opt_name_qscale[]                    = {"q", "qscale", NULL};
static const char *const opt_name_rc_overrides[]              = {"rc_override", NULL};
static const char *const opt_name_time_bases[]                = {"time_base", NULL};
static const char *const opt_name_audio_channels[]            = {"ac", NULL};
static const char *const opt_name_audio_ch_layouts[]          = {"channel_layout", "ch_layout", NULL};
static const char *const opt_name_audio_sample_rate[]         = {"ar", NULL};
static const char *const opt_name_frame_sizes[]               = {"s", NULL};
static const char *const opt_name_frame_pix_fmts[]            = {"pix_fmt", NULL};
static const char *const opt_name_sample_fmts[]               = {"sample_fmt", NULL};

static int check_opt_bitexact(void *ctx, const AVDictionary *opts,
                              const char *opt_name, int flag)
{
    const AVDictionaryEntry *e = av_dict_get(opts, opt_name, NULL, 0);

    if (e) {
        const AVOption *o = av_opt_find(ctx, opt_name, NULL, 0, 0);
        int val = 0;
        if (!o)
            return 0;
        av_opt_eval_flags(ctx, o, e->value, &val);
        return !!(val & flag);
    }
    return 0;
}

static int choose_encoder(OptionsContext *o, AVFormatContext *s,
                          OutputStream *ost, const AVCodec **enc)
{
    enum AVMediaType type = ost->st->codecpar->codec_type;
    char *codec_name = NULL;

    *enc = NULL;

    if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO || type == AVMEDIA_TYPE_SUBTITLE) {
        MATCH_PER_STREAM_OPT(codec_names, str, codec_name, s, ost->st);
        if (!codec_name) {
            ost->st->codecpar->codec_id = av_guess_codec(s->oformat, NULL, s->url,
                                                         NULL, ost->st->codecpar->codec_type);
            *enc = avcodec_find_encoder(ost->st->codecpar->codec_id);
            if (!*enc) {
                av_log(NULL, AV_LOG_FATAL, "Automatic encoder selection failed for "
                       "output stream #%d:%d. Default encoder for format %s (codec %s) is "
                       "probably disabled. Please choose an encoder manually.\n",
                       ost->file_index, ost->index, s->oformat->name,
                       avcodec_get_name(ost->st->codecpar->codec_id));
                return AVERROR_ENCODER_NOT_FOUND;
            }
        } else if (strcmp(codec_name, "copy")) {
            *enc = find_codec_or_die(codec_name, ost->st->codecpar->codec_type, 1);
            ost->st->codecpar->codec_id = (*enc)->id;
        }
    }

    return 0;
}

static char *get_line(AVIOContext *s, AVBPrint *bprint)
{
    char c;

    while ((c = avio_r8(s)) && c != '\n')
        av_bprint_chars(bprint, c, 1);

    if (!av_bprint_is_complete(bprint))
        report_and_exit(AVERROR(ENOMEM));
    return bprint->str;
}

static int get_preset_file_2(const char *preset_name, const char *codec_name, AVIOContext **s)
{
    int i, ret = -1;
    char filename[1000];
    char *env_avconv_datadir = getenv_utf8("AVCONV_DATADIR");
    char *env_home = getenv_utf8("HOME");
    const char *base[3] = { env_avconv_datadir,
                            env_home,
                            AVCONV_DATADIR,
                            };

    for (i = 0; i < FF_ARRAY_ELEMS(base) && ret < 0; i++) {
        if (!base[i])
            continue;
        if (codec_name) {
            snprintf(filename, sizeof(filename), "%s%s/%s-%s.avpreset", base[i],
                     i != 1 ? "" : "/.avconv", codec_name, preset_name);
            ret = avio_open2(s, filename, AVIO_FLAG_READ, &int_cb, NULL);
        }
        if (ret < 0) {
            snprintf(filename, sizeof(filename), "%s%s/%s.avpreset", base[i],
                     i != 1 ? "" : "/.avconv", preset_name);
            ret = avio_open2(s, filename, AVIO_FLAG_READ, &int_cb, NULL);
        }
    }
    freeenv_utf8(env_home);
    freeenv_utf8(env_avconv_datadir);
    return ret;
}

static OutputStream *new_output_stream(Muxer *mux, OptionsContext *o,
                                       enum AVMediaType type, int source_index)
{
    AVFormatContext *oc = mux->fc;
    MuxStream     *ms;
    OutputStream *ost;
    const AVCodec *enc;
    AVStream *st = avformat_new_stream(oc, NULL);
    int idx      = oc->nb_streams - 1, ret = 0;
    const char *bsfs = NULL, *time_base = NULL;
    char *next, *codec_tag = NULL;
    double qscale = -1;
    int i;

    if (!st)
        report_and_exit(AVERROR(ENOMEM));

    if (oc->nb_streams - 1 < o->nb_streamid_map)
        st->id = o->streamid_map[oc->nb_streams - 1];

    ms  = allocate_array_elem(&mux->of.streams, sizeof(MuxStream),
                              &mux->of.nb_streams);
    ost = &ms->ost;

    ms->muxing_queue = av_fifo_alloc2(8, sizeof(AVPacket*), 0);
    if (!ms->muxing_queue)
        report_and_exit(AVERROR(ENOMEM));
    ms->last_mux_dts = AV_NOPTS_VALUE;

    ost->file_index = nb_output_files - 1;
    ost->index      = idx;
    ost->st         = st;
    ost->forced_kf_ref_pts = AV_NOPTS_VALUE;
    st->codecpar->codec_type = type;

    ret = choose_encoder(o, oc, ost, &enc);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL, "Error selecting an encoder for stream "
               "%d:%d\n", ost->file_index, ost->index);
        exit_program(1);
    }

    if (enc) {
        ost->enc_ctx = avcodec_alloc_context3(enc);
        if (!ost->enc_ctx)
            report_and_exit(AVERROR(ENOMEM));
    }

    ost->filtered_frame = av_frame_alloc();
    if (!ost->filtered_frame)
        report_and_exit(AVERROR(ENOMEM));

    ost->pkt = av_packet_alloc();
    if (!ost->pkt)
        report_and_exit(AVERROR(ENOMEM));

    if (ost->enc_ctx) {
        AVCodecContext *enc = ost->enc_ctx;
        AVIOContext *s = NULL;
        char *buf = NULL, *arg = NULL, *preset = NULL;

        ost->encoder_opts = filter_codec_opts(o->g->codec_opts, enc->codec_id,
                                              oc, st, enc->codec);

        MATCH_PER_STREAM_OPT(presets, str, preset, oc, st);
        ost->autoscale = 1;
        MATCH_PER_STREAM_OPT(autoscale, i, ost->autoscale, oc, st);
        if (preset && (!(ret = get_preset_file_2(preset, enc->codec->name, &s)))) {
            AVBPrint bprint;
            av_bprint_init(&bprint, 0, AV_BPRINT_SIZE_UNLIMITED);
            do  {
                av_bprint_clear(&bprint);
                buf = get_line(s, &bprint);
                if (!buf[0] || buf[0] == '#')
                    continue;
                if (!(arg = strchr(buf, '='))) {
                    av_log(NULL, AV_LOG_FATAL, "Invalid line found in the preset file.\n");
                    exit_program(1);
                }
                *arg++ = 0;
                av_dict_set(&ost->encoder_opts, buf, arg, AV_DICT_DONT_OVERWRITE);
            } while (!s->eof_reached);
            av_bprint_finalize(&bprint, NULL);
            avio_closep(&s);
        }
        if (ret) {
            av_log(NULL, AV_LOG_FATAL,
                   "Preset %s specified for stream %d:%d, but could not be opened.\n",
                   preset, ost->file_index, ost->index);
            exit_program(1);
        }
    } else {
        ost->encoder_opts = filter_codec_opts(o->g->codec_opts, AV_CODEC_ID_NONE, oc, st, NULL);
    }


    if (o->bitexact) {
        ost->bitexact        = 1;
    } else if (ost->enc_ctx) {
        ost->bitexact        = check_opt_bitexact(ost->enc_ctx, ost->encoder_opts, "flags",
                                                  AV_CODEC_FLAG_BITEXACT);
    }

    MATCH_PER_STREAM_OPT(time_bases, str, time_base, oc, st);
    if (time_base) {
        AVRational q;
        if (av_parse_ratio(&q, time_base, INT_MAX, 0, NULL) < 0 ||
            q.num <= 0 || q.den <= 0) {
            av_log(NULL, AV_LOG_FATAL, "Invalid time base: %s\n", time_base);
            exit_program(1);
        }
        st->time_base = q;
    }

    MATCH_PER_STREAM_OPT(enc_time_bases, str, time_base, oc, st);
    if (time_base) {
        AVRational q;
        if (av_parse_ratio(&q, time_base, INT_MAX, 0, NULL) < 0 ||
            q.den <= 0) {
            av_log(NULL, AV_LOG_FATAL, "Invalid time base: %s\n", time_base);
            exit_program(1);
        }
        ost->enc_timebase = q;
    }

    ost->max_frames = INT64_MAX;
    MATCH_PER_STREAM_OPT(max_frames, i64, ost->max_frames, oc, st);
    for (i = 0; i<o->nb_max_frames; i++) {
        char *p = o->max_frames[i].specifier;
        if (!*p && type != AVMEDIA_TYPE_VIDEO) {
            av_log(NULL, AV_LOG_WARNING, "Applying unspecific -frames to non video streams, maybe you meant -vframes ?\n");
            break;
        }
    }

    ost->copy_prior_start = -1;
    MATCH_PER_STREAM_OPT(copy_prior_start, i, ost->copy_prior_start, oc ,st);

    MATCH_PER_STREAM_OPT(bitstream_filters, str, bsfs, oc, st);
    if (bsfs && *bsfs) {
        ret = av_bsf_list_parse_str(bsfs, &ms->bsf_ctx);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error parsing bitstream filter sequence '%s': %s\n", bsfs, av_err2str(ret));
            exit_program(1);
        }
    }

    MATCH_PER_STREAM_OPT(codec_tags, str, codec_tag, oc, st);
    if (codec_tag) {
        uint32_t tag = strtol(codec_tag, &next, 0);
        if (*next)
            tag = AV_RL32(codec_tag);
        ost->st->codecpar->codec_tag = tag;
        if (ost->enc_ctx)
            ost->enc_ctx->codec_tag = tag;
    }

    MATCH_PER_STREAM_OPT(qscale, dbl, qscale, oc, st);
    if (ost->enc_ctx && qscale >= 0) {
        ost->enc_ctx->flags |= AV_CODEC_FLAG_QSCALE;
        ost->enc_ctx->global_quality = FF_QP2LAMBDA * qscale;
    }

    MATCH_PER_STREAM_OPT(disposition, str, ost->disposition, oc, st);
    ost->disposition = av_strdup(ost->disposition);

    ms->max_muxing_queue_size = 128;
    MATCH_PER_STREAM_OPT(max_muxing_queue_size, i, ms->max_muxing_queue_size, oc, st);

    ms->muxing_queue_data_threshold = 50*1024*1024;
    MATCH_PER_STREAM_OPT(muxing_queue_data_threshold, i, ms->muxing_queue_data_threshold, oc, st);

    MATCH_PER_STREAM_OPT(bits_per_raw_sample, i, ost->bits_per_raw_sample,
                         oc, st);

    if (oc->oformat->flags & AVFMT_GLOBALHEADER && ost->enc_ctx)
        ost->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    av_dict_copy(&ost->sws_dict, o->g->sws_dict, 0);

    av_dict_copy(&ost->swr_opts, o->g->swr_opts, 0);
    if (ost->enc_ctx && av_get_exact_bits_per_sample(ost->enc_ctx->codec_id) == 24)
        av_dict_set(&ost->swr_opts, "output_sample_bits", "24", 0);

    ost->source_index = source_index;
    if (source_index >= 0) {
        input_streams[source_index]->discard = 0;
        input_streams[source_index]->st->discard = input_streams[source_index]->user_set_discard;
    }
    ost->last_mux_dts = AV_NOPTS_VALUE;
    ost->last_filter_pts = AV_NOPTS_VALUE;

    MATCH_PER_STREAM_OPT(copy_initial_nonkeyframes, i,
                         ost->copy_initial_nonkeyframes, oc, st);

    return ost;
}

static char *get_ost_filters(OptionsContext *o, AVFormatContext *oc,
                             OutputStream *ost)
{
    AVStream *st = ost->st;

    if (ost->filters_script && ost->filters) {
        av_log(NULL, AV_LOG_ERROR, "Both -filter and -filter_script set for "
               "output stream #%d:%d.\n", nb_output_files, st->index);
        exit_program(1);
    }

    if (ost->filters_script)
        return file_read(ost->filters_script);
    else if (ost->filters)
        return av_strdup(ost->filters);

    return av_strdup(st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ?
                     "null" : "anull");
}

static void check_streamcopy_filters(OptionsContext *o, AVFormatContext *oc,
                                     const OutputStream *ost, enum AVMediaType type)
{
    if (ost->filters_script || ost->filters) {
        av_log(NULL, AV_LOG_ERROR,
               "%s '%s' was defined for %s output stream %d:%d but codec copy was selected.\n"
               "Filtering and streamcopy cannot be used together.\n",
               ost->filters ? "Filtergraph" : "Filtergraph script",
               ost->filters ? ost->filters : ost->filters_script,
               av_get_media_type_string(type), ost->file_index, ost->index);
        exit_program(1);
    }
}

static void parse_matrix_coeffs(uint16_t *dest, const char *str)
{
    int i;
    const char *p = str;
    for (i = 0;; i++) {
        dest[i] = atoi(p);
        if (i == 63)
            break;
        p = strchr(p, ',');
        if (!p) {
            av_log(NULL, AV_LOG_FATAL, "Syntax error in matrix \"%s\" at coeff %d\n", str, i);
            exit_program(1);
        }
        p++;
    }
}

static OutputStream *new_video_stream(Muxer *mux, OptionsContext *o, int source_index)
{
    AVFormatContext *oc = mux->fc;
    AVStream *st;
    OutputStream *ost;
    char *frame_rate = NULL, *max_frame_rate = NULL, *frame_aspect_ratio = NULL;

    ost = new_output_stream(mux, o, AVMEDIA_TYPE_VIDEO, source_index);
    st  = ost->st;

    MATCH_PER_STREAM_OPT(frame_rates, str, frame_rate, oc, st);
    if (frame_rate && av_parse_video_rate(&ost->frame_rate, frame_rate) < 0) {
        av_log(NULL, AV_LOG_FATAL, "Invalid framerate value: %s\n", frame_rate);
        exit_program(1);
    }

    MATCH_PER_STREAM_OPT(max_frame_rates, str, max_frame_rate, oc, st);
    if (max_frame_rate && av_parse_video_rate(&ost->max_frame_rate, max_frame_rate) < 0) {
        av_log(NULL, AV_LOG_FATAL, "Invalid maximum framerate value: %s\n", max_frame_rate);
        exit_program(1);
    }

    if (frame_rate && max_frame_rate) {
        av_log(NULL, AV_LOG_ERROR, "Only one of -fpsmax and -r can be set for a stream.\n");
        exit_program(1);
    }

    if ((frame_rate || max_frame_rate) &&
        video_sync_method == VSYNC_PASSTHROUGH)
        av_log(NULL, AV_LOG_ERROR, "Using -vsync passthrough and -r/-fpsmax can produce invalid output files\n");

    MATCH_PER_STREAM_OPT(frame_aspect_ratios, str, frame_aspect_ratio, oc, st);
    if (frame_aspect_ratio) {
        AVRational q;
        if (av_parse_ratio(&q, frame_aspect_ratio, 255, 0, NULL) < 0 ||
            q.num <= 0 || q.den <= 0) {
            av_log(NULL, AV_LOG_FATAL, "Invalid aspect ratio: %s\n", frame_aspect_ratio);
            exit_program(1);
        }
        ost->frame_aspect_ratio = q;
    }

    MATCH_PER_STREAM_OPT(filter_scripts, str, ost->filters_script, oc, st);
    MATCH_PER_STREAM_OPT(filters,        str, ost->filters,        oc, st);

    if (ost->enc_ctx) {
        AVCodecContext *video_enc = ost->enc_ctx;
        const char *p = NULL, *fps_mode = NULL;
        char *frame_size = NULL;
        char *frame_pix_fmt = NULL;
        char *intra_matrix = NULL, *inter_matrix = NULL;
        char *chroma_intra_matrix = NULL;
        int do_pass = 0;
        int i;

        MATCH_PER_STREAM_OPT(frame_sizes, str, frame_size, oc, st);
        if (frame_size && av_parse_video_size(&video_enc->width, &video_enc->height, frame_size) < 0) {
            av_log(NULL, AV_LOG_FATAL, "Invalid frame size: %s.\n", frame_size);
            exit_program(1);
        }

        MATCH_PER_STREAM_OPT(frame_pix_fmts, str, frame_pix_fmt, oc, st);
        if (frame_pix_fmt && *frame_pix_fmt == '+') {
            ost->keep_pix_fmt = 1;
            if (!*++frame_pix_fmt)
                frame_pix_fmt = NULL;
        }
        if (frame_pix_fmt && (video_enc->pix_fmt = av_get_pix_fmt(frame_pix_fmt)) == AV_PIX_FMT_NONE) {
            av_log(NULL, AV_LOG_FATAL, "Unknown pixel format requested: %s.\n", frame_pix_fmt);
            exit_program(1);
        }
        st->sample_aspect_ratio = video_enc->sample_aspect_ratio;

        MATCH_PER_STREAM_OPT(intra_matrices, str, intra_matrix, oc, st);
        if (intra_matrix) {
            if (!(video_enc->intra_matrix = av_mallocz(sizeof(*video_enc->intra_matrix) * 64)))
                report_and_exit(AVERROR(ENOMEM));
            parse_matrix_coeffs(video_enc->intra_matrix, intra_matrix);
        }
        MATCH_PER_STREAM_OPT(chroma_intra_matrices, str, chroma_intra_matrix, oc, st);
        if (chroma_intra_matrix) {
            uint16_t *p = av_mallocz(sizeof(*video_enc->chroma_intra_matrix) * 64);
            if (!p)
                report_and_exit(AVERROR(ENOMEM));
            video_enc->chroma_intra_matrix = p;
            parse_matrix_coeffs(p, chroma_intra_matrix);
        }
        MATCH_PER_STREAM_OPT(inter_matrices, str, inter_matrix, oc, st);
        if (inter_matrix) {
            if (!(video_enc->inter_matrix = av_mallocz(sizeof(*video_enc->inter_matrix) * 64)))
                report_and_exit(AVERROR(ENOMEM));
            parse_matrix_coeffs(video_enc->inter_matrix, inter_matrix);
        }

        MATCH_PER_STREAM_OPT(rc_overrides, str, p, oc, st);
        for (i = 0; p; i++) {
            int start, end, q;
            int e = sscanf(p, "%d,%d,%d", &start, &end, &q);
            if (e != 3) {
                av_log(NULL, AV_LOG_FATAL, "error parsing rc_override\n");
                exit_program(1);
            }
            video_enc->rc_override =
                av_realloc_array(video_enc->rc_override,
                                 i + 1, sizeof(RcOverride));
            if (!video_enc->rc_override) {
                av_log(NULL, AV_LOG_FATAL, "Could not (re)allocate memory for rc_override.\n");
                exit_program(1);
            }
            video_enc->rc_override[i].start_frame = start;
            video_enc->rc_override[i].end_frame   = end;
            if (q > 0) {
                video_enc->rc_override[i].qscale         = q;
                video_enc->rc_override[i].quality_factor = 1.0;
            }
            else {
                video_enc->rc_override[i].qscale         = 0;
                video_enc->rc_override[i].quality_factor = -q/100.0;
            }
            p = strchr(p, '/');
            if (p) p++;
        }
        video_enc->rc_override_count = i;

#if FFMPEG_OPT_PSNR
        if (do_psnr) {
            av_log(NULL, AV_LOG_WARNING, "The -psnr option is deprecated, use -flags +psnr\n");
            video_enc->flags|= AV_CODEC_FLAG_PSNR;
        }
#endif

        /* two pass mode */
        MATCH_PER_STREAM_OPT(pass, i, do_pass, oc, st);
        if (do_pass) {
            if (do_pass & 1) {
                video_enc->flags |= AV_CODEC_FLAG_PASS1;
                av_dict_set(&ost->encoder_opts, "flags", "+pass1", AV_DICT_APPEND);
            }
            if (do_pass & 2) {
                video_enc->flags |= AV_CODEC_FLAG_PASS2;
                av_dict_set(&ost->encoder_opts, "flags", "+pass2", AV_DICT_APPEND);
            }
        }

        MATCH_PER_STREAM_OPT(passlogfiles, str, ost->logfile_prefix, oc, st);
        if (ost->logfile_prefix &&
            !(ost->logfile_prefix = av_strdup(ost->logfile_prefix)))
            report_and_exit(AVERROR(ENOMEM));

        if (do_pass) {
            int ost_idx = -1;
            char logfilename[1024];
            FILE *f;

            /* compute this stream's global index */
            for (int i = 0; i <= ost->file_index; i++)
                ost_idx += output_files[i]->nb_streams;

            snprintf(logfilename, sizeof(logfilename), "%s-%d.log",
                     ost->logfile_prefix ? ost->logfile_prefix :
                                           DEFAULT_PASS_LOGFILENAME_PREFIX,
                     ost_idx);
            if (!strcmp(ost->enc_ctx->codec->name, "libx264")) {
                av_dict_set(&ost->encoder_opts, "stats", logfilename, AV_DICT_DONT_OVERWRITE);
            } else {
                if (video_enc->flags & AV_CODEC_FLAG_PASS2) {
                    char  *logbuffer = file_read(logfilename);

                    if (!logbuffer) {
                        av_log(NULL, AV_LOG_FATAL, "Error reading log file '%s' for pass-2 encoding\n",
                               logfilename);
                        exit_program(1);
                    }
                    video_enc->stats_in = logbuffer;
                }
                if (video_enc->flags & AV_CODEC_FLAG_PASS1) {
                    f = fopen_utf8(logfilename, "wb");
                    if (!f) {
                        av_log(NULL, AV_LOG_FATAL,
                               "Cannot write log file '%s' for pass-1 encoding: %s\n",
                               logfilename, strerror(errno));
                        exit_program(1);
                    }
                    ost->logfile = f;
                }
            }
        }

        MATCH_PER_STREAM_OPT(forced_key_frames, str, ost->forced_keyframes, oc, st);
        if (ost->forced_keyframes)
            ost->forced_keyframes = av_strdup(ost->forced_keyframes);

        MATCH_PER_STREAM_OPT(force_fps, i, ost->force_fps, oc, st);

        ost->top_field_first = -1;
        MATCH_PER_STREAM_OPT(top_field_first, i, ost->top_field_first, oc, st);

        ost->vsync_method = video_sync_method;
        MATCH_PER_STREAM_OPT(fps_mode, str, fps_mode, oc, st);
        if (fps_mode)
            parse_and_set_vsync(fps_mode, &ost->vsync_method, ost->file_index, ost->index, 0);

        if (ost->vsync_method == VSYNC_AUTO) {
            if (!strcmp(oc->oformat->name, "avi")) {
                ost->vsync_method = VSYNC_VFR;
            } else {
                ost->vsync_method = (oc->oformat->flags & AVFMT_VARIABLE_FPS)       ?
                                     ((oc->oformat->flags & AVFMT_NOTIMESTAMPS) ?
                                      VSYNC_PASSTHROUGH : VSYNC_VFR)                :
                                     VSYNC_CFR;
            }

            if (ost->source_index >= 0 && ost->vsync_method == VSYNC_CFR) {
                const InputStream *ist = input_streams[ost->source_index];
                const InputFile *ifile = input_files[ist->file_index];

                if (ifile->nb_streams == 1 && ifile->input_ts_offset == 0)
                    ost->vsync_method = VSYNC_VSCFR;
            }

            if (ost->vsync_method == VSYNC_CFR && copy_ts) {
                ost->vsync_method = VSYNC_VSCFR;
            }
        }
        ost->is_cfr = (ost->vsync_method == VSYNC_CFR || ost->vsync_method == VSYNC_VSCFR);

        ost->avfilter = get_ost_filters(o, oc, ost);
        if (!ost->avfilter)
            exit_program(1);

        ost->last_frame = av_frame_alloc();
        if (!ost->last_frame)
            report_and_exit(AVERROR(ENOMEM));
    } else
        check_streamcopy_filters(o, oc, ost, AVMEDIA_TYPE_VIDEO);

    return ost;
}

static OutputStream *new_audio_stream(Muxer *mux, OptionsContext *o, int source_index)
{
    AVFormatContext *oc = mux->fc;
    AVStream *st;
    OutputStream *ost;

    ost = new_output_stream(mux, o, AVMEDIA_TYPE_AUDIO, source_index);
    st  = ost->st;


    MATCH_PER_STREAM_OPT(filter_scripts, str, ost->filters_script, oc, st);
    MATCH_PER_STREAM_OPT(filters,        str, ost->filters,        oc, st);

    if (ost->enc_ctx) {
        AVCodecContext *audio_enc = ost->enc_ctx;
        int channels = 0;
        char *layout = NULL;
        char *sample_fmt = NULL;

        MATCH_PER_STREAM_OPT(audio_channels, i, channels, oc, st);
        if (channels) {
            audio_enc->ch_layout.order       = AV_CHANNEL_ORDER_UNSPEC;
            audio_enc->ch_layout.nb_channels = channels;
        }

        MATCH_PER_STREAM_OPT(audio_ch_layouts, str, layout, oc, st);
        if (layout) {
            if (av_channel_layout_from_string(&audio_enc->ch_layout, layout) < 0) {
#if FF_API_OLD_CHANNEL_LAYOUT
                uint64_t mask;
                AV_NOWARN_DEPRECATED({
                mask = av_get_channel_layout(layout);
                })
                if (!mask) {
#endif
                    av_log(NULL, AV_LOG_FATAL, "Unknown channel layout: %s\n", layout);
                    exit_program(1);
#if FF_API_OLD_CHANNEL_LAYOUT
                }
                av_log(NULL, AV_LOG_WARNING, "Channel layout '%s' uses a deprecated syntax.\n",
                       layout);
                av_channel_layout_from_mask(&audio_enc->ch_layout, mask);
#endif
            }
        }

        MATCH_PER_STREAM_OPT(sample_fmts, str, sample_fmt, oc, st);
        if (sample_fmt &&
            (audio_enc->sample_fmt = av_get_sample_fmt(sample_fmt)) == AV_SAMPLE_FMT_NONE) {
            av_log(NULL, AV_LOG_FATAL, "Invalid sample format '%s'\n", sample_fmt);
            exit_program(1);
        }

        MATCH_PER_STREAM_OPT(audio_sample_rate, i, audio_enc->sample_rate, oc, st);

        MATCH_PER_STREAM_OPT(apad, str, ost->apad, oc, st);
        ost->apad = av_strdup(ost->apad);

        ost->avfilter = get_ost_filters(o, oc, ost);
        if (!ost->avfilter)
            exit_program(1);

#if FFMPEG_OPT_MAP_CHANNEL
        /* check for channel mapping for this audio stream */
        for (int n = 0; n < o->nb_audio_channel_maps; n++) {
            AudioChannelMap *map = &o->audio_channel_maps[n];
            if ((map->ofile_idx   == -1 || ost->file_index == map->ofile_idx) &&
                (map->ostream_idx == -1 || ost->st->index  == map->ostream_idx)) {
                InputStream *ist;

                if (map->channel_idx == -1) {
                    ist = NULL;
                } else if (ost->source_index < 0) {
                    av_log(NULL, AV_LOG_FATAL, "Cannot determine input stream for channel mapping %d.%d\n",
                           ost->file_index, ost->st->index);
                    continue;
                } else {
                    ist = input_streams[ost->source_index];
                }

                if (!ist || (ist->file_index == map->file_idx && ist->st->index == map->stream_idx)) {
                    if (av_reallocp_array(&ost->audio_channels_map,
                                          ost->audio_channels_mapped + 1,
                                          sizeof(*ost->audio_channels_map)
                                          ) < 0 )
                        report_and_exit(AVERROR(ENOMEM));

                    ost->audio_channels_map[ost->audio_channels_mapped++] = map->channel_idx;
                }
            }
        }
#endif
    } else
        check_streamcopy_filters(o, oc, ost, AVMEDIA_TYPE_AUDIO);

    return ost;
}

static OutputStream *new_data_stream(Muxer *mux, OptionsContext *o, int source_index)
{
    OutputStream *ost;

    ost = new_output_stream(mux, o, AVMEDIA_TYPE_DATA, source_index);
    if (ost->enc_ctx) {
        av_log(NULL, AV_LOG_FATAL, "Data stream encoding not supported yet (only streamcopy)\n");
        exit_program(1);
    }

    return ost;
}

static OutputStream *new_unknown_stream(Muxer *mux, OptionsContext *o, int source_index)
{
    OutputStream *ost;

    ost = new_output_stream(mux, o, AVMEDIA_TYPE_UNKNOWN, source_index);
    if (ost->enc_ctx) {
        av_log(NULL, AV_LOG_FATAL, "Unknown stream encoding not supported yet (only streamcopy)\n");
        exit_program(1);
    }

    return ost;
}

static OutputStream *new_attachment_stream(Muxer *mux, OptionsContext *o, int source_index)
{
    OutputStream *ost = new_output_stream(mux, o, AVMEDIA_TYPE_ATTACHMENT, source_index);
    ost->finished    = 1;
    return ost;
}

static OutputStream *new_subtitle_stream(Muxer *mux, OptionsContext *o, int source_index)
{
    AVStream *st;
    OutputStream *ost;

    ost = new_output_stream(mux, o, AVMEDIA_TYPE_SUBTITLE, source_index);
    st  = ost->st;

    if (ost->enc_ctx) {
        AVCodecContext *subtitle_enc = ost->enc_ctx;
        char *frame_size = NULL;

        MATCH_PER_STREAM_OPT(frame_sizes, str, frame_size, mux->fc, st);
        if (frame_size && av_parse_video_size(&subtitle_enc->width, &subtitle_enc->height, frame_size) < 0) {
            av_log(NULL, AV_LOG_FATAL, "Invalid frame size: %s.\n", frame_size);
            exit_program(1);
        }
    }

    return ost;
}

static void init_output_filter(OutputFilter *ofilter, OptionsContext *o,
                               Muxer *mux)
{
    OutputStream *ost;

    switch (ofilter->type) {
    case AVMEDIA_TYPE_VIDEO: ost = new_video_stream(mux, o, -1); break;
    case AVMEDIA_TYPE_AUDIO: ost = new_audio_stream(mux, o, -1); break;
    default:
        av_log(NULL, AV_LOG_FATAL, "Only video and audio filters are supported "
               "currently.\n");
        exit_program(1);
    }

    ost->filter       = ofilter;

    ofilter->ost      = ost;
    ofilter->format   = -1;

    if (!ost->enc_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Streamcopy requested for output stream %d:%d, "
               "which is fed from a complex filtergraph. Filtering and streamcopy "
               "cannot be used together.\n", ost->file_index, ost->index);
        exit_program(1);
    }

    if (ost->avfilter && (ost->filters || ost->filters_script)) {
        const char *opt = ost->filters ? "-vf/-af/-filter" : "-filter_script";
        av_log(NULL, AV_LOG_ERROR,
               "%s '%s' was specified through the %s option "
               "for output stream %d:%d, which is fed from a complex filtergraph.\n"
               "%s and -filter_complex cannot be used together for the same stream.\n",
               ost->filters ? "Filtergraph" : "Filtergraph script",
               ost->filters ? ost->filters : ost->filters_script,
               opt, ost->file_index, ost->index, opt);
        exit_program(1);
    }

    avfilter_inout_free(&ofilter->out_tmp);
}

static void map_auto_video(Muxer *mux, OptionsContext *o)
{
    AVFormatContext *oc = mux->fc;
    InputStream *ist;
    int best_score = 0, idx = -1;
    int qcr;

    /* video: highest resolution */
    if (av_guess_codec(oc->oformat, NULL, oc->url, NULL, AVMEDIA_TYPE_VIDEO) == AV_CODEC_ID_NONE)
        return;

    qcr = avformat_query_codec(oc->oformat, oc->oformat->video_codec, 0);
    for (int j = 0; j < nb_input_files; j++) {
        InputFile *ifile = input_files[j];
        int file_best_score = 0, file_best_idx = -1;
        for (int i = 0; i < ifile->nb_streams; i++) {
            int score;
            ist = input_streams[ifile->ist_index + i];
            score = ist->st->codecpar->width * ist->st->codecpar->height
                       + 100000000 * !!(ist->st->event_flags & AVSTREAM_EVENT_FLAG_NEW_PACKETS)
                       + 5000000*!!(ist->st->disposition & AV_DISPOSITION_DEFAULT);
            if (ist->user_set_discard == AVDISCARD_ALL)
                continue;
            if((qcr!=MKTAG('A', 'P', 'I', 'C')) && (ist->st->disposition & AV_DISPOSITION_ATTACHED_PIC))
                score = 1;
            if (ist->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                score > file_best_score) {
                if((qcr==MKTAG('A', 'P', 'I', 'C')) && !(ist->st->disposition & AV_DISPOSITION_ATTACHED_PIC))
                    continue;
                file_best_score = score;
                file_best_idx = ifile->ist_index + i;
            }
        }
        if (file_best_idx >= 0) {
            if((qcr == MKTAG('A', 'P', 'I', 'C')) || !(ist->st->disposition & AV_DISPOSITION_ATTACHED_PIC))
                file_best_score -= 5000000*!!(input_streams[file_best_idx]->st->disposition & AV_DISPOSITION_DEFAULT);
            if (file_best_score > best_score) {
                best_score = file_best_score;
                idx = file_best_idx;
            }
       }
    }
    if (idx >= 0)
        new_video_stream(mux, o, idx);
}

static void map_auto_audio(Muxer *mux, OptionsContext *o)
{
    AVFormatContext *oc = mux->fc;
    InputStream *ist;
    int best_score = 0, idx = -1;

        /* audio: most channels */
    if (av_guess_codec(oc->oformat, NULL, oc->url, NULL, AVMEDIA_TYPE_AUDIO) == AV_CODEC_ID_NONE)
        return;

    for (int j = 0; j < nb_input_files; j++) {
        InputFile *ifile = input_files[j];
        int file_best_score = 0, file_best_idx = -1;
        for (int i = 0; i < ifile->nb_streams; i++) {
            int score;
            ist = input_streams[ifile->ist_index + i];
            score = ist->st->codecpar->ch_layout.nb_channels
                    + 100000000 * !!(ist->st->event_flags & AVSTREAM_EVENT_FLAG_NEW_PACKETS)
                    + 5000000*!!(ist->st->disposition & AV_DISPOSITION_DEFAULT);
            if (ist->user_set_discard == AVDISCARD_ALL)
                continue;
            if (ist->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                score > file_best_score) {
                file_best_score = score;
                file_best_idx = ifile->ist_index + i;
            }
        }
        if (file_best_idx >= 0) {
            file_best_score -= 5000000*!!(input_streams[file_best_idx]->st->disposition & AV_DISPOSITION_DEFAULT);
            if (file_best_score > best_score) {
                best_score = file_best_score;
                idx = file_best_idx;
            }
       }
    }
    if (idx >= 0)
        new_audio_stream(mux, o, idx);
}

static void map_auto_subtitle(Muxer *mux, OptionsContext *o)
{
    AVFormatContext *oc = mux->fc;
    char *subtitle_codec_name = NULL;

        /* subtitles: pick first */
    MATCH_PER_TYPE_OPT(codec_names, str, subtitle_codec_name, oc, "s");
    if (!avcodec_find_encoder(oc->oformat->subtitle_codec) && !subtitle_codec_name)
        return;

    for (int i = 0; i < nb_input_streams; i++)
        if (input_streams[i]->st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            AVCodecDescriptor const *input_descriptor =
                avcodec_descriptor_get(input_streams[i]->st->codecpar->codec_id);
            AVCodecDescriptor const *output_descriptor = NULL;
            AVCodec const *output_codec =
                avcodec_find_encoder(oc->oformat->subtitle_codec);
            int input_props = 0, output_props = 0;
            if (input_streams[i]->user_set_discard == AVDISCARD_ALL)
                continue;
            if (output_codec)
                output_descriptor = avcodec_descriptor_get(output_codec->id);
            if (input_descriptor)
                input_props = input_descriptor->props & (AV_CODEC_PROP_TEXT_SUB | AV_CODEC_PROP_BITMAP_SUB);
            if (output_descriptor)
                output_props = output_descriptor->props & (AV_CODEC_PROP_TEXT_SUB | AV_CODEC_PROP_BITMAP_SUB);
            if (subtitle_codec_name ||
                input_props & output_props ||
                // Map dvb teletext which has neither property to any output subtitle encoder
                input_descriptor && output_descriptor &&
                (!input_descriptor->props ||
                 !output_descriptor->props)) {
                new_subtitle_stream(mux, o, i);
                break;
            }
        }
}

static void map_auto_data(Muxer *mux, OptionsContext *o)
{
    AVFormatContext *oc = mux->fc;
    /* Data only if codec id match */
    enum AVCodecID codec_id = av_guess_codec(oc->oformat, NULL, oc->url, NULL, AVMEDIA_TYPE_DATA);
    for (int i = 0; codec_id != AV_CODEC_ID_NONE && i < nb_input_streams; i++) {
        if (input_streams[i]->user_set_discard == AVDISCARD_ALL)
            continue;
        if (input_streams[i]->st->codecpar->codec_type == AVMEDIA_TYPE_DATA
            && input_streams[i]->st->codecpar->codec_id == codec_id )
            new_data_stream(mux, o, i);
    }
}

static void map_manual(Muxer *mux, OptionsContext *o, const StreamMap *map)
{
    InputStream *ist;

    if (map->disabled)
        return;

    if (map->linklabel) {
        FilterGraph *fg;
        OutputFilter *ofilter = NULL;
        int j, k;

        for (j = 0; j < nb_filtergraphs; j++) {
            fg = filtergraphs[j];
            for (k = 0; k < fg->nb_outputs; k++) {
                AVFilterInOut *out = fg->outputs[k]->out_tmp;
                if (out && !strcmp(out->name, map->linklabel)) {
                    ofilter = fg->outputs[k];
                    goto loop_end;
                }
            }
        }
loop_end:
        if (!ofilter) {
            av_log(NULL, AV_LOG_FATAL, "Output with label '%s' does not exist "
                   "in any defined filter graph, or was already used elsewhere.\n", map->linklabel);
            exit_program(1);
        }
        init_output_filter(ofilter, o, mux);
    } else {
        int src_idx = input_files[map->file_index]->ist_index + map->stream_index;

        ist = input_streams[input_files[map->file_index]->ist_index + map->stream_index];
        if (ist->user_set_discard == AVDISCARD_ALL) {
            av_log(NULL, AV_LOG_FATAL, "Stream #%d:%d is disabled and cannot be mapped.\n",
                   map->file_index, map->stream_index);
            exit_program(1);
        }
        if(o->subtitle_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
            return;
        if(o->   audio_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            return;
        if(o->   video_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            return;
        if(o->    data_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_DATA)
            return;

        switch (ist->st->codecpar->codec_type) {
        case AVMEDIA_TYPE_VIDEO:      new_video_stream     (mux, o, src_idx); break;
        case AVMEDIA_TYPE_AUDIO:      new_audio_stream     (mux, o, src_idx); break;
        case AVMEDIA_TYPE_SUBTITLE:   new_subtitle_stream  (mux, o, src_idx); break;
        case AVMEDIA_TYPE_DATA:       new_data_stream      (mux, o, src_idx); break;
        case AVMEDIA_TYPE_ATTACHMENT: new_attachment_stream(mux, o, src_idx); break;
        case AVMEDIA_TYPE_UNKNOWN:
            if (copy_unknown_streams) {
                new_unknown_stream   (mux, o, src_idx);
                break;
            }
        default:
            av_log(NULL, ignore_unknown_streams ? AV_LOG_WARNING : AV_LOG_FATAL,
                   "Cannot map stream #%d:%d - unsupported type.\n",
                   map->file_index, map->stream_index);
            if (!ignore_unknown_streams) {
                av_log(NULL, AV_LOG_FATAL,
                       "If you want unsupported types ignored instead "
                       "of failing, please use the -ignore_unknown option\n"
                       "If you want them copied, please use -copy_unknown\n");
                exit_program(1);
            }
        }
    }
}

static int setup_sync_queues(Muxer *mux, AVFormatContext *oc, int64_t buf_size_us)
{
    OutputFile *of = &mux->of;
    int nb_av_enc = 0, nb_interleaved = 0;
    int limit_frames = 0, limit_frames_av_enc = 0;

#define IS_AV_ENC(ost, type)  \
    (ost->enc_ctx && (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO))
#define IS_INTERLEAVED(type) (type != AVMEDIA_TYPE_ATTACHMENT)

    for (int i = 0; i < oc->nb_streams; i++) {
        OutputStream *ost = of->streams[i];
        enum AVMediaType type = ost->st->codecpar->codec_type;

        ost->sq_idx_encode = -1;
        ost->sq_idx_mux    = -1;

        nb_interleaved += IS_INTERLEAVED(type);
        nb_av_enc      += IS_AV_ENC(ost, type);

        limit_frames        |=  ost->max_frames < INT64_MAX;
        limit_frames_av_enc |= (ost->max_frames < INT64_MAX) && IS_AV_ENC(ost, type);
    }

    if (!((nb_interleaved > 1 && of->shortest) ||
          (nb_interleaved > 0 && limit_frames)))
        return 0;

    /* if we have more than one encoded audio/video streams, or at least
     * one encoded audio/video stream is frame-limited, then we
     * synchronize them before encoding */
    if ((of->shortest && nb_av_enc > 1) || limit_frames_av_enc) {
        of->sq_encode = sq_alloc(SYNC_QUEUE_FRAMES, buf_size_us);
        if (!of->sq_encode)
            return AVERROR(ENOMEM);

        for (int i = 0; i < oc->nb_streams; i++) {
            OutputStream *ost = of->streams[i];
            enum AVMediaType type = ost->st->codecpar->codec_type;

            if (!IS_AV_ENC(ost, type))
                continue;

            ost->sq_idx_encode = sq_add_stream(of->sq_encode,
                                               of->shortest || ost->max_frames < INT64_MAX);
            if (ost->sq_idx_encode < 0)
                return ost->sq_idx_encode;

            ost->sq_frame = av_frame_alloc();
            if (!ost->sq_frame)
                return AVERROR(ENOMEM);

            if (ost->max_frames != INT64_MAX)
                sq_limit_frames(of->sq_encode, ost->sq_idx_encode, ost->max_frames);
        }
    }

    /* if there are any additional interleaved streams, then ALL the streams
     * are also synchronized before sending them to the muxer */
    if (nb_interleaved > nb_av_enc) {
        mux->sq_mux = sq_alloc(SYNC_QUEUE_PACKETS, buf_size_us);
        if (!mux->sq_mux)
            return AVERROR(ENOMEM);

        mux->sq_pkt = av_packet_alloc();
        if (!mux->sq_pkt)
            return AVERROR(ENOMEM);

        for (int i = 0; i < oc->nb_streams; i++) {
            OutputStream *ost = of->streams[i];
            enum AVMediaType type = ost->st->codecpar->codec_type;

            if (!IS_INTERLEAVED(type))
                continue;

            ost->sq_idx_mux = sq_add_stream(mux->sq_mux,
                                            of->shortest || ost->max_frames < INT64_MAX);
            if (ost->sq_idx_mux < 0)
                return ost->sq_idx_mux;

            if (ost->max_frames != INT64_MAX)
                sq_limit_frames(mux->sq_mux, ost->sq_idx_mux, ost->max_frames);
        }
    }

#undef IS_AV_ENC
#undef IS_INTERLEAVED

    return 0;
}

static void of_add_attachments(Muxer *mux, OptionsContext *o)
{
    OutputStream *ost;
    int err;

    for (int i = 0; i < o->nb_attachments; i++) {
        AVIOContext *pb;
        uint8_t *attachment;
        const char *p;
        int64_t len;

        if ((err = avio_open2(&pb, o->attachments[i], AVIO_FLAG_READ, &int_cb, NULL)) < 0) {
            av_log(NULL, AV_LOG_FATAL, "Could not open attachment file %s.\n",
                   o->attachments[i]);
            exit_program(1);
        }
        if ((len = avio_size(pb)) <= 0) {
            av_log(NULL, AV_LOG_FATAL, "Could not get size of the attachment %s.\n",
                   o->attachments[i]);
            exit_program(1);
        }
        if (len > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE ||
            !(attachment = av_malloc(len + AV_INPUT_BUFFER_PADDING_SIZE))) {
            av_log(NULL, AV_LOG_FATAL, "Attachment %s too large.\n",
                   o->attachments[i]);
            exit_program(1);
        }
        avio_read(pb, attachment, len);
        memset(attachment + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);

        ost = new_attachment_stream(mux, o, -1);
        ost->attachment_filename       = o->attachments[i];
        ost->st->codecpar->extradata      = attachment;
        ost->st->codecpar->extradata_size = len;

        p = strrchr(o->attachments[i], '/');
        av_dict_set(&ost->st->metadata, "filename", (p && *p) ? p + 1 : o->attachments[i], AV_DICT_DONT_OVERWRITE);
        avio_closep(&pb);
    }
}

static void of_add_programs(AVFormatContext *oc, const OptionsContext *o)
{
    /* process manually set programs */
    for (int i = 0; i < o->nb_program; i++) {
        const char *p = o->program[i].u.str;
        int progid = i+1;
        AVProgram *program;

        while(*p) {
            const char *p2 = av_get_token(&p, ":");
            const char *to_dealloc = p2;
            char *key;
            if (!p2)
                break;

            if(*p) p++;

            key = av_get_token(&p2, "=");
            if (!key || !*p2) {
                av_freep(&to_dealloc);
                av_freep(&key);
                break;
            }
            p2++;

            if (!strcmp(key, "program_num"))
                progid = strtol(p2, NULL, 0);
            av_freep(&to_dealloc);
            av_freep(&key);
        }

        program = av_new_program(oc, progid);
        if (!program)
            report_and_exit(AVERROR(ENOMEM));

        p = o->program[i].u.str;
        while(*p) {
            const char *p2 = av_get_token(&p, ":");
            const char *to_dealloc = p2;
            char *key;
            if (!p2)
                break;
            if(*p) p++;

            key = av_get_token(&p2, "=");
            if (!key) {
                av_log(NULL, AV_LOG_FATAL,
                       "No '=' character in program string %s.\n",
                       p2);
                exit_program(1);
            }
            if (!*p2)
                exit_program(1);
            p2++;

            if (!strcmp(key, "title")) {
                av_dict_set(&program->metadata, "title", p2, 0);
            } else if (!strcmp(key, "program_num")) {
            } else if (!strcmp(key, "st")) {
                int st_num = strtol(p2, NULL, 0);
                av_program_add_stream_index(oc, progid, st_num);
            } else {
                av_log(NULL, AV_LOG_FATAL, "Unknown program key %s.\n", key);
                exit_program(1);
            }
            av_freep(&to_dealloc);
            av_freep(&key);
        }
    }
}

/**
 * Parse a metadata specifier passed as 'arg' parameter.
 * @param arg  metadata string to parse
 * @param type metadata type is written here -- g(lobal)/s(tream)/c(hapter)/p(rogram)
 * @param index for type c/p, chapter/program index is written here
 * @param stream_spec for type s, the stream specifier is written here
 */
static void parse_meta_type(char *arg, char *type, int *index, const char **stream_spec)
{
    if (*arg) {
        *type = *arg;
        switch (*arg) {
        case 'g':
            break;
        case 's':
            if (*(++arg) && *arg != ':') {
                av_log(NULL, AV_LOG_FATAL, "Invalid metadata specifier %s.\n", arg);
                exit_program(1);
            }
            *stream_spec = *arg == ':' ? arg + 1 : "";
            break;
        case 'c':
        case 'p':
            if (*(++arg) == ':')
                *index = strtol(++arg, NULL, 0);
            break;
        default:
            av_log(NULL, AV_LOG_FATAL, "Invalid metadata type %c.\n", *arg);
            exit_program(1);
        }
    } else
        *type = 'g';
}

static void of_add_metadata(OutputFile *of, AVFormatContext *oc,
                            const OptionsContext *o)
{
    for (int i = 0; i < o->nb_metadata; i++) {
        AVDictionary **m;
        char type, *val;
        const char *stream_spec;
        int index = 0, ret = 0;

        val = strchr(o->metadata[i].u.str, '=');
        if (!val) {
            av_log(NULL, AV_LOG_FATAL, "No '=' character in metadata string %s.\n",
                   o->metadata[i].u.str);
            exit_program(1);
        }
        *val++ = 0;

        parse_meta_type(o->metadata[i].specifier, &type, &index, &stream_spec);
        if (type == 's') {
            for (int j = 0; j < oc->nb_streams; j++) {
                OutputStream *ost = of->streams[j];
                if ((ret = check_stream_specifier(oc, oc->streams[j], stream_spec)) > 0) {
#if FFMPEG_ROTATION_METADATA
                    if (!strcmp(o->metadata[i].u.str, "rotate")) {
                        char *tail;
                        double theta = av_strtod(val, &tail);
                        if (!*tail) {
                            ost->rotate_overridden = 1;
                            ost->rotate_override_value = theta;
                        }

                        av_log(NULL, AV_LOG_WARNING,
                               "Conversion of a 'rotate' metadata key to a "
                               "proper display matrix rotation is deprecated. "
                               "See -display_rotation for setting rotation "
                               "instead.");
                    } else {
#endif
                        av_dict_set(&oc->streams[j]->metadata, o->metadata[i].u.str, *val ? val : NULL, 0);
#if FFMPEG_ROTATION_METADATA
                    }
#endif
                } else if (ret < 0)
                    exit_program(1);
            }
        } else {
            switch (type) {
            case 'g':
                m = &oc->metadata;
                break;
            case 'c':
                if (index < 0 || index >= oc->nb_chapters) {
                    av_log(NULL, AV_LOG_FATAL, "Invalid chapter index %d in metadata specifier.\n", index);
                    exit_program(1);
                }
                m = &oc->chapters[index]->metadata;
                break;
            case 'p':
                if (index < 0 || index >= oc->nb_programs) {
                    av_log(NULL, AV_LOG_FATAL, "Invalid program index %d in metadata specifier.\n", index);
                    exit_program(1);
                }
                m = &oc->programs[index]->metadata;
                break;
            default:
                av_log(NULL, AV_LOG_FATAL, "Invalid metadata specifier %s.\n", o->metadata[i].specifier);
                exit_program(1);
            }
            av_dict_set(m, o->metadata[i].u.str, *val ? val : NULL, 0);
        }
    }
}

static void set_channel_layout(OutputFilter *f, OutputStream *ost)
{
    const AVCodec *c = ost->enc_ctx->codec;
    int i, err;

    if (ost->enc_ctx->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
        /* Pass the layout through for all orders but UNSPEC */
        err = av_channel_layout_copy(&f->ch_layout, &ost->enc_ctx->ch_layout);
        if (err < 0)
            report_and_exit(AVERROR(ENOMEM));
        return;
    }

    /* Requested layout is of order UNSPEC */
    if (!c->ch_layouts) {
        /* Use the default native layout for the requested amount of channels when the
           encoder doesn't have a list of supported layouts */
        av_channel_layout_default(&f->ch_layout, ost->enc_ctx->ch_layout.nb_channels);
        return;
    }
    /* Encoder has a list of supported layouts. Pick the first layout in it with the
       same amount of channels as the requested layout */
    for (i = 0; c->ch_layouts[i].nb_channels; i++) {
        if (c->ch_layouts[i].nb_channels == ost->enc_ctx->ch_layout.nb_channels)
            break;
    }
    if (c->ch_layouts[i].nb_channels) {
        /* Use it if one is found */
        err = av_channel_layout_copy(&f->ch_layout, &c->ch_layouts[i]);
        if (err < 0)
            report_and_exit(AVERROR(ENOMEM));
        return;
    }
    /* If no layout for the amount of channels requested was found, use the default
       native layout for it. */
    av_channel_layout_default(&f->ch_layout, ost->enc_ctx->ch_layout.nb_channels);
}

static int copy_chapters(InputFile *ifile, OutputFile *ofile, AVFormatContext *os,
                         int copy_metadata)
{
    AVFormatContext *is = ifile->ctx;
    AVChapter **tmp;
    int i;

    tmp = av_realloc_f(os->chapters, is->nb_chapters + os->nb_chapters, sizeof(*os->chapters));
    if (!tmp)
        return AVERROR(ENOMEM);
    os->chapters = tmp;

    for (i = 0; i < is->nb_chapters; i++) {
        AVChapter *in_ch = is->chapters[i], *out_ch;
        int64_t start_time = (ofile->start_time == AV_NOPTS_VALUE) ? 0 : ofile->start_time;
        int64_t ts_off   = av_rescale_q(start_time - ifile->ts_offset,
                                       AV_TIME_BASE_Q, in_ch->time_base);
        int64_t rt       = (ofile->recording_time == INT64_MAX) ? INT64_MAX :
                           av_rescale_q(ofile->recording_time, AV_TIME_BASE_Q, in_ch->time_base);


        if (in_ch->end < ts_off)
            continue;
        if (rt != INT64_MAX && in_ch->start > rt + ts_off)
            break;

        out_ch = av_mallocz(sizeof(AVChapter));
        if (!out_ch)
            return AVERROR(ENOMEM);

        out_ch->id        = in_ch->id;
        out_ch->time_base = in_ch->time_base;
        out_ch->start     = FFMAX(0,  in_ch->start - ts_off);
        out_ch->end       = FFMIN(rt, in_ch->end   - ts_off);

        if (copy_metadata)
            av_dict_copy(&out_ch->metadata, in_ch->metadata, 0);

        os->chapters[os->nb_chapters++] = out_ch;
    }
    return 0;
}

static int copy_metadata(char *outspec, char *inspec, AVFormatContext *oc, AVFormatContext *ic, OptionsContext *o)
{
    AVDictionary **meta_in = NULL;
    AVDictionary **meta_out = NULL;
    int i, ret = 0;
    char type_in, type_out;
    const char *istream_spec = NULL, *ostream_spec = NULL;
    int idx_in = 0, idx_out = 0;

    parse_meta_type(inspec,  &type_in,  &idx_in,  &istream_spec);
    parse_meta_type(outspec, &type_out, &idx_out, &ostream_spec);

    if (!ic) {
        if (type_out == 'g' || !*outspec)
            o->metadata_global_manual = 1;
        if (type_out == 's' || !*outspec)
            o->metadata_streams_manual = 1;
        if (type_out == 'c' || !*outspec)
            o->metadata_chapters_manual = 1;
        return 0;
    }

    if (type_in == 'g' || type_out == 'g')
        o->metadata_global_manual = 1;
    if (type_in == 's' || type_out == 's')
        o->metadata_streams_manual = 1;
    if (type_in == 'c' || type_out == 'c')
        o->metadata_chapters_manual = 1;

    /* ic is NULL when just disabling automatic mappings */
    if (!ic)
        return 0;

#define METADATA_CHECK_INDEX(index, nb_elems, desc)\
    if ((index) < 0 || (index) >= (nb_elems)) {\
        av_log(NULL, AV_LOG_FATAL, "Invalid %s index %d while processing metadata maps.\n",\
                (desc), (index));\
        exit_program(1);\
    }

#define SET_DICT(type, meta, context, index)\
        switch (type) {\
        case 'g':\
            meta = &context->metadata;\
            break;\
        case 'c':\
            METADATA_CHECK_INDEX(index, context->nb_chapters, "chapter")\
            meta = &context->chapters[index]->metadata;\
            break;\
        case 'p':\
            METADATA_CHECK_INDEX(index, context->nb_programs, "program")\
            meta = &context->programs[index]->metadata;\
            break;\
        case 's':\
            break; /* handled separately below */ \
        default: av_assert0(0);\
        }\

    SET_DICT(type_in, meta_in, ic, idx_in);
    SET_DICT(type_out, meta_out, oc, idx_out);

    /* for input streams choose first matching stream */
    if (type_in == 's') {
        for (i = 0; i < ic->nb_streams; i++) {
            if ((ret = check_stream_specifier(ic, ic->streams[i], istream_spec)) > 0) {
                meta_in = &ic->streams[i]->metadata;
                break;
            } else if (ret < 0)
                exit_program(1);
        }
        if (!meta_in) {
            av_log(NULL, AV_LOG_FATAL, "Stream specifier %s does not match  any streams.\n", istream_spec);
            exit_program(1);
        }
    }

    if (type_out == 's') {
        for (i = 0; i < oc->nb_streams; i++) {
            if ((ret = check_stream_specifier(oc, oc->streams[i], ostream_spec)) > 0) {
                meta_out = &oc->streams[i]->metadata;
                av_dict_copy(meta_out, *meta_in, AV_DICT_DONT_OVERWRITE);
            } else if (ret < 0)
                exit_program(1);
        }
    } else
        av_dict_copy(meta_out, *meta_in, AV_DICT_DONT_OVERWRITE);

    return 0;
}

static void copy_meta(Muxer *mux, OptionsContext *o)
{
    OutputFile      *of = &mux->of;
    AVFormatContext *oc = mux->fc;

    /* copy metadata */
    for (int i = 0; i < o->nb_metadata_map; i++) {
        char *p;
        int in_file_index = strtol(o->metadata_map[i].u.str, &p, 0);

        if (in_file_index >= nb_input_files) {
            av_log(NULL, AV_LOG_FATAL, "Invalid input file index %d while processing metadata maps\n", in_file_index);
            exit_program(1);
        }
        copy_metadata(o->metadata_map[i].specifier, *p ? p + 1 : p, oc,
                      in_file_index >= 0 ?
                      input_files[in_file_index]->ctx : NULL, o);
    }

    /* copy chapters */
    if (o->chapters_input_file >= nb_input_files) {
        if (o->chapters_input_file == INT_MAX) {
            /* copy chapters from the first input file that has them*/
            o->chapters_input_file = -1;
            for (int i = 0; i < nb_input_files; i++)
                if (input_files[i]->ctx->nb_chapters) {
                    o->chapters_input_file = i;
                    break;
                }
        } else {
            av_log(NULL, AV_LOG_FATAL, "Invalid input file index %d in chapter mapping.\n",
                   o->chapters_input_file);
            exit_program(1);
        }
    }
    if (o->chapters_input_file >= 0)
        copy_chapters(input_files[o->chapters_input_file], of, oc,
                      !o->metadata_chapters_manual);

    /* copy global metadata by default */
    if (!o->metadata_global_manual && nb_input_files){
        av_dict_copy(&oc->metadata, input_files[0]->ctx->metadata,
                     AV_DICT_DONT_OVERWRITE);
        if(o->recording_time != INT64_MAX)
            av_dict_set(&oc->metadata, "duration", NULL, 0);
        av_dict_set(&oc->metadata, "creation_time", NULL, 0);
        av_dict_set(&oc->metadata, "company_name", NULL, 0);
        av_dict_set(&oc->metadata, "product_name", NULL, 0);
        av_dict_set(&oc->metadata, "product_version", NULL, 0);
    }
    if (!o->metadata_streams_manual)
        for (int i = 0; i < of->nb_streams; i++) {
            OutputStream *ost = of->streams[i];
            InputStream *ist;
            if (ost->source_index < 0)         /* this is true e.g. for attached files */
                continue;
            ist = input_streams[ost->source_index];
            av_dict_copy(&ost->st->metadata, ist->st->metadata, AV_DICT_DONT_OVERWRITE);
            if (ost->enc_ctx) {
                av_dict_set(&ost->st->metadata, "encoder", NULL, 0);
            }
        }
}

static int set_dispositions(OutputFile *of, AVFormatContext *ctx)
{
    int nb_streams[AVMEDIA_TYPE_NB]   = { 0 };
    int have_default[AVMEDIA_TYPE_NB] = { 0 };
    int have_manual = 0;

    // first, copy the input dispositions
    for (int i = 0; i < ctx->nb_streams; i++) {
        OutputStream *ost = of->streams[i];

        nb_streams[ost->st->codecpar->codec_type]++;

        have_manual |= !!ost->disposition;

        if (ost->source_index >= 0) {
            ost->st->disposition = input_streams[ost->source_index]->st->disposition;

            if (ost->st->disposition & AV_DISPOSITION_DEFAULT)
                have_default[ost->st->codecpar->codec_type] = 1;
        }
    }

    if (have_manual) {
        // process manually set dispositions - they override the above copy
        for (int i = 0; i < ctx->nb_streams; i++) {
            OutputStream *ost = of->streams[i];
            int ret;

            if (!ost->disposition)
                continue;

#if LIBAVFORMAT_VERSION_MAJOR >= 60
            ret = av_opt_set(ost->st, "disposition", ost->disposition, 0);
#else
            {
                const AVClass *class = av_stream_get_class();
                const AVOption    *o = av_opt_find(&class, "disposition", NULL, 0, AV_OPT_SEARCH_FAKE_OBJ);

                av_assert0(o);
                ret = av_opt_eval_flags(&class, o, ost->disposition, &ost->st->disposition);
            }
#endif

            if (ret < 0)
                return ret;
        }
    } else {
        // For each media type with more than one stream, find a suitable stream to
        // mark as default, unless one is already marked default.
        // "Suitable" means the first of that type, skipping attached pictures.
        for (int i = 0; i < ctx->nb_streams; i++) {
            OutputStream *ost = of->streams[i];
            enum AVMediaType type = ost->st->codecpar->codec_type;

            if (nb_streams[type] < 2 || have_default[type] ||
                ost->st->disposition & AV_DISPOSITION_ATTACHED_PIC)
                continue;

            ost->st->disposition |= AV_DISPOSITION_DEFAULT;
            have_default[type] = 1;
        }
    }

    return 0;
}

int of_open(OptionsContext *o, const char *filename)
{
    Muxer *mux;
    AVFormatContext *oc;
    int i, j, err;
    OutputFile *of;
    AVDictionary *unused_opts = NULL;
    const AVDictionaryEntry *e = NULL;

    if (o->stop_time != INT64_MAX && o->recording_time != INT64_MAX) {
        o->stop_time = INT64_MAX;
        av_log(NULL, AV_LOG_WARNING, "-t and -to cannot be used together; using -t.\n");
    }

    if (o->stop_time != INT64_MAX && o->recording_time == INT64_MAX) {
        int64_t start_time = o->start_time == AV_NOPTS_VALUE ? 0 : o->start_time;
        if (o->stop_time <= start_time) {
            av_log(NULL, AV_LOG_ERROR, "-to value smaller than -ss; aborting.\n");
            exit_program(1);
        } else {
            o->recording_time = o->stop_time - start_time;
        }
    }

    mux = allocate_array_elem(&output_files, sizeof(Muxer), &nb_output_files);
    of  = &mux->of;

    of->index          = nb_output_files - 1;
    of->recording_time = o->recording_time;
    of->start_time     = o->start_time;
    of->shortest       = o->shortest;

    mux->thread_queue_size = o->thread_queue_size > 0 ? o->thread_queue_size : 8;
    mux->limit_filesize    = o->limit_filesize;
    av_dict_copy(&mux->opts, o->g->format_opts, 0);

    if (!strcmp(filename, "-"))
        filename = "pipe:";

    err = avformat_alloc_output_context2(&oc, NULL, o->format, filename);
    if (!oc) {
        print_error(filename, err);
        exit_program(1);
    }
    mux->fc = oc;

    if (strcmp(oc->oformat->name, "rtp"))
        want_sdp = 0;

    of->format = oc->oformat;
    if (o->recording_time != INT64_MAX)
        oc->duration = o->recording_time;

    oc->interrupt_callback = int_cb;

    if (o->bitexact) {
        oc->flags    |= AVFMT_FLAG_BITEXACT;
        of->bitexact  = 1;
    } else {
        of->bitexact  = check_opt_bitexact(oc, mux->opts, "fflags",
                                           AVFMT_FLAG_BITEXACT);
    }

    /* create streams for all unlabeled output pads */
    for (i = 0; i < nb_filtergraphs; i++) {
        FilterGraph *fg = filtergraphs[i];
        for (j = 0; j < fg->nb_outputs; j++) {
            OutputFilter *ofilter = fg->outputs[j];

            if (!ofilter->out_tmp || ofilter->out_tmp->name)
                continue;

            switch (ofilter->type) {
            case AVMEDIA_TYPE_VIDEO:    o->video_disable    = 1; break;
            case AVMEDIA_TYPE_AUDIO:    o->audio_disable    = 1; break;
            case AVMEDIA_TYPE_SUBTITLE: o->subtitle_disable = 1; break;
            }
            init_output_filter(ofilter, o, mux);
        }
    }

    if (!o->nb_stream_maps) {
        /* pick the "best" stream of each type */
        if (!o->video_disable)
            map_auto_video(mux, o);
        if (!o->audio_disable)
            map_auto_audio(mux, o);
        if (!o->subtitle_disable)
            map_auto_subtitle(mux, o);
        if (!o->data_disable)
            map_auto_data(mux, o);
    } else {
        for (int i = 0; i < o->nb_stream_maps; i++)
            map_manual(mux, o, &o->stream_maps[i]);
    }

    of_add_attachments(mux, o);

    if (!oc->nb_streams && !(oc->oformat->flags & AVFMT_NOSTREAMS)) {
        av_dump_format(oc, nb_output_files - 1, oc->url, 1);
        av_log(NULL, AV_LOG_ERROR, "Output file #%d does not contain any stream\n", nb_output_files - 1);
        exit_program(1);
    }

    /* check if all codec options have been used */
    unused_opts = strip_specifiers(o->g->codec_opts);
    for (int i = 0; i < of->nb_streams; i++) {
        e = NULL;
        while ((e = av_dict_get(of->streams[i]->encoder_opts, "", e,
                                AV_DICT_IGNORE_SUFFIX)))
            av_dict_set(&unused_opts, e->key, NULL, 0);
    }

    e = NULL;
    while ((e = av_dict_get(unused_opts, "", e, AV_DICT_IGNORE_SUFFIX))) {
        const AVClass *class = avcodec_get_class();
        const AVOption *option = av_opt_find(&class, e->key, NULL, 0,
                                             AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ);
        const AVClass *fclass = avformat_get_class();
        const AVOption *foption = av_opt_find(&fclass, e->key, NULL, 0,
                                              AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ);
        if (!option || foption)
            continue;


        if (!(option->flags & AV_OPT_FLAG_ENCODING_PARAM)) {
            av_log(NULL, AV_LOG_ERROR, "Codec AVOption %s (%s) specified for "
                   "output file #%d (%s) is not an encoding option.\n", e->key,
                   option->help ? option->help : "", nb_output_files - 1,
                   filename);
            exit_program(1);
        }

        // gop_timecode is injected by generic code but not always used
        if (!strcmp(e->key, "gop_timecode"))
            continue;

        av_log(NULL, AV_LOG_WARNING, "Codec AVOption %s (%s) specified for "
               "output file #%d (%s) has not been used for any stream. The most "
               "likely reason is either wrong type (e.g. a video option with "
               "no video streams) or that it is a private option of some encoder "
               "which was not actually used for any stream.\n", e->key,
               option->help ? option->help : "", nb_output_files - 1, filename);
    }
    av_dict_free(&unused_opts);

    /* set the decoding_needed flags and create simple filtergraphs */
    for (int i = 0; i < of->nb_streams; i++) {
        OutputStream *ost = of->streams[i];

        if (ost->enc_ctx && ost->source_index >= 0) {
            InputStream *ist = input_streams[ost->source_index];
            ist->decoding_needed |= DECODING_FOR_OST;
            ist->processing_needed = 1;

            if (ost->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
                ost->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                err = init_simple_filtergraph(ist, ost);
                if (err < 0) {
                    av_log(NULL, AV_LOG_ERROR,
                           "Error initializing a simple filtergraph between streams "
                           "%d:%d->%d:%d\n", ist->file_index, ost->source_index,
                           nb_output_files - 1, ost->st->index);
                    exit_program(1);
                }
            }
        } else if (ost->source_index >= 0) {
            InputStream *ist = input_streams[ost->source_index];
            ist->processing_needed = 1;
        }

        /* set the filter output constraints */
        if (ost->filter) {
            const AVCodec *c = ost->enc_ctx->codec;
            OutputFilter *f = ost->filter;
            switch (ost->enc_ctx->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                f->frame_rate = ost->frame_rate;
                f->width      = ost->enc_ctx->width;
                f->height     = ost->enc_ctx->height;
                if (ost->enc_ctx->pix_fmt != AV_PIX_FMT_NONE) {
                    f->format = ost->enc_ctx->pix_fmt;
                } else {
                    f->formats = c->pix_fmts;
                }
                break;
            case AVMEDIA_TYPE_AUDIO:
                if (ost->enc_ctx->sample_fmt != AV_SAMPLE_FMT_NONE) {
                    f->format = ost->enc_ctx->sample_fmt;
                } else {
                    f->formats = c->sample_fmts;
                }
                if (ost->enc_ctx->sample_rate) {
                    f->sample_rate = ost->enc_ctx->sample_rate;
                } else {
                    f->sample_rates = c->supported_samplerates;
                }
                if (ost->enc_ctx->ch_layout.nb_channels) {
                    set_channel_layout(f, ost);
                } else if (c->ch_layouts) {
                    f->ch_layouts = c->ch_layouts;
                }
                break;
            }
        }
    }

    /* check filename in case of an image number is expected */
    if (oc->oformat->flags & AVFMT_NEEDNUMBER) {
        if (!av_filename_number_test(oc->url)) {
            print_error(oc->url, AVERROR(EINVAL));
            exit_program(1);
        }
    }

    if (!(oc->oformat->flags & AVFMT_NOSTREAMS) && !input_stream_potentially_available) {
        av_log(NULL, AV_LOG_ERROR,
               "No input streams but output needs an input stream\n");
        exit_program(1);
    }

    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        /* test if it already exists to avoid losing precious files */
        assert_file_overwrite(filename);

        /* open the file */
        if ((err = avio_open2(&oc->pb, filename, AVIO_FLAG_WRITE,
                              &oc->interrupt_callback,
                              &mux->opts)) < 0) {
            print_error(filename, err);
            exit_program(1);
        }
    } else if (strcmp(oc->oformat->name, "image2")==0 && !av_filename_number_test(filename))
        assert_file_overwrite(filename);

    if (o->mux_preload) {
        av_dict_set_int(&mux->opts, "preload", o->mux_preload*AV_TIME_BASE, 0);
    }
    oc->max_delay = (int)(o->mux_max_delay * AV_TIME_BASE);

    /* copy metadata and chapters from input files */
    copy_meta(mux, o);

    of_add_programs(oc, o);
    of_add_metadata(of, oc, o);

    err = set_dispositions(of, oc);
    if (err < 0) {
        av_log(NULL, AV_LOG_FATAL, "Error setting output stream dispositions\n");
        exit_program(1);
    }

    err = setup_sync_queues(mux, oc, o->shortest_buf_duration * AV_TIME_BASE);
    if (err < 0) {
        av_log(NULL, AV_LOG_FATAL, "Error setting up output sync queues\n");
        exit_program(1);
    }

    of->url        = filename;

    /* write the header for files with no streams */
    if (of->format->flags & AVFMT_NOSTREAMS && oc->nb_streams == 0) {
        int ret = mux_check_init(mux);
        if (ret < 0)
            return ret;
    }

    return 0;
}
