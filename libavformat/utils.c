/*
 * various utility functions for use within FFmpeg
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard
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

#include <stdint.h>

#include "config.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/dict.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixfmt.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"

#include "libavcodec/bsf.h"
#include "libavcodec/bytestream.h"
#include "libavcodec/internal.h"
#include "libavcodec/packet_internal.h"
#include "libavcodec/raw.h"

#include "avformat.h"
#include "avio_internal.h"
#include "id3v2.h"
#include "internal.h"
#if CONFIG_NETWORK
#include "network.h"
#endif
#include "url.h"

#include "libavutil/ffversion.h"
const char av_format_ffversion[] = "FFmpeg version " FFMPEG_VERSION;

static AVMutex avformat_mutex = AV_MUTEX_INITIALIZER;

/**
 * @file
 * various utility functions for use within FFmpeg
 */

unsigned avformat_version(void)
{
    av_assert0(LIBAVFORMAT_VERSION_MICRO >= 100);
    return LIBAVFORMAT_VERSION_INT;
}

const char *avformat_configuration(void)
{
    return FFMPEG_CONFIGURATION;
}

const char *avformat_license(void)
{
#define LICENSE_PREFIX "libavformat license: "
    return &LICENSE_PREFIX FFMPEG_LICENSE[sizeof(LICENSE_PREFIX) - 1];
}

int ff_lock_avformat(void)
{
    return ff_mutex_lock(&avformat_mutex) ? -1 : 0;
}

int ff_unlock_avformat(void)
{
    return ff_mutex_unlock(&avformat_mutex) ? -1 : 0;
}

static int64_t wrap_timestamp(const AVStream *st, int64_t timestamp)
{
    const FFStream *const sti = cffstream(st);
    if (sti->pts_wrap_behavior != AV_PTS_WRAP_IGNORE && st->pts_wrap_bits < 64 &&
        sti->pts_wrap_reference != AV_NOPTS_VALUE && timestamp != AV_NOPTS_VALUE) {
        if (sti->pts_wrap_behavior == AV_PTS_WRAP_ADD_OFFSET &&
            timestamp < sti->pts_wrap_reference)
            return timestamp + (1ULL << st->pts_wrap_bits);
        else if (sti->pts_wrap_behavior == AV_PTS_WRAP_SUB_OFFSET &&
            timestamp >= sti->pts_wrap_reference)
            return timestamp - (1ULL << st->pts_wrap_bits);
    }
    return timestamp;
}

int64_t ff_wrap_timestamp(const AVStream *st, int64_t timestamp)
{
    return wrap_timestamp(st, timestamp);
}

int64_t av_stream_get_end_pts(const AVStream *st)
{
    if (cffstream(st)->priv_pts) {
        return cffstream(st)->priv_pts->val;
    } else
        return AV_NOPTS_VALUE;
}

struct AVCodecParserContext *av_stream_get_parser(const AVStream *st)
{
    return cffstream(st)->parser;
}

void avpriv_stream_set_need_parsing(AVStream *st, enum AVStreamParseType type)
{
    ffstream(st)->need_parsing = type;
}

void av_format_inject_global_side_data(AVFormatContext *s)
{
    FFFormatContext *const si = ffformatcontext(s);
    si->inject_global_side_data = 1;
    for (unsigned i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        ffstream(st)->inject_global_side_data = 1;
    }
}

int ff_copy_whiteblacklists(AVFormatContext *dst, const AVFormatContext *src)
{
    av_assert0(!dst->codec_whitelist &&
               !dst->format_whitelist &&
               !dst->protocol_whitelist &&
               !dst->protocol_blacklist);
    dst-> codec_whitelist = av_strdup(src->codec_whitelist);
    dst->format_whitelist = av_strdup(src->format_whitelist);
    dst->protocol_whitelist = av_strdup(src->protocol_whitelist);
    dst->protocol_blacklist = av_strdup(src->protocol_blacklist);
    if (   (src-> codec_whitelist && !dst-> codec_whitelist)
        || (src->  format_whitelist && !dst->  format_whitelist)
        || (src->protocol_whitelist && !dst->protocol_whitelist)
        || (src->protocol_blacklist && !dst->protocol_blacklist)) {
        av_log(dst, AV_LOG_ERROR, "Failed to duplicate black/whitelist\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

static const AVCodec *find_decoder(AVFormatContext *s, const AVStream *st, enum AVCodecID codec_id)
{
    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        if (s->video_codec)    return s->video_codec;
        break;
    case AVMEDIA_TYPE_AUDIO:
        if (s->audio_codec)    return s->audio_codec;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        if (s->subtitle_codec) return s->subtitle_codec;
        break;
    }

    return avcodec_find_decoder(codec_id);
}

static const AVCodec *find_probe_decoder(AVFormatContext *s, const AVStream *st, enum AVCodecID codec_id)
{
    const AVCodec *codec;

#if CONFIG_H264_DECODER
    /* Other parts of the code assume this decoder to be used for h264,
     * so force it if possible. */
    if (codec_id == AV_CODEC_ID_H264)
        return avcodec_find_decoder_by_name("h264");
#endif

    codec = find_decoder(s, st, codec_id);
    if (!codec)
        return NULL;

    if (codec->capabilities & AV_CODEC_CAP_AVOID_PROBING) {
        const AVCodec *probe_codec = NULL;
        void *iter = NULL;
        while ((probe_codec = av_codec_iterate(&iter))) {
            if (probe_codec->id == codec->id &&
                    av_codec_is_decoder(probe_codec) &&
                    !(probe_codec->capabilities & (AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_EXPERIMENTAL))) {
                return probe_codec;
            }
        }
    }

    return codec;
}

/* an arbitrarily chosen "sane" max packet size -- 50M */
#define SANE_CHUNK_SIZE (50000000)

/* Read the data in sane-sized chunks and append to pkt.
 * Return the number of bytes read or an error. */
static int append_packet_chunked(AVIOContext *s, AVPacket *pkt, int size)
{
    int orig_size      = pkt->size;
    int ret;

    do {
        int prev_size = pkt->size;
        int read_size;

        /* When the caller requests a lot of data, limit it to the amount
         * left in file or SANE_CHUNK_SIZE when it is not known. */
        read_size = size;
        if (read_size > SANE_CHUNK_SIZE/10) {
            read_size = ffio_limit(s, read_size);
            // If filesize/maxsize is unknown, limit to SANE_CHUNK_SIZE
            if (ffiocontext(s)->maxsize < 0)
                read_size = FFMIN(read_size, SANE_CHUNK_SIZE);
        }

        ret = av_grow_packet(pkt, read_size);
        if (ret < 0)
            break;

        ret = avio_read(s, pkt->data + prev_size, read_size);
        if (ret != read_size) {
            av_shrink_packet(pkt, prev_size + FFMAX(ret, 0));
            break;
        }

        size -= read_size;
    } while (size > 0);
    if (size > 0)
        pkt->flags |= AV_PKT_FLAG_CORRUPT;

    if (!pkt->size)
        av_packet_unref(pkt);
    return pkt->size > orig_size ? pkt->size - orig_size : ret;
}

int av_get_packet(AVIOContext *s, AVPacket *pkt, int size)
{
#if FF_API_INIT_PACKET
FF_DISABLE_DEPRECATION_WARNINGS
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
FF_ENABLE_DEPRECATION_WARNINGS
#else
    av_packet_unref(pkt);
#endif
    pkt->pos  = avio_tell(s);

    return append_packet_chunked(s, pkt, size);
}

int av_append_packet(AVIOContext *s, AVPacket *pkt, int size)
{
    if (!pkt->size)
        return av_get_packet(s, pkt, size);
    return append_packet_chunked(s, pkt, size);
}

int av_filename_number_test(const char *filename)
{
    char buf[1024];
    return filename &&
           (av_get_frame_filename(buf, sizeof(buf), filename, 1) >= 0);
}

static int set_codec_from_probe_data(AVFormatContext *s, AVStream *st,
                                     AVProbeData *pd)
{
    static const struct {
        const char *name;
        enum AVCodecID id;
        enum AVMediaType type;
    } fmt_id_type[] = {
        { "aac",       AV_CODEC_ID_AAC,        AVMEDIA_TYPE_AUDIO },
        { "ac3",       AV_CODEC_ID_AC3,        AVMEDIA_TYPE_AUDIO },
        { "aptx",      AV_CODEC_ID_APTX,       AVMEDIA_TYPE_AUDIO },
        { "dts",       AV_CODEC_ID_DTS,        AVMEDIA_TYPE_AUDIO },
        { "dvbsub",    AV_CODEC_ID_DVB_SUBTITLE,AVMEDIA_TYPE_SUBTITLE },
        { "dvbtxt",    AV_CODEC_ID_DVB_TELETEXT,AVMEDIA_TYPE_SUBTITLE },
        { "eac3",      AV_CODEC_ID_EAC3,       AVMEDIA_TYPE_AUDIO },
        { "h264",      AV_CODEC_ID_H264,       AVMEDIA_TYPE_VIDEO },
        { "hevc",      AV_CODEC_ID_HEVC,       AVMEDIA_TYPE_VIDEO },
        { "loas",      AV_CODEC_ID_AAC_LATM,   AVMEDIA_TYPE_AUDIO },
        { "m4v",       AV_CODEC_ID_MPEG4,      AVMEDIA_TYPE_VIDEO },
        { "mjpeg_2000",AV_CODEC_ID_JPEG2000,   AVMEDIA_TYPE_VIDEO },
        { "mp3",       AV_CODEC_ID_MP3,        AVMEDIA_TYPE_AUDIO },
        { "mpegvideo", AV_CODEC_ID_MPEG2VIDEO, AVMEDIA_TYPE_VIDEO },
        { "truehd",    AV_CODEC_ID_TRUEHD,     AVMEDIA_TYPE_AUDIO },
        { 0 }
    };
    int score;
    const AVInputFormat *fmt = av_probe_input_format3(pd, 1, &score);
    FFStream *const sti = ffstream(st);

    if (fmt) {
        av_log(s, AV_LOG_DEBUG,
               "Probe with size=%d, packets=%d detected %s with score=%d\n",
               pd->buf_size, s->max_probe_packets - sti->probe_packets,
               fmt->name, score);
        for (int i = 0; fmt_id_type[i].name; i++) {
            if (!strcmp(fmt->name, fmt_id_type[i].name)) {
                if (fmt_id_type[i].type != AVMEDIA_TYPE_AUDIO &&
                    st->codecpar->sample_rate)
                    continue;
                if (sti->request_probe > score &&
                    st->codecpar->codec_id != fmt_id_type[i].id)
                    continue;
                st->codecpar->codec_id   = fmt_id_type[i].id;
                st->codecpar->codec_type = fmt_id_type[i].type;
                sti->need_context_update = 1;
                return score;
            }
        }
    }
    return 0;
}

/************************************************************/
/* input media file */

/* Open input file and probe the format if necessary. */
static int init_input(AVFormatContext *s, const char *filename,
                      AVDictionary **options)
{
    int ret;
    AVProbeData pd = { filename, NULL, 0 };
    int score = AVPROBE_SCORE_RETRY;

    if (s->pb) {
        s->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (!s->iformat)
            return av_probe_input_buffer2(s->pb, &s->iformat, filename,
                                         s, 0, s->format_probesize);
        else if (s->iformat->flags & AVFMT_NOFILE)
            av_log(s, AV_LOG_WARNING, "Custom AVIOContext makes no sense and "
                                      "will be ignored with AVFMT_NOFILE format.\n");
        return 0;
    }

    if ((s->iformat && s->iformat->flags & AVFMT_NOFILE) ||
        (!s->iformat && (s->iformat = av_probe_input_format2(&pd, 0, &score))))
        return score;

    if ((ret = s->io_open(s, &s->pb, filename, AVIO_FLAG_READ | s->avio_flags, options)) < 0)
        return ret;

    if (s->iformat)
        return 0;
    return av_probe_input_buffer2(s->pb, &s->iformat, filename,
                                 s, 0, s->format_probesize);
}

int avformat_queue_attached_pictures(AVFormatContext *s)
{
    FFFormatContext *const si = ffformatcontext(s);
    int ret;
    for (unsigned i = 0; i < s->nb_streams; i++)
        if (s->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC &&
            s->streams[i]->discard < AVDISCARD_ALL) {
            if (s->streams[i]->attached_pic.size <= 0) {
                av_log(s, AV_LOG_WARNING,
                    "Attached picture on stream %d has invalid size, "
                    "ignoring\n", i);
                continue;
            }

            ret = avpriv_packet_list_put(&si->raw_packet_buffer,
                                         &si->raw_packet_buffer_end,
                                     &s->streams[i]->attached_pic,
                                     av_packet_ref, 0);
            if (ret < 0)
                return ret;
        }
    return 0;
}

int ff_add_attached_pic(AVFormatContext *s, AVStream *st0, AVIOContext *pb,
                        AVBufferRef **buf, int size)
{
    AVStream *st = st0;
    AVPacket *pkt;
    int ret;

    if (!st && !(st = avformat_new_stream(s, NULL)))
        return AVERROR(ENOMEM);
    pkt = &st->attached_pic;
    if (buf) {
        av_assert1(*buf);
        av_packet_unref(pkt);
        pkt->buf  = *buf;
        pkt->data = (*buf)->data;
        pkt->size = (*buf)->size - AV_INPUT_BUFFER_PADDING_SIZE;
        *buf = NULL;
    } else {
        ret = av_get_packet(pb, pkt, size);
        if (ret < 0)
            goto fail;
    }
    st->disposition         |= AV_DISPOSITION_ATTACHED_PIC;
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;

    pkt->stream_index = st->index;
    pkt->flags       |= AV_PKT_FLAG_KEY;

    return 0;
fail:
    if (!st0)
        ff_free_stream(s, st);
    return ret;
}

static int update_stream_avctx(AVFormatContext *s)
{
    int ret;
    for (unsigned i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        FFStream *const sti = ffstream(st);

        if (!sti->need_context_update)
            continue;

        /* close parser, because it depends on the codec */
        if (sti->parser && sti->avctx->codec_id != st->codecpar->codec_id) {
            av_parser_close(sti->parser);
            sti->parser = NULL;
        }

        /* update internal codec context, for the parser */
        ret = avcodec_parameters_to_context(sti->avctx, st->codecpar);
        if (ret < 0)
            return ret;

        sti->need_context_update = 0;
    }
    return 0;
}


int avformat_open_input(AVFormatContext **ps, const char *filename,
                        const AVInputFormat *fmt, AVDictionary **options)
{
    AVFormatContext *s = *ps;
    FFFormatContext *si;
    int ret = 0;
    AVDictionary *tmp = NULL;
    ID3v2ExtraMeta *id3v2_extra_meta = NULL;

    if (!s && !(s = avformat_alloc_context()))
        return AVERROR(ENOMEM);
    si = ffformatcontext(s);
    if (!s->av_class) {
        av_log(NULL, AV_LOG_ERROR, "Input context has not been properly allocated by avformat_alloc_context() and is not NULL either\n");
        return AVERROR(EINVAL);
    }
    if (fmt)
        s->iformat = fmt;

    if (options)
        av_dict_copy(&tmp, *options, 0);

    if (s->pb) // must be before any goto fail
        s->flags |= AVFMT_FLAG_CUSTOM_IO;

    if ((ret = av_opt_set_dict(s, &tmp)) < 0)
        goto fail;

    if (!(s->url = av_strdup(filename ? filename : ""))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if ((ret = init_input(s, filename, &tmp)) < 0)
        goto fail;
    s->probe_score = ret;

    if (!s->protocol_whitelist && s->pb && s->pb->protocol_whitelist) {
        s->protocol_whitelist = av_strdup(s->pb->protocol_whitelist);
        if (!s->protocol_whitelist) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    }

    if (!s->protocol_blacklist && s->pb && s->pb->protocol_blacklist) {
        s->protocol_blacklist = av_strdup(s->pb->protocol_blacklist);
        if (!s->protocol_blacklist) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    }

    if (s->format_whitelist && av_match_list(s->iformat->name, s->format_whitelist, ',') <= 0) {
        av_log(s, AV_LOG_ERROR, "Format not on whitelist \'%s\'\n", s->format_whitelist);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avio_skip(s->pb, s->skip_initial_bytes);

    /* Check filename in case an image number is expected. */
    if (s->iformat->flags & AVFMT_NEEDNUMBER) {
        if (!av_filename_number_test(filename)) {
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }

    s->duration = s->start_time = AV_NOPTS_VALUE;

    /* Allocate private data. */
    if (s->iformat->priv_data_size > 0) {
        if (!(s->priv_data = av_mallocz(s->iformat->priv_data_size))) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        if (s->iformat->priv_class) {
            *(const AVClass **) s->priv_data = s->iformat->priv_class;
            av_opt_set_defaults(s->priv_data);
            if ((ret = av_opt_set_dict(s->priv_data, &tmp)) < 0)
                goto fail;
        }
    }

    /* e.g. AVFMT_NOFILE formats will not have a AVIOContext */
    if (s->pb)
        ff_id3v2_read_dict(s->pb, &si->id3v2_meta, ID3v2_DEFAULT_MAGIC, &id3v2_extra_meta);

    if (s->iformat->read_header)
        if ((ret = s->iformat->read_header(s)) < 0) {
            if (s->iformat->flags_internal & FF_FMT_INIT_CLEANUP)
                goto close;
            goto fail;
        }

    if (!s->metadata) {
        s->metadata    = si->id3v2_meta;
        si->id3v2_meta = NULL;
    } else if (si->id3v2_meta) {
        av_log(s, AV_LOG_WARNING, "Discarding ID3 tags because more suitable tags were found.\n");
        av_dict_free(&si->id3v2_meta);
    }

    if (id3v2_extra_meta) {
        if (!strcmp(s->iformat->name, "mp3") || !strcmp(s->iformat->name, "aac") ||
            !strcmp(s->iformat->name, "tta") || !strcmp(s->iformat->name, "wav")) {
            if ((ret = ff_id3v2_parse_apic(s, id3v2_extra_meta)) < 0)
                goto close;
            if ((ret = ff_id3v2_parse_chapters(s, id3v2_extra_meta)) < 0)
                goto close;
            if ((ret = ff_id3v2_parse_priv(s, id3v2_extra_meta)) < 0)
                goto close;
        } else
            av_log(s, AV_LOG_DEBUG, "demuxer does not support additional id3 data, skipping\n");
    }
    ff_id3v2_free_extra_meta(&id3v2_extra_meta);

    if ((ret = avformat_queue_attached_pictures(s)) < 0)
        goto close;

    if (s->pb && !si->data_offset)
        si->data_offset = avio_tell(s->pb);

    si->raw_packet_buffer_remaining_size = RAW_PACKET_BUFFER_SIZE;

    update_stream_avctx(s);

    if (options) {
        av_dict_free(options);
        *options = tmp;
    }
    *ps = s;
    return 0;

close:
    if (s->iformat->read_close)
        s->iformat->read_close(s);
fail:
    ff_id3v2_free_extra_meta(&id3v2_extra_meta);
    av_dict_free(&tmp);
    if (s->pb && !(s->flags & AVFMT_FLAG_CUSTOM_IO))
        avio_closep(&s->pb);
    avformat_free_context(s);
    *ps = NULL;
    return ret;
}

/*******************************************************/

static void force_codec_ids(AVFormatContext *s, AVStream *st)
{
    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        if (s->video_codec_id)
            st->codecpar->codec_id = s->video_codec_id;
        break;
    case AVMEDIA_TYPE_AUDIO:
        if (s->audio_codec_id)
            st->codecpar->codec_id = s->audio_codec_id;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        if (s->subtitle_codec_id)
            st->codecpar->codec_id = s->subtitle_codec_id;
        break;
    case AVMEDIA_TYPE_DATA:
        if (s->data_codec_id)
            st->codecpar->codec_id = s->data_codec_id;
        break;
    }
}

static int probe_codec(AVFormatContext *s, AVStream *st, const AVPacket *pkt)
{
    FFFormatContext *const si = ffformatcontext(s);
    FFStream *const sti = ffstream(st);

    if (sti->request_probe > 0) {
        AVProbeData *pd = &sti->probe_data;
        int end;
        av_log(s, AV_LOG_DEBUG, "probing stream %d pp:%d\n", st->index, sti->probe_packets);
        --sti->probe_packets;

        if (pkt) {
            uint8_t *new_buf = av_realloc(pd->buf, pd->buf_size+pkt->size+AVPROBE_PADDING_SIZE);
            if (!new_buf) {
                av_log(s, AV_LOG_WARNING,
                       "Failed to reallocate probe buffer for stream %d\n",
                       st->index);
                goto no_packet;
            }
            pd->buf = new_buf;
            memcpy(pd->buf + pd->buf_size, pkt->data, pkt->size);
            pd->buf_size += pkt->size;
            memset(pd->buf + pd->buf_size, 0, AVPROBE_PADDING_SIZE);
        } else {
no_packet:
            sti->probe_packets = 0;
            if (!pd->buf_size) {
                av_log(s, AV_LOG_WARNING,
                       "nothing to probe for stream %d\n", st->index);
            }
        }

        end = si->raw_packet_buffer_remaining_size <= 0
                || sti->probe_packets <= 0;

        if (end || av_log2(pd->buf_size) != av_log2(pd->buf_size - pkt->size)) {
            int score = set_codec_from_probe_data(s, st, pd);
            if (    (st->codecpar->codec_id != AV_CODEC_ID_NONE && score > AVPROBE_SCORE_STREAM_RETRY)
                || end) {
                pd->buf_size = 0;
                av_freep(&pd->buf);
                sti->request_probe = -1;
                if (st->codecpar->codec_id != AV_CODEC_ID_NONE) {
                    av_log(s, AV_LOG_DEBUG, "probed stream %d\n", st->index);
                } else
                    av_log(s, AV_LOG_WARNING, "probed stream %d failed\n", st->index);
            }
            force_codec_ids(s, st);
        }
    }
    return 0;
}

static int update_wrap_reference(AVFormatContext *s, AVStream *st, int stream_index, AVPacket *pkt)
{
    FFStream *const sti = ffstream(st);
    int64_t ref = pkt->dts;
    int pts_wrap_behavior;
    int64_t pts_wrap_reference;
    AVProgram *first_program;

    if (ref == AV_NOPTS_VALUE)
        ref = pkt->pts;
    if (sti->pts_wrap_reference != AV_NOPTS_VALUE || st->pts_wrap_bits >= 63 || ref == AV_NOPTS_VALUE || !s->correct_ts_overflow)
        return 0;
    ref &= (1LL << st->pts_wrap_bits)-1;

    // reference time stamp should be 60 s before first time stamp
    pts_wrap_reference = ref - av_rescale(60, st->time_base.den, st->time_base.num);
    // if first time stamp is not more than 1/8 and 60s before the wrap point, subtract rather than add wrap offset
    pts_wrap_behavior = (ref < (1LL << st->pts_wrap_bits) - (1LL << st->pts_wrap_bits-3)) ||
        (ref < (1LL << st->pts_wrap_bits) - av_rescale(60, st->time_base.den, st->time_base.num)) ?
        AV_PTS_WRAP_ADD_OFFSET : AV_PTS_WRAP_SUB_OFFSET;

    first_program = av_find_program_from_stream(s, NULL, stream_index);

    if (!first_program) {
        int default_stream_index = av_find_default_stream_index(s);
        FFStream *const default_sti = ffstream(s->streams[default_stream_index]);
        if (default_sti->pts_wrap_reference == AV_NOPTS_VALUE) {
            for (unsigned i = 0; i < s->nb_streams; i++) {
                FFStream *const sti = ffstream(s->streams[i]);
                if (av_find_program_from_stream(s, NULL, i))
                    continue;
                sti->pts_wrap_reference = pts_wrap_reference;
                sti->pts_wrap_behavior  = pts_wrap_behavior;
            }
        }
        else {
            sti->pts_wrap_reference = default_sti->pts_wrap_reference;
            sti->pts_wrap_behavior  = default_sti->pts_wrap_behavior;
        }
    }
    else {
        AVProgram *program = first_program;
        while (program) {
            if (program->pts_wrap_reference != AV_NOPTS_VALUE) {
                pts_wrap_reference = program->pts_wrap_reference;
                pts_wrap_behavior = program->pts_wrap_behavior;
                break;
            }
            program = av_find_program_from_stream(s, program, stream_index);
        }

        // update every program with differing pts_wrap_reference
        program = first_program;
        while (program) {
            if (program->pts_wrap_reference != pts_wrap_reference) {
                for (unsigned i = 0; i < program->nb_stream_indexes; i++) {
                    FFStream *const sti = ffstream(s->streams[program->stream_index[i]]);
                    sti->pts_wrap_reference = pts_wrap_reference;
                    sti->pts_wrap_behavior  = pts_wrap_behavior;
                }

                program->pts_wrap_reference = pts_wrap_reference;
                program->pts_wrap_behavior = pts_wrap_behavior;
            }
            program = av_find_program_from_stream(s, program, stream_index);
        }
    }
    return 1;
}

int ff_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    FFFormatContext *const si = ffformatcontext(s);
    AVStream *st;
    int err;

#if FF_API_INIT_PACKET
FF_DISABLE_DEPRECATION_WARNINGS
    pkt->data = NULL;
    pkt->size = 0;
    av_init_packet(pkt);
FF_ENABLE_DEPRECATION_WARNINGS
#else
    av_packet_unref(pkt);
#endif

    for (;;) {
        PacketList *pktl = si->raw_packet_buffer;
        FFStream *sti;
        const AVPacket *pkt1;

        if (pktl) {
            st = s->streams[pktl->pkt.stream_index];
            if (si->raw_packet_buffer_remaining_size <= 0)
                if ((err = probe_codec(s, st, NULL)) < 0)
                    return err;
            if (ffstream(st)->request_probe <= 0) {
                avpriv_packet_list_get(&si->raw_packet_buffer,
                                       &si->raw_packet_buffer_end, pkt);
                si->raw_packet_buffer_remaining_size += pkt->size;
                return 0;
            }
        }

        err = s->iformat->read_packet(s, pkt);
        if (err < 0) {
            av_packet_unref(pkt);

            /* Some demuxers return FFERROR_REDO when they consume
               data and discard it (ignored streams, junk, extradata).
               We must re-call the demuxer to get the real packet. */
            if (err == FFERROR_REDO)
                continue;
            if (!pktl || err == AVERROR(EAGAIN))
                return err;
            for (unsigned i = 0; i < s->nb_streams; i++) {
                AVStream *const st  = s->streams[i];
                FFStream *const sti = ffstream(st);
                if (sti->probe_packets || sti->request_probe > 0)
                    if ((err = probe_codec(s, st, NULL)) < 0)
                        return err;
                av_assert0(sti->request_probe <= 0);
            }
            continue;
        }

        err = av_packet_make_refcounted(pkt);
        if (err < 0) {
            av_packet_unref(pkt);
            return err;
        }

        if (pkt->flags & AV_PKT_FLAG_CORRUPT) {
            av_log(s, AV_LOG_WARNING,
                   "Packet corrupt (stream = %d, dts = %s)",
                   pkt->stream_index, av_ts2str(pkt->dts));
            if (s->flags & AVFMT_FLAG_DISCARD_CORRUPT) {
                av_log(s, AV_LOG_WARNING, ", dropping it.\n");
                av_packet_unref(pkt);
                continue;
            }
            av_log(s, AV_LOG_WARNING, ".\n");
        }

        av_assert0(pkt->stream_index < (unsigned)s->nb_streams &&
                   "Invalid stream index.\n");

        st = s->streams[pkt->stream_index];
        sti = ffstream(st);

        if (!st)
            return -1;

        if (update_wrap_reference(s, st, pkt->stream_index, pkt) && sti->pts_wrap_behavior == AV_PTS_WRAP_SUB_OFFSET) {
            // correct first time stamps to negative values
            if (!is_relative(sti->first_dts))
                sti->first_dts = wrap_timestamp(st, sti->first_dts);
            if (!is_relative(st->start_time))
                st->start_time = wrap_timestamp(st, st->start_time);
            if (!is_relative(sti->cur_dts))
                sti->cur_dts = wrap_timestamp(st, sti->cur_dts);
        }

        pkt->dts = wrap_timestamp(st, pkt->dts);
        pkt->pts = wrap_timestamp(st, pkt->pts);

        force_codec_ids(s, st);

        /* TODO: audio: time filter; video: frame reordering (pts != dts) */
        if (s->use_wallclock_as_timestamps)
            pkt->dts = pkt->pts = av_rescale_q(av_gettime(), AV_TIME_BASE_Q, st->time_base);

        if (!pktl && sti->request_probe <= 0)
            return 0;

        err = avpriv_packet_list_put(&si->raw_packet_buffer,
                                     &si->raw_packet_buffer_end,
                                 pkt, NULL, 0);
        if (err < 0) {
            av_packet_unref(pkt);
            return err;
        }
        pkt1 = &si->raw_packet_buffer_end->pkt;
        si->raw_packet_buffer_remaining_size -= pkt1->size;

        if ((err = probe_codec(s, st, pkt1)) < 0)
            return err;
    }
}


/**********************************************************/

static int determinable_frame_size(AVCodecContext *avctx)
{
    switch(avctx->codec_id) {
    case AV_CODEC_ID_MP1:
    case AV_CODEC_ID_MP2:
    case AV_CODEC_ID_MP3:
    case AV_CODEC_ID_CODEC2:
        return 1;
    }

    return 0;
}

/**
 * Return the frame duration in seconds. Return 0 if not available.
 */
static void compute_frame_duration(AVFormatContext *s, int *pnum, int *pden,
                                   AVStream *st, AVCodecParserContext *pc,
                                   AVPacket *pkt)
{
    FFStream *const sti = ffstream(st);
    AVRational codec_framerate = sti->avctx->framerate;
    int frame_size, sample_rate;

    *pnum = 0;
    *pden = 0;

    if (!st || !st->codecpar)
        return;

    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        if (st->r_frame_rate.num && !pc) {
            *pnum = st->r_frame_rate.den;
            *pden = st->r_frame_rate.num;
        } else if (st->time_base.num * 1000LL > st->time_base.den) {
            *pnum = st->time_base.num;
            *pden = st->time_base.den;
        } else if (codec_framerate.den * 1000LL > codec_framerate.num) {
            av_assert0(sti->avctx->ticks_per_frame);
            av_reduce(pnum, pden,
                      codec_framerate.den,
                      codec_framerate.num * (int64_t)sti->avctx->ticks_per_frame,
                      INT_MAX);

            if (pc && pc->repeat_pict) {
                av_reduce(pnum, pden,
                          (*pnum) * (1LL + pc->repeat_pict),
                          (*pden),
                          INT_MAX);
            }
            /* If this codec can be interlaced or progressive then we need
             * a parser to compute duration of a packet. Thus if we have
             * no parser in such case leave duration undefined. */
            if (sti->avctx->ticks_per_frame > 1 && !pc)
                *pnum = *pden = 0;
        }
        break;
    case AVMEDIA_TYPE_AUDIO:
        if (sti->avctx_inited) {
            frame_size = av_get_audio_frame_duration(sti->avctx, pkt->size);
            sample_rate = sti->avctx->sample_rate;
        } else {
            frame_size = av_get_audio_frame_duration2(st->codecpar, pkt->size);
            sample_rate = st->codecpar->sample_rate;
        }
        if (frame_size <= 0 || sample_rate <= 0)
            break;
        *pnum = frame_size;
        *pden = sample_rate;
        break;
    default:
        break;
    }
}

int ff_is_intra_only(enum AVCodecID id)
{
    const AVCodecDescriptor *d = avcodec_descriptor_get(id);
    if (!d)
        return 0;
    if ((d->type == AVMEDIA_TYPE_VIDEO || d->type == AVMEDIA_TYPE_AUDIO) &&
        !(d->props & AV_CODEC_PROP_INTRA_ONLY))
        return 0;
    return 1;
}

static int has_decode_delay_been_guessed(AVStream *st)
{
    FFStream *const sti = ffstream(st);
    if (st->codecpar->codec_id != AV_CODEC_ID_H264) return 1;
    if (!sti->info) // if we have left find_stream_info then nb_decoded_frames won't increase anymore for stream copy
        return 1;
#if CONFIG_H264_DECODER
    if (sti->avctx->has_b_frames &&
       avpriv_h264_has_num_reorder_frames(sti->avctx) == sti->avctx->has_b_frames)
        return 1;
#endif
    if (sti->avctx->has_b_frames<3)
        return sti->nb_decoded_frames >= 7;
    else if (sti->avctx->has_b_frames<4)
        return sti->nb_decoded_frames >= 18;
    else
        return sti->nb_decoded_frames >= 20;
}

static PacketList *get_next_pkt(AVFormatContext *s, AVStream *st, PacketList *pktl)
{
    FFFormatContext *const si = ffformatcontext(s);
    if (pktl->next)
        return pktl->next;
    if (pktl == si->packet_buffer_end)
        return si->parse_queue;
    return NULL;
}

static int64_t select_from_pts_buffer(AVStream *st, int64_t *pts_buffer, int64_t dts) {
    FFStream *const sti = ffstream(st);
    int onein_oneout = st->codecpar->codec_id != AV_CODEC_ID_H264 &&
                       st->codecpar->codec_id != AV_CODEC_ID_HEVC;

    if(!onein_oneout) {
        int delay = sti->avctx->has_b_frames;

        if (dts == AV_NOPTS_VALUE) {
            int64_t best_score = INT64_MAX;
            for (int i = 0; i < delay; i++) {
                if (sti->pts_reorder_error_count[i]) {
                    int64_t score = sti->pts_reorder_error[i] / sti->pts_reorder_error_count[i];
                    if (score < best_score) {
                        best_score = score;
                        dts = pts_buffer[i];
                    }
                }
            }
        } else {
            for (int i = 0; i < delay; i++) {
                if (pts_buffer[i] != AV_NOPTS_VALUE) {
                    int64_t diff =  FFABS(pts_buffer[i] - dts)
                                    + (uint64_t)sti->pts_reorder_error[i];
                    diff = FFMAX(diff, sti->pts_reorder_error[i]);
                    sti->pts_reorder_error[i] = diff;
                    sti->pts_reorder_error_count[i]++;
                    if (sti->pts_reorder_error_count[i] > 250) {
                        sti->pts_reorder_error[i] >>= 1;
                        sti->pts_reorder_error_count[i] >>= 1;
                    }
                }
            }
        }
    }

    if (dts == AV_NOPTS_VALUE)
        dts = pts_buffer[0];

    return dts;
}

/**
 * Updates the dts of packets of a stream in pkt_buffer, by re-ordering the pts
 * of the packets in a window.
 */
static void update_dts_from_pts(AVFormatContext *s, int stream_index,
                                PacketList *pkt_buffer)
{
    AVStream *st       = s->streams[stream_index];
    int delay = ffstream(st)->avctx->has_b_frames;

    int64_t pts_buffer[MAX_REORDER_DELAY+1];

    for (int i = 0; i < MAX_REORDER_DELAY + 1; i++)
        pts_buffer[i] = AV_NOPTS_VALUE;

    for (; pkt_buffer; pkt_buffer = get_next_pkt(s, st, pkt_buffer)) {
        if (pkt_buffer->pkt.stream_index != stream_index)
            continue;

        if (pkt_buffer->pkt.pts != AV_NOPTS_VALUE && delay <= MAX_REORDER_DELAY) {
            pts_buffer[0] = pkt_buffer->pkt.pts;
            for (int i = 0; i < delay && pts_buffer[i] > pts_buffer[i + 1]; i++)
                FFSWAP(int64_t, pts_buffer[i], pts_buffer[i + 1]);

            pkt_buffer->pkt.dts = select_from_pts_buffer(st, pts_buffer, pkt_buffer->pkt.dts);
        }
    }
}

static void update_initial_timestamps(AVFormatContext *s, int stream_index,
                                      int64_t dts, int64_t pts, AVPacket *pkt)
{
    FFFormatContext *const si = ffformatcontext(s);
    AVStream *st       = s->streams[stream_index];
    FFStream *const sti = ffstream(st);
    PacketList *pktl = si->packet_buffer ? si->packet_buffer : si->parse_queue;
    PacketList *pktl_it;

    uint64_t shift;

    if (!st)
        return;

    if (sti->first_dts != AV_NOPTS_VALUE ||
        dts           == AV_NOPTS_VALUE ||
        sti->cur_dts   == AV_NOPTS_VALUE ||
        sti->cur_dts < INT_MIN + RELATIVE_TS_BASE ||
        dts  < INT_MIN + (sti->cur_dts - RELATIVE_TS_BASE) ||
        is_relative(dts))
        return;

    sti->first_dts = dts - (sti->cur_dts - RELATIVE_TS_BASE);
    sti->cur_dts   = dts;
    shift          = (uint64_t)sti->first_dts - RELATIVE_TS_BASE;

    if (is_relative(pts))
        pts += shift;

    for (pktl_it = pktl; pktl_it; pktl_it = get_next_pkt(s, st, pktl_it)) {
        if (pktl_it->pkt.stream_index != stream_index)
            continue;
        if (is_relative(pktl_it->pkt.pts))
            pktl_it->pkt.pts += shift;

        if (is_relative(pktl_it->pkt.dts))
            pktl_it->pkt.dts += shift;

        if (st->start_time == AV_NOPTS_VALUE && pktl_it->pkt.pts != AV_NOPTS_VALUE) {
            st->start_time = pktl_it->pkt.pts;
            if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && st->codecpar->sample_rate)
                st->start_time = av_sat_add64(st->start_time, av_rescale_q(sti->skip_samples, (AVRational){1, st->codecpar->sample_rate}, st->time_base));
        }
    }

    if (has_decode_delay_been_guessed(st)) {
        update_dts_from_pts(s, stream_index, pktl);
    }

    if (st->start_time == AV_NOPTS_VALUE) {
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO || !(pkt->flags & AV_PKT_FLAG_DISCARD)) {
            st->start_time = pts;
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && st->codecpar->sample_rate)
            st->start_time = av_sat_add64(st->start_time, av_rescale_q(sti->skip_samples, (AVRational){1, st->codecpar->sample_rate}, st->time_base));
    }
}

static void update_initial_durations(AVFormatContext *s, AVStream *st,
                                     int stream_index, int64_t duration)
{
    FFFormatContext *const si = ffformatcontext(s);
    FFStream *const sti = ffstream(st);
    PacketList *pktl = si->packet_buffer ? si->packet_buffer : si->parse_queue;
    int64_t cur_dts    = RELATIVE_TS_BASE;

    if (sti->first_dts != AV_NOPTS_VALUE) {
        if (sti->update_initial_durations_done)
            return;
        sti->update_initial_durations_done = 1;
        cur_dts = sti->first_dts;
        for (; pktl; pktl = get_next_pkt(s, st, pktl)) {
            if (pktl->pkt.stream_index == stream_index) {
                if (pktl->pkt.pts != pktl->pkt.dts  ||
                    pktl->pkt.dts != AV_NOPTS_VALUE ||
                    pktl->pkt.duration)
                    break;
                cur_dts -= duration;
            }
        }
        if (pktl && pktl->pkt.dts != sti->first_dts) {
            av_log(s, AV_LOG_DEBUG, "first_dts %s not matching first dts %s (pts %s, duration %"PRId64") in the queue\n",
                   av_ts2str(sti->first_dts), av_ts2str(pktl->pkt.dts), av_ts2str(pktl->pkt.pts), pktl->pkt.duration);
            return;
        }
        if (!pktl) {
            av_log(s, AV_LOG_DEBUG, "first_dts %s but no packet with dts in the queue\n", av_ts2str(sti->first_dts));
            return;
        }
        pktl = si->packet_buffer ? si->packet_buffer : si->parse_queue;
        sti->first_dts = cur_dts;
    } else if (sti->cur_dts != RELATIVE_TS_BASE)
        return;

    for (; pktl; pktl = get_next_pkt(s, st, pktl)) {
        if (pktl->pkt.stream_index != stream_index)
            continue;
        if ((pktl->pkt.pts == pktl->pkt.dts ||
             pktl->pkt.pts == AV_NOPTS_VALUE) &&
            (pktl->pkt.dts == AV_NOPTS_VALUE ||
             pktl->pkt.dts == sti->first_dts ||
             pktl->pkt.dts == RELATIVE_TS_BASE) &&
            !pktl->pkt.duration &&
            av_sat_add64(cur_dts, duration) == cur_dts + (uint64_t)duration
        ) {
            pktl->pkt.dts = cur_dts;
            if (!sti->avctx->has_b_frames)
                pktl->pkt.pts = cur_dts;
            pktl->pkt.duration = duration;
        } else
            break;
        cur_dts = pktl->pkt.dts + pktl->pkt.duration;
    }
    if (!pktl)
        sti->cur_dts = cur_dts;
}

static void compute_pkt_fields(AVFormatContext *s, AVStream *st,
                               AVCodecParserContext *pc, AVPacket *pkt,
                               int64_t next_dts, int64_t next_pts)
{
    FFFormatContext *const si = ffformatcontext(s);
    FFStream *const sti = ffstream(st);
    int num, den, presentation_delayed, delay;
    int64_t offset;
    AVRational duration;
    int onein_oneout = st->codecpar->codec_id != AV_CODEC_ID_H264 &&
                       st->codecpar->codec_id != AV_CODEC_ID_HEVC;

    if (s->flags & AVFMT_FLAG_NOFILLIN)
        return;

    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && pkt->dts != AV_NOPTS_VALUE) {
        if (pkt->dts == pkt->pts && sti->last_dts_for_order_check != AV_NOPTS_VALUE) {
            if (sti->last_dts_for_order_check <= pkt->dts) {
                sti->dts_ordered++;
            } else {
                av_log(s, sti->dts_misordered ? AV_LOG_DEBUG : AV_LOG_WARNING,
                       "DTS %"PRIi64" < %"PRIi64" out of order\n",
                       pkt->dts,
                       sti->last_dts_for_order_check);
                sti->dts_misordered++;
            }
            if (sti->dts_ordered + sti->dts_misordered > 250) {
                sti->dts_ordered    >>= 1;
                sti->dts_misordered >>= 1;
            }
        }

        sti->last_dts_for_order_check = pkt->dts;
        if (sti->dts_ordered < 8*sti->dts_misordered && pkt->dts == pkt->pts)
            pkt->dts = AV_NOPTS_VALUE;
    }

    if ((s->flags & AVFMT_FLAG_IGNDTS) && pkt->pts != AV_NOPTS_VALUE)
        pkt->dts = AV_NOPTS_VALUE;

    if (pc && pc->pict_type == AV_PICTURE_TYPE_B
        && !sti->avctx->has_b_frames)
        //FIXME Set low_delay = 0 when has_b_frames = 1
        sti->avctx->has_b_frames = 1;

    /* do we have a video B-frame ? */
    delay = sti->avctx->has_b_frames;
    presentation_delayed = 0;

    /* XXX: need has_b_frame, but cannot get it if the codec is
     *  not initialized */
    if (delay &&
        pc && pc->pict_type != AV_PICTURE_TYPE_B)
        presentation_delayed = 1;

    if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE &&
        st->pts_wrap_bits < 63 && pkt->dts > INT64_MIN + (1LL << st->pts_wrap_bits) &&
        pkt->dts - (1LL << (st->pts_wrap_bits - 1)) > pkt->pts) {
        if (is_relative(sti->cur_dts) || pkt->dts - (1LL<<(st->pts_wrap_bits - 1)) > sti->cur_dts) {
            pkt->dts -= 1LL << st->pts_wrap_bits;
        } else
            pkt->pts += 1LL << st->pts_wrap_bits;
    }

    /* Some MPEG-2 in MPEG-PS lack dts (issue #171 / input_file.mpg).
     * We take the conservative approach and discard both.
     * Note: If this is misbehaving for an H.264 file, then possibly
     * presentation_delayed is not set correctly. */
    if (delay == 1 && pkt->dts == pkt->pts &&
        pkt->dts != AV_NOPTS_VALUE && presentation_delayed) {
        av_log(s, AV_LOG_DEBUG, "invalid dts/pts combination %"PRIi64"\n", pkt->dts);
        if (    strcmp(s->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2")
             && strcmp(s->iformat->name, "flv")) // otherwise we discard correct timestamps for vc1-wmapro.ism
            pkt->dts = AV_NOPTS_VALUE;
    }

    duration = av_mul_q((AVRational) {pkt->duration, 1}, st->time_base);
    if (pkt->duration <= 0) {
        compute_frame_duration(s, &num, &den, st, pc, pkt);
        if (den && num) {
            duration = (AVRational) {num, den};
            pkt->duration = av_rescale_rnd(1,
                                           num * (int64_t) st->time_base.den,
                                           den * (int64_t) st->time_base.num,
                                           AV_ROUND_DOWN);
        }
    }

    if (pkt->duration > 0 && (si->packet_buffer || si->parse_queue))
        update_initial_durations(s, st, pkt->stream_index, pkt->duration);

    /* Correct timestamps with byte offset if demuxers only have timestamps
     * on packet boundaries */
    if (pc && sti->need_parsing == AVSTREAM_PARSE_TIMESTAMPS && pkt->size) {
        /* this will estimate bitrate based on this frame's duration and size */
        offset = av_rescale(pc->offset, pkt->duration, pkt->size);
        if (pkt->pts != AV_NOPTS_VALUE)
            pkt->pts += offset;
        if (pkt->dts != AV_NOPTS_VALUE)
            pkt->dts += offset;
    }

    /* This may be redundant, but it should not hurt. */
    if (pkt->dts != AV_NOPTS_VALUE &&
        pkt->pts != AV_NOPTS_VALUE &&
        pkt->pts > pkt->dts)
        presentation_delayed = 1;

    if (s->debug & FF_FDEBUG_TS)
        av_log(s, AV_LOG_DEBUG,
            "IN delayed:%d pts:%s, dts:%s cur_dts:%s st:%d pc:%p duration:%"PRId64" delay:%d onein_oneout:%d\n",
            presentation_delayed, av_ts2str(pkt->pts), av_ts2str(pkt->dts), av_ts2str(sti->cur_dts),
            pkt->stream_index, pc, pkt->duration, delay, onein_oneout);

    /* Interpolate PTS and DTS if they are not present. We skip H264
     * currently because delay and has_b_frames are not reliably set. */
    if ((delay == 0 || (delay == 1 && pc)) &&
        onein_oneout) {
        if (presentation_delayed) {
            /* DTS = decompression timestamp */
            /* PTS = presentation timestamp */
            if (pkt->dts == AV_NOPTS_VALUE)
                pkt->dts = sti->last_IP_pts;
            update_initial_timestamps(s, pkt->stream_index, pkt->dts, pkt->pts, pkt);
            if (pkt->dts == AV_NOPTS_VALUE)
                pkt->dts = sti->cur_dts;

            /* This is tricky: the dts must be incremented by the duration
             * of the frame we are displaying, i.e. the last I- or P-frame. */
            if (sti->last_IP_duration == 0 && (uint64_t)pkt->duration <= INT32_MAX)
                sti->last_IP_duration = pkt->duration;
            if (pkt->dts != AV_NOPTS_VALUE)
                sti->cur_dts = av_sat_add64(pkt->dts, sti->last_IP_duration);
            if (pkt->dts != AV_NOPTS_VALUE &&
                pkt->pts == AV_NOPTS_VALUE &&
                sti->last_IP_duration > 0 &&
                ((uint64_t)sti->cur_dts - (uint64_t)next_dts + 1) <= 2 &&
                next_dts != next_pts &&
                next_pts != AV_NOPTS_VALUE)
                pkt->pts = next_dts;

            if ((uint64_t)pkt->duration <= INT32_MAX)
                sti->last_IP_duration = pkt->duration;
            sti->last_IP_pts      = pkt->pts;
            /* Cannot compute PTS if not present (we can compute it only
             * by knowing the future. */
        } else if (pkt->pts != AV_NOPTS_VALUE ||
                   pkt->dts != AV_NOPTS_VALUE ||
                   pkt->duration > 0             ) {

            /* presentation is not delayed : PTS and DTS are the same */
            if (pkt->pts == AV_NOPTS_VALUE)
                pkt->pts = pkt->dts;
            update_initial_timestamps(s, pkt->stream_index, pkt->pts,
                                      pkt->pts, pkt);
            if (pkt->pts == AV_NOPTS_VALUE)
                pkt->pts = sti->cur_dts;
            pkt->dts = pkt->pts;
            if (pkt->pts != AV_NOPTS_VALUE && duration.num >= 0)
                sti->cur_dts = av_add_stable(st->time_base, pkt->pts, duration, 1);
        }
    }

    if (pkt->pts != AV_NOPTS_VALUE && delay <= MAX_REORDER_DELAY) {
        sti->pts_buffer[0] = pkt->pts;
        for (int i = 0; i < delay && sti->pts_buffer[i] > sti->pts_buffer[i + 1]; i++)
            FFSWAP(int64_t, sti->pts_buffer[i], sti->pts_buffer[i + 1]);

        if(has_decode_delay_been_guessed(st))
            pkt->dts = select_from_pts_buffer(st, sti->pts_buffer, pkt->dts);
    }
    // We skipped it above so we try here.
    if (!onein_oneout)
        // This should happen on the first packet
        update_initial_timestamps(s, pkt->stream_index, pkt->dts, pkt->pts, pkt);
    if (pkt->dts > sti->cur_dts)
        sti->cur_dts = pkt->dts;

    if (s->debug & FF_FDEBUG_TS)
        av_log(s, AV_LOG_DEBUG, "OUTdelayed:%d/%d pts:%s, dts:%s cur_dts:%s st:%d (%d)\n",
            presentation_delayed, delay, av_ts2str(pkt->pts), av_ts2str(pkt->dts), av_ts2str(sti->cur_dts), st->index, st->id);

    /* update flags */
    if (st->codecpar->codec_type == AVMEDIA_TYPE_DATA || ff_is_intra_only(st->codecpar->codec_id))
        pkt->flags |= AV_PKT_FLAG_KEY;
}

/**
 * Parse a packet, add all split parts to parse_queue.
 *
 * @param pkt   Packet to parse; must not be NULL.
 * @param flush Indicates whether to flush. If set, pkt must be blank.
 */
static int parse_packet(AVFormatContext *s, AVPacket *pkt,
                        int stream_index, int flush)
{
    FFFormatContext *const si = ffformatcontext(s);
    AVPacket *out_pkt = si->parse_pkt;
    AVStream *st = s->streams[stream_index];
    FFStream *const sti = ffstream(st);
    uint8_t *data = pkt->data;
    int size      = pkt->size;
    int ret = 0, got_output = flush;

    if (!size && !flush && sti->parser->flags & PARSER_FLAG_COMPLETE_FRAMES) {
        // preserve 0-size sync packets
        compute_pkt_fields(s, st, sti->parser, pkt, AV_NOPTS_VALUE, AV_NOPTS_VALUE);
    }

    while (size > 0 || (flush && got_output)) {
        int len;
        int64_t next_pts = pkt->pts;
        int64_t next_dts = pkt->dts;

        len = av_parser_parse2(sti->parser, sti->avctx,
                               &out_pkt->data, &out_pkt->size, data, size,
                               pkt->pts, pkt->dts, pkt->pos);

        pkt->pts = pkt->dts = AV_NOPTS_VALUE;
        pkt->pos = -1;
        /* increment read pointer */
        av_assert1(data || !len);
        data  = len ? data + len : data;
        size -= len;

        got_output = !!out_pkt->size;

        if (!out_pkt->size)
            continue;

        if (pkt->buf && out_pkt->data == pkt->data) {
            /* reference pkt->buf only when out_pkt->data is guaranteed to point
             * to data in it and not in the parser's internal buffer. */
            /* XXX: Ensure this is the case with all parsers when sti->parser->flags
             * is PARSER_FLAG_COMPLETE_FRAMES and check for that instead? */
            out_pkt->buf = av_buffer_ref(pkt->buf);
            if (!out_pkt->buf) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        } else {
            ret = av_packet_make_refcounted(out_pkt);
            if (ret < 0)
                goto fail;
        }

        if (pkt->side_data) {
            out_pkt->side_data       = pkt->side_data;
            out_pkt->side_data_elems = pkt->side_data_elems;
            pkt->side_data          = NULL;
            pkt->side_data_elems    = 0;
        }

        /* set the duration */
        out_pkt->duration = (sti->parser->flags & PARSER_FLAG_COMPLETE_FRAMES) ? pkt->duration : 0;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (sti->avctx->sample_rate > 0) {
                out_pkt->duration =
                    av_rescale_q_rnd(sti->parser->duration,
                                     (AVRational) { 1, sti->avctx->sample_rate },
                                     st->time_base,
                                     AV_ROUND_DOWN);
            }
        }

        out_pkt->stream_index = st->index;
        out_pkt->pts          = sti->parser->pts;
        out_pkt->dts          = sti->parser->dts;
        out_pkt->pos          = sti->parser->pos;
        out_pkt->flags       |= pkt->flags & AV_PKT_FLAG_DISCARD;

        if (sti->need_parsing == AVSTREAM_PARSE_FULL_RAW)
            out_pkt->pos = sti->parser->frame_offset;

        if (sti->parser->key_frame == 1 ||
            (sti->parser->key_frame == -1 &&
             sti->parser->pict_type == AV_PICTURE_TYPE_I))
            out_pkt->flags |= AV_PKT_FLAG_KEY;

        if (sti->parser->key_frame == -1 && sti->parser->pict_type ==AV_PICTURE_TYPE_NONE && (pkt->flags&AV_PKT_FLAG_KEY))
            out_pkt->flags |= AV_PKT_FLAG_KEY;

        compute_pkt_fields(s, st, sti->parser, out_pkt, next_dts, next_pts);

        ret = avpriv_packet_list_put(&si->parse_queue,
                                     &si->parse_queue_end,
                                 out_pkt, NULL, 0);
        if (ret < 0)
            goto fail;
    }

    /* end of the stream => close and free the parser */
    if (flush) {
        av_parser_close(sti->parser);
        sti->parser = NULL;
    }

fail:
    if (ret < 0)
        av_packet_unref(out_pkt);
    av_packet_unref(pkt);
    return ret;
}

static int64_t ts_to_samples(AVStream *st, int64_t ts)
{
    return av_rescale(ts, st->time_base.num * st->codecpar->sample_rate, st->time_base.den);
}

/**
 * Simply sets data pointer to null.
 *
 * This will leak memory if no one else frees the memory used by the packet.
 */
static int read_frame_internal(AVFormatContext *s, AVPacket *pkt)
{
    FFFormatContext *const si = ffformatcontext(s);
    int ret, got_packet = 0;
    AVDictionary *metadata = NULL;

    while (!got_packet && !si->parse_queue) {
        AVStream *st;
        FFStream *sti;

        /* read next packet */
        ret = ff_read_packet(s, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN))
                return ret;
            /* flush the parsers */
            for (unsigned i = 0; i < s->nb_streams; i++) {
                AVStream *const st  = s->streams[i];
                FFStream *const sti = ffstream(st);
                if (sti->parser && sti->need_parsing)
                    parse_packet(s, pkt, st->index, 1);
            }
            /* all remaining packets are now in parse_queue =>
             * really terminate parsing */
            break;
        }
        ret = 0;
        st  = s->streams[pkt->stream_index];
        sti = ffstream(st);

        st->event_flags |= AVSTREAM_EVENT_FLAG_NEW_PACKETS;

        /* update context if required */
        if (sti->need_context_update) {
            if (avcodec_is_open(sti->avctx)) {
                av_log(s, AV_LOG_DEBUG, "Demuxer context update while decoder is open, closing and trying to re-open\n");
                avcodec_close(sti->avctx);
                sti->info->found_decoder = 0;
            }

            /* close parser, because it depends on the codec */
            if (sti->parser && sti->avctx->codec_id != st->codecpar->codec_id) {
                av_parser_close(sti->parser);
                sti->parser = NULL;
            }

            ret = avcodec_parameters_to_context(sti->avctx, st->codecpar);
            if (ret < 0) {
                av_packet_unref(pkt);
                return ret;
            }

            sti->need_context_update = 0;
        }

        if (pkt->pts != AV_NOPTS_VALUE &&
            pkt->dts != AV_NOPTS_VALUE &&
            pkt->pts < pkt->dts) {
            av_log(s, AV_LOG_WARNING,
                   "Invalid timestamps stream=%d, pts=%s, dts=%s, size=%d\n",
                   pkt->stream_index,
                   av_ts2str(pkt->pts),
                   av_ts2str(pkt->dts),
                   pkt->size);
        }
        if (s->debug & FF_FDEBUG_TS)
            av_log(s, AV_LOG_DEBUG,
                   "ff_read_packet stream=%d, pts=%s, dts=%s, size=%d, duration=%"PRId64", flags=%d\n",
                   pkt->stream_index,
                   av_ts2str(pkt->pts),
                   av_ts2str(pkt->dts),
                   pkt->size, pkt->duration, pkt->flags);

        if (/* MythTV null checks */ st && st->codecpar &&
            sti->need_parsing && !sti->parser && !(s->flags & AVFMT_FLAG_NOPARSE)) {
            sti->parser = av_parser_init(st->codecpar->codec_id);
            if (!sti->parser) {
                av_log(s, AV_LOG_VERBOSE, "parser not found for codec "
                       "%s, packets or times may be invalid.\n",
                       avcodec_get_name(st->codecpar->codec_id));
                /* no parser available: just output the raw packets */
                sti->need_parsing = AVSTREAM_PARSE_NONE;
            } else if (sti->need_parsing == AVSTREAM_PARSE_HEADERS)
                sti->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
            else if (sti->need_parsing == AVSTREAM_PARSE_FULL_ONCE)
                sti->parser->flags |= PARSER_FLAG_ONCE;
            else if (sti->need_parsing == AVSTREAM_PARSE_FULL_RAW)
                sti->parser->flags |= PARSER_FLAG_USE_CODEC_TS;
        }

        if (!sti->need_parsing || !sti->parser) {
            /* no parsing needed: we just output the packet as is */
            compute_pkt_fields(s, st, NULL, pkt, AV_NOPTS_VALUE, AV_NOPTS_VALUE);
            if ((s->iformat->flags & AVFMT_GENERIC_INDEX) &&
                (pkt->flags & AV_PKT_FLAG_KEY) && pkt->dts != AV_NOPTS_VALUE) {
                ff_reduce_index(s, st->index);
                av_add_index_entry(st, pkt->pos, pkt->dts,
                                   0, 0, AVINDEX_KEYFRAME);
            }
            got_packet = 1;
        } else if (st->discard < AVDISCARD_ALL) {
            if ((ret = parse_packet(s, pkt, pkt->stream_index, 0)) < 0)
                return ret;
            st->codecpar->sample_rate = sti->avctx->sample_rate;
            st->codecpar->bit_rate = sti->avctx->bit_rate;
            st->codecpar->channels = sti->avctx->channels;
            st->codecpar->channel_layout = sti->avctx->channel_layout;
            st->codecpar->codec_id = sti->avctx->codec_id;
        } else {
            /* free packet */
            av_packet_unref(pkt);
        }
        if (pkt->flags & AV_PKT_FLAG_KEY)
            sti->skip_to_keyframe = 0;
        if (sti->skip_to_keyframe) {
            av_packet_unref(pkt);
            got_packet = 0;
        }
    }

    if (!got_packet && si->parse_queue)
        ret = avpriv_packet_list_get(&si->parse_queue, &si->parse_queue_end, pkt);

    if (ret >= 0) {
        AVStream *st = s->streams[pkt->stream_index];
        FFStream *const sti = ffstream(st);
        int discard_padding = 0;
        if (sti->first_discard_sample && pkt->pts != AV_NOPTS_VALUE) {
            int64_t pts = pkt->pts - (is_relative(pkt->pts) ? RELATIVE_TS_BASE : 0);
            int64_t sample = ts_to_samples(st, pts);
            int duration = ts_to_samples(st, pkt->duration);
            int64_t end_sample = sample + duration;
            if (duration > 0 && end_sample >= sti->first_discard_sample &&
                sample < sti->last_discard_sample)
                discard_padding = FFMIN(end_sample - sti->first_discard_sample, duration);
        }
        if (sti->start_skip_samples && (pkt->pts == 0 || pkt->pts == RELATIVE_TS_BASE))
            sti->skip_samples = sti->start_skip_samples;
        if (sti->skip_samples || discard_padding) {
            uint8_t *p = av_packet_new_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, 10);
            if (p) {
                AV_WL32(p, sti->skip_samples);
                AV_WL32(p + 4, discard_padding);
                av_log(s, AV_LOG_DEBUG, "demuxer injecting skip %d / discard %d\n", sti->skip_samples, discard_padding);
            }
            sti->skip_samples = 0;
        }

        if (sti->inject_global_side_data) {
            for (int i = 0; i < st->nb_side_data; i++) {
                AVPacketSideData *src_sd = &st->side_data[i];
                uint8_t *dst_data;

                if (av_packet_get_side_data(pkt, src_sd->type, NULL))
                    continue;

                dst_data = av_packet_new_side_data(pkt, src_sd->type, src_sd->size);
                if (!dst_data) {
                    av_log(s, AV_LOG_WARNING, "Could not inject global side data\n");
                    continue;
                }

                memcpy(dst_data, src_sd->data, src_sd->size);
            }
            sti->inject_global_side_data = 0;
        }
    }

    av_opt_get_dict_val(s, "metadata", AV_OPT_SEARCH_CHILDREN, &metadata);
    if (metadata) {
        s->event_flags |= AVFMT_EVENT_FLAG_METADATA_UPDATED;
        av_dict_copy(&s->metadata, metadata, 0);
        av_dict_free(&metadata);
        av_opt_set_dict_val(s, "metadata", NULL, AV_OPT_SEARCH_CHILDREN);
    }

    if (s->debug & FF_FDEBUG_TS)
        av_log(s, AV_LOG_DEBUG,
               "read_frame_internal stream=%d, pts=%s, dts=%s, "
               "size=%d, duration=%"PRId64", flags=%d\n",
               pkt->stream_index,
               av_ts2str(pkt->pts),
               av_ts2str(pkt->dts),
               pkt->size, pkt->duration, pkt->flags);

    /* A demuxer might have returned EOF because of an IO error, let's
     * propagate this back to the user. */
    if (ret == AVERROR_EOF && s->pb && s->pb->error < 0 && s->pb->error != AVERROR(EAGAIN))
        ret = s->pb->error;

    return ret;
}

int av_read_frame(AVFormatContext *s, AVPacket *pkt)
{
    FFFormatContext *const si = ffformatcontext(s);
    const int genpts = s->flags & AVFMT_FLAG_GENPTS;
    int eof = 0;
    int ret;
    AVStream *st;

    if (!genpts) {
        ret = si->packet_buffer
              ? avpriv_packet_list_get(&si->packet_buffer,
                                       &si->packet_buffer_end, pkt)
              : read_frame_internal(s, pkt);
        if (ret < 0)
            return ret;
        goto return_packet;
    }

    for (;;) {
        PacketList *pktl = si->packet_buffer;

        if (pktl) {
            AVPacket *next_pkt = &pktl->pkt;

            if (next_pkt->dts != AV_NOPTS_VALUE) {
                int wrap_bits = s->streams[next_pkt->stream_index]->pts_wrap_bits;
                // last dts seen for this stream. if any of packets following
                // current one had no dts, we will set this to AV_NOPTS_VALUE.
                int64_t last_dts = next_pkt->dts;
                av_assert2(wrap_bits <= 64);
                while (pktl && next_pkt->pts == AV_NOPTS_VALUE) {
                    if (pktl->pkt.stream_index == next_pkt->stream_index &&
                        av_compare_mod(next_pkt->dts, pktl->pkt.dts, 2ULL << (wrap_bits - 1)) < 0) {
                        if (av_compare_mod(pktl->pkt.pts, pktl->pkt.dts, 2ULL << (wrap_bits - 1))) {
                            // not B-frame
                            next_pkt->pts = pktl->pkt.dts;
                        }
                        if (last_dts != AV_NOPTS_VALUE) {
                            // Once last dts was set to AV_NOPTS_VALUE, we don't change it.
                            last_dts = pktl->pkt.dts;
                        }
                    }
                    pktl = pktl->next;
                }
                if (eof && next_pkt->pts == AV_NOPTS_VALUE && last_dts != AV_NOPTS_VALUE) {
                    // Fixing the last reference frame had none pts issue (For MXF etc).
                    // We only do this when
                    // 1. eof.
                    // 2. we are not able to resolve a pts value for current packet.
                    // 3. the packets for this stream at the end of the files had valid dts.
                    next_pkt->pts = last_dts + next_pkt->duration;
                }
                pktl = si->packet_buffer;
            }

            /* read packet from packet buffer, if there is data */
            st = s->streams[next_pkt->stream_index];
            if (!(next_pkt->pts == AV_NOPTS_VALUE && st->discard < AVDISCARD_ALL &&
                  next_pkt->dts != AV_NOPTS_VALUE && !eof)) {
                ret = avpriv_packet_list_get(&si->packet_buffer,
                                             &si->packet_buffer_end, pkt);
                goto return_packet;
            }
        }

        ret = read_frame_internal(s, pkt);
        if (ret < 0) {
            if (pktl && ret != AVERROR(EAGAIN)) {
                eof = 1;
                continue;
            } else
                return ret;
        }

        ret = avpriv_packet_list_put(&si->packet_buffer,
                                     &si->packet_buffer_end,
                                 pkt, NULL, 0);
        if (ret < 0) {
            av_packet_unref(pkt);
            return ret;
        }
    }

return_packet:

    st = s->streams[pkt->stream_index];
    if ((s->iformat->flags & AVFMT_GENERIC_INDEX) && pkt->flags & AV_PKT_FLAG_KEY) {
        ff_reduce_index(s, st->index);
        av_add_index_entry(st, pkt->pos, pkt->dts, 0, 0, AVINDEX_KEYFRAME);
    }

    if (is_relative(pkt->dts))
        pkt->dts -= RELATIVE_TS_BASE;
    if (is_relative(pkt->pts))
        pkt->pts -= RELATIVE_TS_BASE;

    return ret;
}

/* XXX: suppress the packet queue */
void ff_flush_packet_queue(AVFormatContext *s)
{
    FFFormatContext *const si = ffformatcontext(s);
    avpriv_packet_list_free(&si->parse_queue,       &si->parse_queue_end);
    avpriv_packet_list_free(&si->packet_buffer,     &si->packet_buffer_end);
    avpriv_packet_list_free(&si->raw_packet_buffer, &si->raw_packet_buffer_end);

    si->raw_packet_buffer_remaining_size = RAW_PACKET_BUFFER_SIZE;
}

int av_find_default_stream_index(AVFormatContext *s)
{
    int best_stream = 0;
    int best_score = INT_MIN;

    if (s->nb_streams <= 0)
        return -1;
    for (unsigned i = 0; i < s->nb_streams; i++) {
        const AVStream *const st = s->streams[i];
        const FFStream *const sti = cffstream(st);
        int score = 0;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
                score -= 400;
            if (st->codecpar->width && st->codecpar->height)
                score += 50;
            score+= 25;
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (st->codecpar->sample_rate)
                score += 50;
        }
        if (sti->codec_info_nb_frames)
            score += 12;

        if (st->discard != AVDISCARD_ALL)
            score += 200;

        if (score > best_score) {
            best_score = score;
            best_stream = i;
        }
    }
    return best_stream;
}

/*******************************************************/

/**
 * Return TRUE if the stream has accurate duration in any stream.
 *
 * @return TRUE if the stream has accurate duration for at least one component.
 */
static int has_duration(AVFormatContext *ic)
{
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        const AVStream *const st = ic->streams[i];
        if (st->duration != AV_NOPTS_VALUE)
            return 1;
    }
    if (ic->duration != AV_NOPTS_VALUE)
        return 1;
    return 0;
}

/**
 * Estimate the stream timings from the one of each components.
 *
 * Also computes the global bitrate if possible.
 */
static void update_stream_timings(AVFormatContext *ic)
{
    int64_t start_time, start_time1, start_time_text, end_time, end_time1, end_time_text;
    int64_t duration, duration1, duration_text, filesize;

    start_time = INT64_MAX;
    start_time_text = INT64_MAX;
    end_time   = INT64_MIN;
    end_time_text   = INT64_MIN;
    duration   = INT64_MIN;
    duration_text = INT64_MIN;

    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        int is_text = st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE ||
                      st->codecpar->codec_type == AVMEDIA_TYPE_DATA;

        if (st->start_time != AV_NOPTS_VALUE && st->time_base.den) {
            start_time1 = av_rescale_q(st->start_time, st->time_base,
                                       AV_TIME_BASE_Q);
            if (is_text)
                start_time_text = FFMIN(start_time_text, start_time1);
            else
                start_time = FFMIN(start_time, start_time1);
            end_time1 = av_rescale_q_rnd(st->duration, st->time_base,
                                         AV_TIME_BASE_Q,
                                         AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
            if (end_time1 != AV_NOPTS_VALUE && (end_time1 > 0 ? start_time1 <= INT64_MAX - end_time1 : start_time1 >= INT64_MIN - end_time1)) {
                end_time1 += start_time1;
                if (is_text)
                    end_time_text = FFMAX(end_time_text, end_time1);
                else
                    end_time = FFMAX(end_time, end_time1);
            }
            for (AVProgram *p = NULL; (p = av_find_program_from_stream(ic, p, i)); ) {
                if (p->start_time == AV_NOPTS_VALUE || p->start_time > start_time1)
                    p->start_time = start_time1;
                if (p->end_time < end_time1)
                    p->end_time = end_time1;
            }
        }
        if (st->duration != AV_NOPTS_VALUE) {
            duration1 = av_rescale_q(st->duration, st->time_base,
                                     AV_TIME_BASE_Q);
            if (is_text)
                duration_text = FFMAX(duration_text, duration1);
            else
                duration = FFMAX(duration, duration1);
        }
    }
    if (start_time == INT64_MAX || (start_time > start_time_text && start_time - (uint64_t)start_time_text < AV_TIME_BASE))
        start_time = start_time_text;
    else if (start_time > start_time_text)
        av_log(ic, AV_LOG_VERBOSE, "Ignoring outlier non primary stream starttime %f\n", start_time_text / (float)AV_TIME_BASE);

    if (end_time == INT64_MIN || (end_time < end_time_text && end_time_text - (uint64_t)end_time < AV_TIME_BASE))
        end_time = end_time_text;
    else if (end_time < end_time_text)
        av_log(ic, AV_LOG_VERBOSE, "Ignoring outlier non primary stream endtime %f\n", end_time_text / (float)AV_TIME_BASE);

     if (duration == INT64_MIN || (duration < duration_text && duration_text - duration < AV_TIME_BASE))
         duration = duration_text;
     else if (duration < duration_text)
         av_log(ic, AV_LOG_VERBOSE, "Ignoring outlier non primary stream duration %f\n", duration_text / (float)AV_TIME_BASE);

    if (start_time != INT64_MAX) {
        ic->start_time = start_time;
        if (end_time != INT64_MIN) {
            if (ic->nb_programs > 1) {
                for (unsigned i = 0; i < ic->nb_programs; i++) {
                    AVProgram *const p = ic->programs[i];

                    if (p->start_time != AV_NOPTS_VALUE &&
                        p->end_time > p->start_time &&
                        p->end_time - (uint64_t)p->start_time <= INT64_MAX)
                        duration = FFMAX(duration, p->end_time - p->start_time);
                }
            } else if (end_time >= start_time && end_time - (uint64_t)start_time <= INT64_MAX) {
                duration = FFMAX(duration, end_time - start_time);
            }
        }
    }
    if (duration != INT64_MIN && duration > 0 && ic->duration == AV_NOPTS_VALUE) {
        ic->duration = duration;
    }
    if (ic->pb && (filesize = avio_size(ic->pb)) > 0 && ic->duration > 0) {
        /* compute the bitrate */
        double bitrate = (double) filesize * 8.0 * AV_TIME_BASE /
                         (double) ic->duration;
        if (bitrate >= 0 && bitrate <= INT64_MAX)
            ic->bit_rate = bitrate;
    }
}

static void fill_all_stream_timings(AVFormatContext *ic)
{
    update_stream_timings(ic);
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *const st = ic->streams[i];

        if (st->start_time == AV_NOPTS_VALUE) {
            if (ic->start_time != AV_NOPTS_VALUE)
                st->start_time = av_rescale_q(ic->start_time, AV_TIME_BASE_Q,
                                              st->time_base);
            if (ic->duration != AV_NOPTS_VALUE)
                st->duration = av_rescale_q(ic->duration, AV_TIME_BASE_Q,
                                            st->time_base);
        }
    }
}

static void estimate_timings_from_bit_rate(AVFormatContext *ic)
{
    FFFormatContext *const si = ffformatcontext(ic);
    int show_warning = 0;

    /* if bit_rate is already set, we believe it */
    if (ic->bit_rate <= 0) {
        int64_t bit_rate = 0;
        for (unsigned i = 0; i < ic->nb_streams; i++) {
            const AVStream *const st  = ic->streams[i];
            const FFStream *const sti = cffstream(st);
            if (st->codecpar->bit_rate <= 0 && sti->avctx->bit_rate > 0)
                st->codecpar->bit_rate = sti->avctx->bit_rate;
            if (st->codecpar->bit_rate > 0) {
                if (INT64_MAX - st->codecpar->bit_rate < bit_rate) {
                    bit_rate = 0;
                    break;
                }
                bit_rate += st->codecpar->bit_rate;
            } else if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && sti->codec_info_nb_frames > 1) {
                // If we have a videostream with packets but without a bitrate
                // then consider the sum not known
                bit_rate = 0;
                break;
            }
        }
        ic->bit_rate = bit_rate;
    }

    /* if duration is already set, we believe it */
    if (ic->duration == AV_NOPTS_VALUE &&
        ic->bit_rate != 0) {
        int64_t filesize = ic->pb ? avio_size(ic->pb) : 0;
        if (filesize > si->data_offset) {
            filesize -= si->data_offset;
            for (unsigned i = 0; i < ic->nb_streams; i++) {
                AVStream *const st = ic->streams[i];

                if (   st->time_base.num <= INT64_MAX / ic->bit_rate
                    && st->duration == AV_NOPTS_VALUE) {
                    st->duration = av_rescale(filesize, 8LL * st->time_base.den,
                                          ic->bit_rate *
                                          (int64_t) st->time_base.num);
                    show_warning = 1;
                }
            }
        }
    }
    if (show_warning)
        av_log(ic, AV_LOG_WARNING,
               "Estimating duration from bitrate, this may be inaccurate\n");
}

/** Maximum number of bytes we read to determine timing from PTS stream. */
#define DURATION_MAX_READ_SIZE 250000LL
#define DURATION_MAX_RETRY 6

/**
 * @brief Estimates timings using PTS stream.
 *
 * only usable for MPEG-PS streams
 */
static void estimate_timings_from_pts(AVFormatContext *ic, int64_t old_offset)
{
    FFFormatContext *const si = ffformatcontext(ic);
    AVPacket *const pkt = si->pkt;
    int num, den, read_size, ret;
    int found_duration = 0;
    int is_end;
    int64_t filesize, offset, duration;
    int retry = 0;

    /* flush packet queue */
    ff_flush_packet_queue(ic);

    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *const st  = ic->streams[i];
        FFStream *const sti = ffstream(st);

        if (st->start_time == AV_NOPTS_VALUE &&
            sti->first_dts == AV_NOPTS_VALUE &&
            st->codecpar->codec_type != AVMEDIA_TYPE_UNKNOWN)
            av_log(ic, AV_LOG_WARNING,
                   "start time for stream %d is not set in estimate_timings_from_pts\n", i);

        if (sti->parser) {
            av_parser_close(sti->parser);
            sti->parser = NULL;
        }
    }

    if (ic->skip_estimate_duration_from_pts) {
        av_log(ic, AV_LOG_INFO, "Skipping duration calculation in estimate_timings_from_pts\n");
        goto skip_duration_calc;
    }

    av_opt_set(ic, "skip_changes", "1", AV_OPT_SEARCH_CHILDREN);
    /* estimate the end time (duration) */
    /* XXX: may need to support wrapping */
    filesize = ic->pb ? avio_size(ic->pb) : 0;
    do {
        is_end = found_duration;
        offset = filesize - (DURATION_MAX_READ_SIZE << retry);
        if (offset < 0)
            offset = 0;

        avio_seek(ic->pb, offset, SEEK_SET);
        read_size = 0;
        for (;;) {
            AVStream *st;
            FFStream *sti;
            if (read_size >= DURATION_MAX_READ_SIZE << (FFMAX(retry - 1, 0)))
                break;

            do {
                ret = ff_read_packet(ic, pkt);
            } while (ret == AVERROR(EAGAIN));
            if (ret != 0)
                break;
            read_size += pkt->size;

            if (pkt->stream_index >= ic->nb_streams)
            {
                av_packet_unref(pkt);
                continue;
            }

            st         = ic->streams[pkt->stream_index];
            sti        = ffstream(st);
            if (pkt->pts != AV_NOPTS_VALUE &&
                (st->start_time != AV_NOPTS_VALUE ||
                 sti->first_dts != AV_NOPTS_VALUE)) {
                if (pkt->duration == 0) {
                    compute_frame_duration(ic, &num, &den, st, sti->parser, pkt);
                    if (den && num) {
                        pkt->duration = av_rescale_rnd(1,
                                           num * (int64_t) st->time_base.den,
                                           den * (int64_t) st->time_base.num,
                                           AV_ROUND_DOWN);
                    }
                }
                duration = pkt->pts + pkt->duration;
                found_duration = 1;
                if (st->start_time != AV_NOPTS_VALUE)
                    duration -= st->start_time;
                else
                    duration -= sti->first_dts;
                if (duration > 0) {
                    if (st->duration == AV_NOPTS_VALUE || sti->info->last_duration<= 0 ||
                        (st->duration < duration && FFABS(duration - sti->info->last_duration) < 60LL*st->time_base.den / st->time_base.num))
                        st->duration = duration;
                    sti->info->last_duration = duration;
                }
            }
            av_packet_unref(pkt);
        }

        /* check if all audio/video streams have valid duration */
        if (!is_end) {
            is_end = 1;
            for (unsigned i = 0; i < ic->nb_streams; i++) {
                const AVStream *const st = ic->streams[i];
                switch (st->codecpar->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                    case AVMEDIA_TYPE_AUDIO:
                        if (st->duration == AV_NOPTS_VALUE)
                            is_end = 0;
                }
            }
        }
    } while (!is_end &&
             offset &&
             ++retry <= DURATION_MAX_RETRY);

    av_opt_set(ic, "skip_changes", "0", AV_OPT_SEARCH_CHILDREN);

    /* warn about audio/video streams which duration could not be estimated */
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        const AVStream *const st = ic->streams[i];
        const FFStream *const sti = cffstream(st);

        if (st->duration == AV_NOPTS_VALUE) {
            switch (st->codecpar->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
            case AVMEDIA_TYPE_AUDIO:
                if (st->start_time != AV_NOPTS_VALUE || sti->first_dts  != AV_NOPTS_VALUE) {
                    av_log(ic, AV_LOG_WARNING, "stream %d : no PTS found at end of file, duration not set\n", i);
                } else
                    av_log(ic, AV_LOG_WARNING, "stream %d : no TS found at start of file, duration not set\n", i);
            }
        }
    }
skip_duration_calc:
    fill_all_stream_timings(ic);

    avio_seek(ic->pb, old_offset, SEEK_SET);
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *const st  = ic->streams[i];
        FFStream *const sti = ffstream(st);

        sti->cur_dts     = sti->first_dts;
        sti->last_IP_pts = AV_NOPTS_VALUE;
        sti->last_dts_for_order_check = AV_NOPTS_VALUE;
        for (int j = 0; j < MAX_REORDER_DELAY + 1; j++)
            sti->pts_buffer[j] = AV_NOPTS_VALUE;
    }
}

/* 1:1 map to AVDurationEstimationMethod */
static const char *const duration_name[] = {
    [AVFMT_DURATION_FROM_PTS]     = "pts",
    [AVFMT_DURATION_FROM_STREAM]  = "stream",
    [AVFMT_DURATION_FROM_BITRATE] = "bit rate",
};

static const char *duration_estimate_name(enum AVDurationEstimationMethod method)
{
    return duration_name[method];
}

static void estimate_timings(AVFormatContext *ic, int64_t old_offset)
{
    int64_t file_size;

    /* get the file size, if possible */
    if (ic->iformat->flags & AVFMT_NOFILE) {
        file_size = 0;
    } else {
        file_size = avio_size(ic->pb);
        file_size = FFMAX(0, file_size);
    }

    if ((!strcmp(ic->iformat->name, "mpeg") ||
         !strcmp(ic->iformat->name, "mpegts") ||
         !strcmp(ic->iformat->name, "mpegts-ffmpeg")) &&
        file_size && (ic->pb->seekable & AVIO_SEEKABLE_NORMAL)) {
        /* get accurate estimate from the PTSes */
        estimate_timings_from_pts(ic, old_offset);
        ic->duration_estimation_method = AVFMT_DURATION_FROM_PTS;
    } else if (has_duration(ic)) {
        /* at least one component has timings - we use them for all
         * the components */
        fill_all_stream_timings(ic);
        /* nut demuxer estimate the duration from PTS */
        if(!strcmp(ic->iformat->name, "nut"))
            ic->duration_estimation_method = AVFMT_DURATION_FROM_PTS;
        else
            ic->duration_estimation_method = AVFMT_DURATION_FROM_STREAM;
    } else {
        /* less precise: use bitrate info */
        estimate_timings_from_bit_rate(ic);
        ic->duration_estimation_method = AVFMT_DURATION_FROM_BITRATE;
    }
    update_stream_timings(ic);

    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        if (st->time_base.den)
            av_log(ic, AV_LOG_TRACE, "stream %u: start_time: %s duration: %s\n", i,
                   av_ts2timestr(st->start_time, &st->time_base),
                   av_ts2timestr(st->duration, &st->time_base));
    }
    av_log(ic, AV_LOG_TRACE,
           "format: start_time: %s duration: %s (estimate from %s) bitrate=%"PRId64" kb/s\n",
           av_ts2timestr(ic->start_time, &AV_TIME_BASE_Q),
           av_ts2timestr(ic->duration, &AV_TIME_BASE_Q),
           duration_estimate_name(ic->duration_estimation_method),
           (int64_t)ic->bit_rate / 1000);
}

static int has_codec_parameters(AVStream *st, const char **errmsg_ptr)
{
    FFStream *const sti = ffstream(st);
    AVCodecContext *avctx = sti->avctx;

#define FAIL(errmsg) do {                                         \
        if (errmsg_ptr)                                           \
            *errmsg_ptr = errmsg;                                 \
        return 0;                                                 \
    } while (0)

    if (   avctx->codec_id == AV_CODEC_ID_NONE
        && avctx->codec_type != AVMEDIA_TYPE_DATA)
        FAIL("unknown codec");
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        if (!avctx->frame_size && determinable_frame_size(avctx))
            FAIL("unspecified frame size");
        if (sti->info->found_decoder >= 0 &&
            avctx->sample_fmt == AV_SAMPLE_FMT_NONE)
            FAIL("unspecified sample format");
        if (!avctx->sample_rate)
            FAIL("unspecified sample rate");
        if (!avctx->channels)
            FAIL("unspecified number of channels");
        if (sti->info->found_decoder >= 0 && !sti->nb_decoded_frames && avctx->codec_id == AV_CODEC_ID_DTS)
            FAIL("no decodable DTS frames");
        break;
    case AVMEDIA_TYPE_VIDEO:
        if (!avctx->width)
            FAIL("unspecified size");
        if (sti->info->found_decoder >= 0 && avctx->pix_fmt == AV_PIX_FMT_NONE)
            FAIL("unspecified pixel format");
        if (st->codecpar->codec_id == AV_CODEC_ID_RV30 || st->codecpar->codec_id == AV_CODEC_ID_RV40)
            if (!st->sample_aspect_ratio.num && !st->codecpar->sample_aspect_ratio.num && !sti->codec_info_nb_frames)
                FAIL("no frame in rv30/40 and no sar");
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        if (avctx->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE && !avctx->width)
            FAIL("unspecified size");
        break;
    case AVMEDIA_TYPE_DATA:
        if (avctx->codec_id == AV_CODEC_ID_NONE) return 1;
    }

    return 1;
}

/* returns 1 or 0 if or if not decoded data was returned, or a negative error */
static int try_decode_frame(AVFormatContext *s, AVStream *st,
                            const AVPacket *avpkt, AVDictionary **options)
{
    FFStream *const sti = ffstream(st);
    AVCodecContext *avctx = sti->avctx;
    const AVCodec *codec;
    int got_picture = 1, ret = 0;
    AVFrame *frame = av_frame_alloc();
    AVSubtitle subtitle;
    AVPacket pkt = *avpkt;
    int do_skip_frame = 0;
    enum AVDiscard skip_frame;

    if (!frame)
        return AVERROR(ENOMEM);

    if (!avcodec_is_open(avctx) &&
        sti->info->found_decoder <= 0 &&
        (st->codecpar->codec_id != -sti->info->found_decoder || !st->codecpar->codec_id)) {
        AVDictionary *thread_opt = NULL;

        codec = find_probe_decoder(s, st, st->codecpar->codec_id);

        if (!codec) {
            sti->info->found_decoder = -st->codecpar->codec_id;
            ret                     = -1;
            goto fail;
        }

        /* Force thread count to 1 since the H.264 decoder will not extract
         * SPS and PPS to extradata during multi-threaded decoding. */
        av_dict_set(options ? options : &thread_opt, "threads", "1", 0);
        /* Force lowres to 0. The decoder might reduce the video size by the
         * lowres factor, and we don't want that propagated to the stream's
         * codecpar */
        av_dict_set(options ? options : &thread_opt, "lowres", "0", 0);
        if (s->codec_whitelist)
            av_dict_set(options ? options : &thread_opt, "codec_whitelist", s->codec_whitelist, 0);
        ret = avcodec_open2(avctx, codec, options ? options : &thread_opt);
        if (!options)
            av_dict_free(&thread_opt);
        if (ret < 0) {
            sti->info->found_decoder = -avctx->codec_id;
            goto fail;
        }
        sti->info->found_decoder = 1;
    } else if (!sti->info->found_decoder)
        sti->info->found_decoder = 1;

    if (sti->info->found_decoder < 0) {
        ret = -1;
        goto fail;
    }

    if (avpriv_codec_get_cap_skip_frame_fill_param(avctx->codec)) {
        do_skip_frame = 1;
        skip_frame = avctx->skip_frame;
        avctx->skip_frame = AVDISCARD_ALL;
    }

    while ((pkt.size > 0 || (!pkt.data && got_picture)) &&
           ret >= 0 &&
           (!has_codec_parameters(st, NULL) || !has_decode_delay_been_guessed(st) ||
            (!sti->codec_info_nb_frames &&
             (avctx->codec->capabilities & AV_CODEC_CAP_CHANNEL_CONF)))) {
        got_picture = 0;
        if (avctx->codec_type == AVMEDIA_TYPE_VIDEO ||
            avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            ret = avcodec_send_packet(avctx, &pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                break;
            if (ret >= 0)
                pkt.size = 0;
            ret = avcodec_receive_frame(avctx, frame);
            if (ret >= 0)
                got_picture = 1;
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                ret = 0;
        } else if (avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            ret = avcodec_decode_subtitle2(avctx, &subtitle,
                                           &got_picture, &pkt);
            if (got_picture)
                avsubtitle_free(&subtitle);
            if (ret >= 0)
                pkt.size = 0;
        }
        if (ret >= 0) {
            if (got_picture)
                sti->nb_decoded_frames++;
            ret       = got_picture;
        }
    }

    if (!pkt.data && !got_picture)
        ret = -1;

fail:
    if (do_skip_frame) {
        avctx->skip_frame = skip_frame;
    }

    av_frame_free(&frame);
    return ret;
}

unsigned int ff_codec_get_tag(const AVCodecTag *tags, enum AVCodecID id)
{
    while (tags->id != AV_CODEC_ID_NONE) {
        if (tags->id == id)
            return tags->tag;
        tags++;
    }
    return 0;
}

enum AVCodecID ff_codec_get_id(const AVCodecTag *tags, unsigned int tag)
{
    for (int i = 0; tags[i].id != AV_CODEC_ID_NONE; i++)
        if (tag == tags[i].tag)
            return tags[i].id;
    for (int i = 0; tags[i].id != AV_CODEC_ID_NONE; i++)
        if (avpriv_toupper4(tag) == avpriv_toupper4(tags[i].tag))
            return tags[i].id;
    return AV_CODEC_ID_NONE;
}

enum AVCodecID ff_get_pcm_codec_id(int bps, int flt, int be, int sflags)
{
    if (bps <= 0 || bps > 64)
        return AV_CODEC_ID_NONE;

    if (flt) {
        switch (bps) {
        case 32:
            return be ? AV_CODEC_ID_PCM_F32BE : AV_CODEC_ID_PCM_F32LE;
        case 64:
            return be ? AV_CODEC_ID_PCM_F64BE : AV_CODEC_ID_PCM_F64LE;
        default:
            return AV_CODEC_ID_NONE;
        }
    } else {
        bps  += 7;
        bps >>= 3;
        if (sflags & (1 << (bps - 1))) {
            switch (bps) {
            case 1:
                return AV_CODEC_ID_PCM_S8;
            case 2:
                return be ? AV_CODEC_ID_PCM_S16BE : AV_CODEC_ID_PCM_S16LE;
            case 3:
                return be ? AV_CODEC_ID_PCM_S24BE : AV_CODEC_ID_PCM_S24LE;
            case 4:
                return be ? AV_CODEC_ID_PCM_S32BE : AV_CODEC_ID_PCM_S32LE;
            case 8:
                return be ? AV_CODEC_ID_PCM_S64BE : AV_CODEC_ID_PCM_S64LE;
            default:
                return AV_CODEC_ID_NONE;
            }
        } else {
            switch (bps) {
            case 1:
                return AV_CODEC_ID_PCM_U8;
            case 2:
                return be ? AV_CODEC_ID_PCM_U16BE : AV_CODEC_ID_PCM_U16LE;
            case 3:
                return be ? AV_CODEC_ID_PCM_U24BE : AV_CODEC_ID_PCM_U24LE;
            case 4:
                return be ? AV_CODEC_ID_PCM_U32BE : AV_CODEC_ID_PCM_U32LE;
            default:
                return AV_CODEC_ID_NONE;
            }
        }
    }
}

unsigned int av_codec_get_tag(const AVCodecTag *const *tags, enum AVCodecID id)
{
    unsigned int tag;
    if (!av_codec_get_tag2(tags, id, &tag))
        return 0;
    return tag;
}

int av_codec_get_tag2(const AVCodecTag * const *tags, enum AVCodecID id,
                      unsigned int *tag)
{
    for (int i = 0; tags && tags[i]; i++) {
        const AVCodecTag *codec_tags = tags[i];
        while (codec_tags->id != AV_CODEC_ID_NONE) {
            if (codec_tags->id == id) {
                *tag = codec_tags->tag;
                return 1;
            }
            codec_tags++;
        }
    }
    return 0;
}

enum AVCodecID av_codec_get_id(const AVCodecTag *const *tags, unsigned int tag)
{
    for (int i = 0; tags && tags[i]; i++) {
        enum AVCodecID id = ff_codec_get_id(tags[i], tag);
        if (id != AV_CODEC_ID_NONE)
            return id;
    }
    return AV_CODEC_ID_NONE;
}

static int chapter_start_cmp(const void *p1, const void *p2)
{
    AVChapter *ch1 = *(AVChapter**)p1;
    AVChapter *ch2 = *(AVChapter**)p2;
    int delta = av_compare_ts(ch1->start, ch1->time_base, ch2->start, ch2->time_base);
    if (delta)
        return delta;
    return FFDIFFSIGN(ch1->id, ch2->id);
}

static int compute_chapters_end(AVFormatContext *s)
{
    int64_t max_time = 0;
    AVChapter **timetable;

    if (!s->nb_chapters)
        return 0;

    if (s->duration > 0 && s->start_time < INT64_MAX - s->duration)
        max_time = s->duration +
                       ((s->start_time == AV_NOPTS_VALUE) ? 0 : s->start_time);

    timetable = av_memdup(s->chapters, s->nb_chapters * sizeof(*timetable));
    if (!timetable)
        return AVERROR(ENOMEM);
    qsort(timetable, s->nb_chapters, sizeof(*timetable), chapter_start_cmp);

    for (unsigned i = 0; i < s->nb_chapters; i++)
        if (timetable[i]->end == AV_NOPTS_VALUE) {
            AVChapter *ch = timetable[i];
            int64_t end = max_time ? av_rescale_q(max_time, AV_TIME_BASE_Q,
                                                ch->time_base)
                                : INT64_MAX;

            if (i + 1 < s->nb_chapters) {
                AVChapter *ch1     = timetable[i + 1];
                int64_t next_start = av_rescale_q(ch1->start, ch1->time_base,
                                                ch->time_base);
                if (next_start > ch->start && next_start < end)
                    end = next_start;
            }
            ch->end = (end == INT64_MAX || end < ch->start) ? ch->start : end;
        }
    av_free(timetable);
    return 0;
}

static int get_std_framerate(int i)
{
    if (i < 30*12)
        return (i + 1) * 1001;
    i -= 30*12;

    if (i < 30)
        return (i + 31) * 1001 * 12;
    i -= 30;

    if (i < 3)
        return ((const int[]) { 80, 120, 240})[i] * 1001 * 12;

    i -= 3;

    return ((const int[]) { 24, 30, 60, 12, 15, 48 })[i] * 1000 * 12;
}

/* Is the time base unreliable?
 * This is a heuristic to balance between quick acceptance of the values in
 * the headers vs. some extra checks.
 * Old DivX and Xvid often have nonsense timebases like 1fps or 2fps.
 * MPEG-2 commonly misuses field repeat flags to store different framerates.
 * And there are "variable" fps files this needs to detect as well. */
static int tb_unreliable(AVCodecContext *c)
{
    if (c->time_base.den >= 101LL * c->time_base.num ||
        c->time_base.den <    5LL * c->time_base.num ||
        // c->codec_tag == AV_RL32("DIVX") ||
        // c->codec_tag == AV_RL32("XVID") ||
        c->codec_tag == AV_RL32("mp4v") ||
        c->codec_id == AV_CODEC_ID_MPEG2VIDEO ||
        c->codec_id == AV_CODEC_ID_GIF ||
        c->codec_id == AV_CODEC_ID_HEVC ||
        c->codec_id == AV_CODEC_ID_H264)
        return 1;
    return 0;
}

int ff_alloc_extradata(AVCodecParameters *par, int size)
{
    av_freep(&par->extradata);
    par->extradata_size = 0;

    if (size < 0 || size >= INT32_MAX - AV_INPUT_BUFFER_PADDING_SIZE)
        return AVERROR(EINVAL);

    par->extradata = av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!par->extradata)
        return AVERROR(ENOMEM);

    memset(par->extradata + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    par->extradata_size = size;

    return 0;
}

int ff_get_extradata(AVFormatContext *s, AVCodecParameters *par, AVIOContext *pb, int size)
{
    int ret = ff_alloc_extradata(par, size);
    if (ret < 0)
        return ret;
    ret = ffio_read_size(pb, par->extradata, size);
    if (ret < 0) {
        av_freep(&par->extradata);
        par->extradata_size = 0;
        av_log(s, AV_LOG_ERROR, "Failed to read extradata of size %d\n", size);
        return ret;
    }

    return ret;
}

int ff_rfps_add_frame(AVFormatContext *ic, AVStream *st, int64_t ts)
{
    FFStream *const sti = ffstream(st);
    int64_t last = sti->info->last_dts;

    if (   ts != AV_NOPTS_VALUE && last != AV_NOPTS_VALUE && ts > last
       && ts - (uint64_t)last < INT64_MAX) {
        double dts = (is_relative(ts) ?  ts - RELATIVE_TS_BASE : ts) * av_q2d(st->time_base);
        int64_t duration = ts - last;

        if (!sti->info->duration_error)
            sti->info->duration_error = av_mallocz(sizeof(sti->info->duration_error[0])*2);
        if (!sti->info->duration_error)
            return AVERROR(ENOMEM);

//         if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO)
//             av_log(NULL, AV_LOG_ERROR, "%f\n", dts);
        for (int i = 0; i < MAX_STD_TIMEBASES; i++) {
            if (sti->info->duration_error[0][1][i] < 1e10) {
                int framerate = get_std_framerate(i);
                double sdts = dts*framerate/(1001*12);
                for (int j = 0; j < 2; j++) {
                    int64_t ticks = llrint(sdts+j*0.5);
                    double error= sdts - ticks + j*0.5;
                    sti->info->duration_error[j][0][i] += error;
                    sti->info->duration_error[j][1][i] += error*error;
                }
            }
        }
        if (sti->info->rfps_duration_sum <= INT64_MAX - duration) {
            sti->info->duration_count++;
            sti->info->rfps_duration_sum += duration;
        }

        if (sti->info->duration_count % 10 == 0) {
            int n = sti->info->duration_count;
            for (int i = 0; i < MAX_STD_TIMEBASES; i++) {
                if (sti->info->duration_error[0][1][i] < 1e10) {
                    double a0     = sti->info->duration_error[0][0][i] / n;
                    double error0 = sti->info->duration_error[0][1][i] / n - a0*a0;
                    double a1     = sti->info->duration_error[1][0][i] / n;
                    double error1 = sti->info->duration_error[1][1][i] / n - a1*a1;
                    if (error0 > 0.04 && error1 > 0.04) {
                        sti->info->duration_error[0][1][i] = 2e10;
                        sti->info->duration_error[1][1][i] = 2e10;
                    }
                }
            }
        }

        // ignore the first 4 values, they might have some random jitter
        if (sti->info->duration_count > 3 && is_relative(ts) == is_relative(last))
            sti->info->duration_gcd = av_gcd(sti->info->duration_gcd, duration);
    }
    if (ts != AV_NOPTS_VALUE)
        sti->info->last_dts = ts;

    return 0;
}

void ff_rfps_calculate(AVFormatContext *ic)
{
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        FFStream *const sti = ffstream(st);

        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;
        // the check for tb_unreliable() is not completely correct, since this is not about handling
        // an unreliable/inexact time base, but a time base that is finer than necessary, as e.g.
        // ipmovie.c produces.
        if (tb_unreliable(sti->avctx) && sti->info->duration_count > 15 && sti->info->duration_gcd > FFMAX(1, st->time_base.den/(500LL*st->time_base.num)) && !st->r_frame_rate.num &&
            sti->info->duration_gcd < INT64_MAX / st->time_base.num)
            av_reduce(&st->r_frame_rate.num, &st->r_frame_rate.den, st->time_base.den, st->time_base.num * sti->info->duration_gcd, INT_MAX);
        if (sti->info->duration_count > 1 && !st->r_frame_rate.num
            && tb_unreliable(sti->avctx)) {
            int num = 0;
            double best_error= 0.01;
            AVRational ref_rate = st->r_frame_rate.num ? st->r_frame_rate : av_inv_q(st->time_base);

            for (int j = 0; j < MAX_STD_TIMEBASES; j++) {
                if (sti->info->codec_info_duration &&
                    sti->info->codec_info_duration*av_q2d(st->time_base) < (1001*11.5)/get_std_framerate(j))
                    continue;
                if (!sti->info->codec_info_duration && get_std_framerate(j) < 1001*12)
                    continue;

                if (av_q2d(st->time_base) * sti->info->rfps_duration_sum / sti->info->duration_count < (1001*12.0 * 0.8)/get_std_framerate(j))
                    continue;

                for (int k = 0; k < 2; k++) {
                    int n = sti->info->duration_count;
                    double a = sti->info->duration_error[k][0][j] / n;
                    double error = sti->info->duration_error[k][1][j]/n - a*a;

                    if (error < best_error && best_error> 0.000000001) {
                        best_error= error;
                        num = get_std_framerate(j);
                    }
                    if (error < 0.02)
                        av_log(ic, AV_LOG_DEBUG, "rfps: %f %f\n", get_std_framerate(j) / 12.0/1001, error);
                }
            }
            // do not increase frame rate by more than 1 % in order to match a standard rate.
            if (num && (!ref_rate.num || (double)num/(12*1001) < 1.01 * av_q2d(ref_rate)))
                av_reduce(&st->r_frame_rate.num, &st->r_frame_rate.den, num, 12*1001, INT_MAX);
        }
        if (   !st->avg_frame_rate.num
            && st->r_frame_rate.num && sti->info->rfps_duration_sum
            && sti->info->codec_info_duration <= 0
            && sti->info->duration_count > 2
            && fabs(1.0 / (av_q2d(st->r_frame_rate) * av_q2d(st->time_base)) - sti->info->rfps_duration_sum / (double)sti->info->duration_count) <= 1.0
            ) {
            av_log(ic, AV_LOG_DEBUG, "Setting avg frame rate based on r frame rate\n");
            st->avg_frame_rate = st->r_frame_rate;
        }

        av_freep(&sti->info->duration_error);
        sti->info->last_dts = AV_NOPTS_VALUE;
        sti->info->duration_count = 0;
        sti->info->rfps_duration_sum = 0;
    }
}

static int extract_extradata_check(AVStream *st)
{
    const AVBitStreamFilter *f;

    f = av_bsf_get_by_name("extract_extradata");
    if (!f)
        return 0;

    if (f->codec_ids) {
        const enum AVCodecID *ids;
        for (ids = f->codec_ids; *ids != AV_CODEC_ID_NONE; ids++)
            if (*ids == st->codecpar->codec_id)
                return 1;
    }

    return 0;
}

static int extract_extradata_init(AVStream *st)
{
    FFStream *sti = ffstream(st);
    const AVBitStreamFilter *f;
    int ret;

    f = av_bsf_get_by_name("extract_extradata");
    if (!f)
        goto finish;

    /* check that the codec id is supported */
    ret = extract_extradata_check(st);
    if (!ret)
        goto finish;

    ret = av_bsf_alloc(f, &sti->extract_extradata.bsf);
    if (ret < 0)
        return ret;

    ret = avcodec_parameters_copy(sti->extract_extradata.bsf->par_in,
                                  st->codecpar);
    if (ret < 0)
        goto fail;

    sti->extract_extradata.bsf->time_base_in = st->time_base;

    ret = av_bsf_init(sti->extract_extradata.bsf);
    if (ret < 0)
        goto fail;

finish:
    sti->extract_extradata.inited = 1;

    return 0;
fail:
    av_bsf_free(&sti->extract_extradata.bsf);
    return ret;
}

static int extract_extradata(FFFormatContext *si, AVStream *st, const AVPacket *pkt)
{
    FFStream *sti = ffstream(st);
    AVPacket *pkt_ref = si->parse_pkt;
    int ret;

    if (!sti->extract_extradata.inited) {
        ret = extract_extradata_init(st);
        if (ret < 0)
            return ret;
    }

    if (sti->extract_extradata.inited && !sti->extract_extradata.bsf)
        return 0;

    ret = av_packet_ref(pkt_ref, pkt);
    if (ret < 0)
        return ret;

    ret = av_bsf_send_packet(sti->extract_extradata.bsf, pkt_ref);
    if (ret < 0) {
        av_packet_unref(pkt_ref);
        return ret;
    }

    while (ret >= 0 && !sti->avctx->extradata) {
        ret = av_bsf_receive_packet(sti->extract_extradata.bsf, pkt_ref);
        if (ret < 0) {
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                return ret;
            continue;
        }

        for (int i = 0; i < pkt_ref->side_data_elems; i++) {
            AVPacketSideData *side_data = &pkt_ref->side_data[i];
            if (side_data->type == AV_PKT_DATA_NEW_EXTRADATA) {
                sti->avctx->extradata      = side_data->data;
                sti->avctx->extradata_size = side_data->size;
                side_data->data = NULL;
                side_data->size = 0;
                break;
            }
        }
        av_packet_unref(pkt_ref);
    }

    return 0;
}

/* absolute maximum size we read until we abort (MythTV) */
#define MAX_READ_SIZE        5000000

/** Number of frames to read, max. (MythTV) */
#define MAX_FRAMES           45

static int add_coded_side_data(AVStream *st, AVCodecContext *avctx)
{
    for (int i = 0; i < avctx->nb_coded_side_data; i++) {
        const AVPacketSideData *sd_src = &avctx->coded_side_data[i];
        uint8_t *dst_data;
        dst_data = av_stream_new_side_data(st, sd_src->type, sd_src->size);
        if (!dst_data)
            return AVERROR(ENOMEM);
        memcpy(dst_data, sd_src->data, sd_src->size);
    }
    return 0;
}

int avformat_find_stream_info(AVFormatContext *ic, AVDictionary **options)
{
    FFFormatContext *const si = ffformatcontext(ic);
    int count = 0, ret = 0;
    int64_t read_size;
    AVPacket *pkt1 = si->pkt;
    int64_t old_offset  = avio_tell(ic->pb);
    // new streams might appear, no options for those
    int orig_nb_streams = ic->nb_streams;
    int flush_codecs;
    int64_t max_analyze_duration = ic->max_analyze_duration;
    int64_t max_stream_analyze_duration;
    int64_t max_subtitle_analyze_duration;
    int64_t probesize = ic->probesize;
    int eof_reached = 0;
    int *missing_streams = av_opt_ptr(ic->iformat->priv_class, ic->priv_data, "missing_streams");

    int hasaudio        = 0;
    int hasvideo        = 0;
    int read_packets    = 0;

    flush_codecs = probesize > 0;

    av_opt_set(ic, "skip_clear", "1", AV_OPT_SEARCH_CHILDREN);

    max_stream_analyze_duration = max_analyze_duration;
    max_subtitle_analyze_duration = max_analyze_duration;
    if (!max_analyze_duration) {
        max_stream_analyze_duration =
        max_analyze_duration        = 5*AV_TIME_BASE;
        max_subtitle_analyze_duration = 30*AV_TIME_BASE;
        if (!strcmp(ic->iformat->name, "flv"))
            max_stream_analyze_duration = 90*AV_TIME_BASE;
        if (!strcmp(ic->iformat->name, "mpeg") || !strcmp(ic->iformat->name, "mpegts") || !strcmp(ic->iformat->name, "mpegts-ffmpeg"))
            max_stream_analyze_duration = 7*AV_TIME_BASE;
    }

    if (ic->pb) {
        FFIOContext *const ctx = ffiocontext(ic->pb);
        av_log(ic, AV_LOG_DEBUG, "Before avformat_find_stream_info() pos: %"PRId64" bytes read:%"PRId64" seeks:%d nb_streams:%d\n",
               avio_tell(ic->pb), ctx->bytes_read, ctx->seek_count, ic->nb_streams);
    }

    for (unsigned i = 0; i < ic->nb_streams; i++) {
        const AVCodec *codec;
        AVDictionary *thread_opt = NULL;
        AVStream *const st  = ic->streams[i];
        FFStream *const sti = ffstream(st);
        AVCodecContext *const avctx = sti->avctx;

        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
            st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
/*            if (!st->time_base.num)
                st->time_base = */
            if (!avctx->time_base.num)
                avctx->time_base = st->time_base;
        }

        /* check if the caller has overridden the codec id */
        // only for the split stuff
        if (!sti->parser && !(ic->flags & AVFMT_FLAG_NOPARSE) && sti->request_probe <= 0) {
            sti->parser = av_parser_init(st->codecpar->codec_id);
            if (sti->parser) {
                if (sti->need_parsing == AVSTREAM_PARSE_HEADERS) {
                    sti->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
                } else if (sti->need_parsing == AVSTREAM_PARSE_FULL_RAW) {
                    sti->parser->flags |= PARSER_FLAG_USE_CODEC_TS;
                }
            } else if (sti->need_parsing) {
                av_log(ic, AV_LOG_VERBOSE, "parser not found for codec "
                       "%s, packets or times may be invalid.\n",
                       avcodec_get_name(st->codecpar->codec_id));
            }
        }

        ret = avcodec_parameters_to_context(avctx, st->codecpar);
        if (ret < 0)
            goto find_stream_info_err;
        if (sti->request_probe <= 0)
            sti->avctx_inited = 1;

        codec = find_probe_decoder(ic, st, st->codecpar->codec_id);

        /* Force thread count to 1 since the H.264 decoder will not extract
         * SPS and PPS to extradata during multi-threaded decoding. */
        av_dict_set(options ? &options[i] : &thread_opt, "threads", "1", 0);
        /* Force lowres to 0. The decoder might reduce the video size by the
         * lowres factor, and we don't want that propagated to the stream's
         * codecpar */
        av_dict_set(options ? &options[i] : &thread_opt, "lowres", "0", 0);

        if (ic->codec_whitelist)
            av_dict_set(options ? &options[i] : &thread_opt, "codec_whitelist", ic->codec_whitelist, 0);

        // Try to just open decoders, in case this is enough to get parameters.
        // Also ensure that subtitle_header is properly set.
        if (!has_codec_parameters(st, NULL) && sti->request_probe <= 0 ||
            st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            if (codec && !avctx->codec)
                if (avcodec_open2(avctx, codec, options ? &options[i] : &thread_opt) < 0)
                    av_log(ic, AV_LOG_WARNING,
                           "Failed to open codec in %s\n",__FUNCTION__);
        }
        if (!options)
            av_dict_free(&thread_opt);
    }

    read_size = 0;
    for (;;) {
        const AVPacket *pkt;
        AVStream *st;
        FFStream *sti;
        AVCodecContext *avctx;
        int analyzed_all_streams;
        unsigned i;
        if (ff_check_interrupt(&ic->interrupt_callback)) {
            ret = AVERROR_EXIT;
            av_log(ic, AV_LOG_DEBUG, "interrupted\n");
            break;
        }

        /* check if one codec still needs to be handled */
        for (i = 0; i < ic->nb_streams; i++) {
            AVStream *const st  = ic->streams[i];
            FFStream *const sti = ffstream(st);
            int fps_analyze_framecount = 20;
            int count;

            if (!has_codec_parameters(st, NULL))
                break;
            /* If the timebase is coarse (like the usual millisecond precision
             * of mkv), we need to analyze more frames to reliably arrive at
             * the correct fps. */
            if (av_q2d(st->time_base) > 0.0005)
                fps_analyze_framecount *= 2;
            if (!tb_unreliable(sti->avctx))
                fps_analyze_framecount = 0;
            if (ic->fps_probe_size >= 0)
                fps_analyze_framecount = ic->fps_probe_size;
            if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
                fps_analyze_framecount = 0;
            /* variable fps and no guess at the real fps */
            count = (ic->iformat->flags & AVFMT_NOTIMESTAMPS) ?
                       sti->info->codec_info_duration_fields/2 :
                       sti->info->duration_count;
            if (!(st->r_frame_rate.num && st->avg_frame_rate.num) &&
                st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (count < fps_analyze_framecount)
                    break;
            }
            // Look at the first 3 frames if there is evidence of frame delay
            // but the decoder delay is not set.
            if (sti->info->frame_delay_evidence && count < 2 && sti->avctx->has_b_frames == 0)
                break;
            if (!sti->avctx->extradata &&
                (!sti->extract_extradata.inited || sti->extract_extradata.bsf) &&
                extract_extradata_check(st))
                break;
            if (sti->first_dts == AV_NOPTS_VALUE &&
                !(ic->iformat->flags & AVFMT_NOTIMESTAMPS) &&
                sti->codec_info_nb_frames < ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) ? 1 : ic->max_ts_probe) &&
                st->codecpar->codec_id != AV_CODEC_ID_DSMCC_B)
                 break;
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                hasvideo = 1;
            else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                hasaudio = 1;
        }
        analyzed_all_streams = 0;
        if (!missing_streams || !*missing_streams)
            if (i == ic->nb_streams) {
                analyzed_all_streams = 1;
                /* NOTE: If the format has no header, then we need to read some
                 * packets to get most of the streams, so we cannot stop here. */
                /* There are MythTV changes here */
                if (!(ic->ctx_flags & AVFMTCTX_NOHEADER) ||
                    (read_size >= MAX_READ_SIZE || read_packets >= MAX_FRAMES) ||
                    (hasvideo && hasaudio)) {
                    /* If we found the info for all the codecs, we can stop. */
                    ret = count;
                    av_log(ic, AV_LOG_DEBUG, "All info found\n");
                    flush_codecs = 0;
                    break;
                }
                /* Is this is an MHEG only stream? Then we really can stop. */
                if (i == 1 && ic->streams[0]->codecpar->codec_id == AV_CODEC_ID_DSMCC_B)
                {
                    ret = count;
                    av_log(ic, AV_LOG_DEBUG, "All DSM info found\n");
                    break;
                }
            }
        /* We did not get all the codec info, but we read too much data. */
        if (read_size >= probesize) {
            ret = count;
            av_log(ic, AV_LOG_DEBUG,
                   "Probe buffer size limit of %"PRId64" bytes reached\n", probesize);
            for (unsigned i = 0; i < ic->nb_streams; i++) {
                AVStream *const st = ic->streams[i];
                FFStream *const sti = ffstream(st);
                if (!st->r_frame_rate.num &&
                    sti->info->duration_count <= 1 &&
                    st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                    strcmp(ic->iformat->name, "image2"))
                    av_log(ic, AV_LOG_WARNING,
                           "Stream #%d: not enough frames to estimate rate; "
                           "consider increasing probesize\n", i);
            }
            break;
        }

        /* NOTE: A new stream can be added there if no header in file
         * (AVFMTCTX_NOHEADER). */
        ret = read_frame_internal(ic, pkt1);

        read_packets++;

        if (ret == AVERROR(EAGAIN))
            continue;

        if (ret < 0) {
            /* EOF or error*/
            eof_reached = 1;
            break;
        }

        if (!(ic->flags & AVFMT_FLAG_NOBUFFER)) {
            ret = avpriv_packet_list_put(&si->packet_buffer,
                                         &si->packet_buffer_end,
                                     pkt1, NULL, 0);
            if (ret < 0)
                goto unref_then_goto_end;

            pkt = &si->packet_buffer_end->pkt;
        } else {
            pkt = pkt1;
        }

        st = ic->streams[pkt->stream_index];
        sti = ffstream(st);
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC))
            read_size += pkt->size;

        avctx = sti->avctx;
        if (!sti->avctx_inited) {
            ret = avcodec_parameters_to_context(avctx, st->codecpar);
            if (ret < 0)
                goto unref_then_goto_end;
            sti->avctx_inited = 1;
        }

        if (pkt->dts != AV_NOPTS_VALUE && sti->codec_info_nb_frames > 1) {
            /* check for non-increasing dts */
            if (sti->info->fps_last_dts != AV_NOPTS_VALUE &&
                sti->info->fps_last_dts >= pkt->dts) {
                av_log(ic, AV_LOG_DEBUG,
                       "Non-increasing DTS in stream %d: packet %d with DTS "
                       "%"PRId64", packet %d with DTS %"PRId64"\n",
                       st->index, sti->info->fps_last_dts_idx,
                       sti->info->fps_last_dts, sti->codec_info_nb_frames,
                       pkt->dts);
                sti->info->fps_first_dts =
                sti->info->fps_last_dts  = AV_NOPTS_VALUE;
            }
            /* Check for a discontinuity in dts. If the difference in dts
             * is more than 1000 times the average packet duration in the
             * sequence, we treat it as a discontinuity. */
            if (sti->info->fps_last_dts != AV_NOPTS_VALUE &&
                sti->info->fps_last_dts_idx > sti->info->fps_first_dts_idx &&
                (pkt->dts - (uint64_t)sti->info->fps_last_dts) / 1000 >
                (sti->info->fps_last_dts     - (uint64_t)sti->info->fps_first_dts) /
                (sti->info->fps_last_dts_idx - sti->info->fps_first_dts_idx)) {
                av_log(ic, AV_LOG_WARNING,
                       "DTS discontinuity in stream %d: packet %d with DTS "
                       "%"PRId64", packet %d with DTS %"PRId64"\n",
                       st->index, sti->info->fps_last_dts_idx,
                       sti->info->fps_last_dts, sti->codec_info_nb_frames,
                       pkt->dts);
                sti->info->fps_first_dts =
                sti->info->fps_last_dts  = AV_NOPTS_VALUE;
            }

            /* update stored dts values */
            if (sti->info->fps_first_dts == AV_NOPTS_VALUE) {
                sti->info->fps_first_dts     = pkt->dts;
                sti->info->fps_first_dts_idx = sti->codec_info_nb_frames;
            }
            sti->info->fps_last_dts     = pkt->dts;
            sti->info->fps_last_dts_idx = sti->codec_info_nb_frames;
        }
        if (sti->codec_info_nb_frames > 1) {
            int64_t t = 0;
            int64_t limit;

            if (st->time_base.den > 0)
                t = av_rescale_q(sti->info->codec_info_duration, st->time_base, AV_TIME_BASE_Q);
            if (st->avg_frame_rate.num > 0)
                t = FFMAX(t, av_rescale_q(sti->codec_info_nb_frames, av_inv_q(st->avg_frame_rate), AV_TIME_BASE_Q));

            if (   t == 0
                && sti->codec_info_nb_frames > 30
                && sti->info->fps_first_dts != AV_NOPTS_VALUE
                && sti->info->fps_last_dts  != AV_NOPTS_VALUE) {
                int64_t dur = av_sat_sub64(sti->info->fps_last_dts, sti->info->fps_first_dts);
                t = FFMAX(t, av_rescale_q(dur, st->time_base, AV_TIME_BASE_Q));
            }

            if (analyzed_all_streams)                                limit = max_analyze_duration;
            else if (avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) limit = max_subtitle_analyze_duration;
            else                                                     limit = max_stream_analyze_duration;

            if (t >= limit) {
                av_log(ic, AV_LOG_VERBOSE, "max_analyze_duration %"PRId64" reached at %"PRId64" microseconds st:%d\n",
                       limit,
                       t, pkt->stream_index);
                if (ic->flags & AVFMT_FLAG_NOBUFFER)
                    av_packet_unref(pkt1);
                break;
            }
            if (pkt->duration > 0) {
                if (avctx->codec_type == AVMEDIA_TYPE_SUBTITLE && pkt->pts != AV_NOPTS_VALUE && st->start_time != AV_NOPTS_VALUE && pkt->pts >= st->start_time
                    && (uint64_t)pkt->pts - st->start_time < INT64_MAX
                ) {
                    sti->info->codec_info_duration = FFMIN(pkt->pts - st->start_time, sti->info->codec_info_duration + pkt->duration);
                } else
                    sti->info->codec_info_duration += pkt->duration;
                sti->info->codec_info_duration_fields += sti->parser && sti->need_parsing && avctx->ticks_per_frame == 2
                                                         ? sti->parser->repeat_pict + 1 : 2;
            }
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
#if FF_API_R_FRAME_RATE
            ff_rfps_add_frame(ic, st, pkt->dts);
#endif
            if (pkt->dts != pkt->pts && pkt->dts != AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE)
                sti->info->frame_delay_evidence = 1;
        }
        if (!sti->avctx->extradata) {
            ret = extract_extradata(si, st, pkt);
            if (ret < 0)
                goto unref_then_goto_end;
        }

        /* If still no information, we try to open the codec and to
         * decompress the frame. We try to avoid that in most cases as
         * it takes longer and uses more memory. For MPEG-4, we need to
         * decompress for QuickTime.
         *
         * If AV_CODEC_CAP_CHANNEL_CONF is set this will force decoding of at
         * least one frame of codec data, this makes sure the codec initializes
         * the channel configuration and does not only trust the values from
         * the container. */
        try_decode_frame(ic, st, pkt,
                         (options && i < orig_nb_streams) ? &options[i] : NULL);

        if (ic->flags & AVFMT_FLAG_NOBUFFER)
            av_packet_unref(pkt1);

        sti->codec_info_nb_frames++;
        count++;
    }

    if (eof_reached) {
        for (unsigned stream_index = 0; stream_index < ic->nb_streams; stream_index++) {
            AVStream *const st = ic->streams[stream_index];
            AVCodecContext *const avctx = ffstream(st)->avctx;
            if (!has_codec_parameters(st, NULL)) {
                const AVCodec *codec = find_probe_decoder(ic, st, st->codecpar->codec_id);
                if (codec && !avctx->codec) {
                    AVDictionary *opts = NULL;
                    if (ic->codec_whitelist)
                        av_dict_set(&opts, "codec_whitelist", ic->codec_whitelist, 0);
                    if (avcodec_open2(avctx, codec, (options && stream_index < orig_nb_streams) ? &options[stream_index] : &opts) < 0)
                        av_log(ic, AV_LOG_WARNING,
                               "Failed to open codec in %s\n",__FUNCTION__);
                    av_dict_free(&opts);
                }
            }

            // EOF already reached while reading the stream above.
            // So continue with reoordering DTS with whatever delay we have.
            if (si->packet_buffer && !has_decode_delay_been_guessed(st)) {
                update_dts_from_pts(ic, stream_index, si->packet_buffer);
            }
        }
    }

    if (flush_codecs) {
        AVPacket *empty_pkt = si->pkt;
        int err = 0;
        av_packet_unref(empty_pkt);

        for (unsigned i = 0; i < ic->nb_streams; i++) {
            AVStream *const st = ic->streams[i];
            FFStream *const sti = ffstream(st);

            /* flush the decoders */
            if (sti->info->found_decoder == 1) {
                do {
                    err = try_decode_frame(ic, st, empty_pkt,
                                            (options && i < orig_nb_streams)
                                            ? &options[i] : NULL);
                } while (err > 0 && !has_codec_parameters(st, NULL));

                if (err < 0) {
                    av_log(ic, AV_LOG_INFO,
                        "decoding for stream %d failed\n", st->index);
                }
            }
        }
    }

    ff_rfps_calculate(ic);

    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream         *const st  = ic->streams[i];
        FFStream *const sti = ffstream(st);
        AVCodecContext *const avctx = sti->avctx;

        if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (avctx->codec_id == AV_CODEC_ID_RAWVIDEO && !avctx->codec_tag && !avctx->bits_per_coded_sample) {
                uint32_t tag= avcodec_pix_fmt_to_codec_tag(avctx->pix_fmt);
                if (avpriv_find_pix_fmt(avpriv_get_raw_pix_fmt_tags(), tag) == avctx->pix_fmt)
                    avctx->codec_tag= tag;
            }

            /* estimate average framerate if not set by demuxer */
            if (sti->info->codec_info_duration_fields &&
                !st->avg_frame_rate.num &&
                sti->info->codec_info_duration) {
                int best_fps      = 0;
                double best_error = 0.01;
                AVRational codec_frame_rate = avctx->framerate;

                if (sti->info->codec_info_duration        >= INT64_MAX / st->time_base.num / 2||
                    sti->info->codec_info_duration_fields >= INT64_MAX / st->time_base.den ||
                    sti->info->codec_info_duration        < 0)
                    continue;
                av_reduce(&st->avg_frame_rate.num, &st->avg_frame_rate.den,
                          sti->info->codec_info_duration_fields * (int64_t) st->time_base.den,
                          sti->info->codec_info_duration * 2 * (int64_t) st->time_base.num, 60000);

                /* Round guessed framerate to a "standard" framerate if it's
                 * within 1% of the original estimate. */
                for (int j = 0; j < MAX_STD_TIMEBASES; j++) {
                    AVRational std_fps = { get_std_framerate(j), 12 * 1001 };
                    double error       = fabs(av_q2d(st->avg_frame_rate) /
                                              av_q2d(std_fps) - 1);

                    if (error < best_error) {
                        best_error = error;
                        best_fps   = std_fps.num;
                    }

                    if (si->prefer_codec_framerate && codec_frame_rate.num > 0 && codec_frame_rate.den > 0) {
                        error       = fabs(av_q2d(codec_frame_rate) /
                                           av_q2d(std_fps) - 1);
                        if (error < best_error) {
                            best_error = error;
                            best_fps   = std_fps.num;
                        }
                    }
                }
                if (best_fps)
                    av_reduce(&st->avg_frame_rate.num, &st->avg_frame_rate.den,
                              best_fps, 12 * 1001, INT_MAX);
            }

            if (!st->r_frame_rate.num) {
                if (    avctx->time_base.den * (int64_t) st->time_base.num
                    <= avctx->time_base.num * (uint64_t)avctx->ticks_per_frame * st->time_base.den) {
                    av_reduce(&st->r_frame_rate.num, &st->r_frame_rate.den,
                              avctx->time_base.den, (int64_t)avctx->time_base.num * avctx->ticks_per_frame, INT_MAX);
                } else {
                    st->r_frame_rate.num = st->time_base.den;
                    st->r_frame_rate.den = st->time_base.num;
                }
            }
            if (sti->display_aspect_ratio.num && sti->display_aspect_ratio.den) {
                AVRational hw_ratio = { avctx->height, avctx->width };
                st->sample_aspect_ratio = av_mul_q(sti->display_aspect_ratio,
                                                   hw_ratio);
            }
        } else if (avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (!avctx->bits_per_coded_sample)
                avctx->bits_per_coded_sample =
                    av_get_bits_per_sample(avctx->codec_id);
            // set stream disposition based on audio service type
            switch (avctx->audio_service_type) {
            case AV_AUDIO_SERVICE_TYPE_EFFECTS:
                st->disposition = AV_DISPOSITION_CLEAN_EFFECTS;
                break;
            case AV_AUDIO_SERVICE_TYPE_VISUALLY_IMPAIRED:
                st->disposition = AV_DISPOSITION_VISUAL_IMPAIRED;
                break;
            case AV_AUDIO_SERVICE_TYPE_HEARING_IMPAIRED:
                st->disposition = AV_DISPOSITION_HEARING_IMPAIRED;
                break;
            case AV_AUDIO_SERVICE_TYPE_COMMENTARY:
                st->disposition = AV_DISPOSITION_COMMENT;
                break;
            case AV_AUDIO_SERVICE_TYPE_KARAOKE:
                st->disposition = AV_DISPOSITION_KARAOKE;
                break;
            }
        }
    }

    if (probesize)
        estimate_timings(ic, old_offset);

    av_opt_set(ic, "skip_clear", "0", AV_OPT_SEARCH_CHILDREN);

    if (ret >= 0 && ic->nb_streams)
        /* We could not have all the codec parameters before EOF. */
        ret = -1;
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *const st  = ic->streams[i];
        FFStream *const sti = ffstream(st);
        const char *errmsg;

        /* if no packet was ever seen, update context now for has_codec_parameters */
        if (!sti->avctx_inited) {
            if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                st->codecpar->format == AV_SAMPLE_FMT_NONE)
                st->codecpar->format = sti->avctx->sample_fmt;
            ret = avcodec_parameters_to_context(sti->avctx, st->codecpar);
            if (ret < 0)
                goto find_stream_info_err;
        }
        if (!has_codec_parameters(st, &errmsg)) {
            char buf[256];
            avcodec_string(buf, sizeof(buf), sti->avctx, 0);
            av_log(ic, AV_LOG_WARNING,
                   "Could not find codec parameters for stream %d (%s): %s\n"
                   "Consider increasing the value for the 'analyzeduration' (%"PRId64") and 'probesize' (%"PRId64") options\n",
                   i, buf, errmsg, ic->max_analyze_duration, ic->probesize);
        } else {
            ret = 0;
        }
    }

    ret = compute_chapters_end(ic);
    if (ret < 0)
        goto find_stream_info_err;

    /* update the stream parameters from the internal codec contexts */
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *const st  = ic->streams[i];
        FFStream *const sti = ffstream(st);

        if (sti->avctx_inited) {
            ret = avcodec_parameters_from_context(st->codecpar, sti->avctx);
            if (ret < 0)
                goto find_stream_info_err;
            ret = add_coded_side_data(st, sti->avctx);
            if (ret < 0)
                goto find_stream_info_err;
        }

        sti->avctx_inited = 0;
    }

find_stream_info_err:
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *const st  = ic->streams[i];
        FFStream *const sti = ffstream(st);
        if (sti->info) {
            av_freep(&sti->info->duration_error);
            av_freep(&sti->info);
        }
        avcodec_close(sti->avctx);
        av_bsf_free(&sti->extract_extradata.bsf);
    }
    if (ic->pb) {
        FFIOContext *const ctx = ffiocontext(ic->pb);
        av_log(ic, AV_LOG_DEBUG, "After avformat_find_stream_info() pos: %"PRId64" bytes read:%"PRId64" seeks:%d frames:%d\n",
               avio_tell(ic->pb), ctx->bytes_read, ctx->seek_count, count);
    }
    return ret;

unref_then_goto_end:
    av_packet_unref(pkt1);
    goto find_stream_info_err;
}

AVProgram *av_find_program_from_stream(AVFormatContext *ic, AVProgram *last, int s)
{
    for (unsigned i = 0; i < ic->nb_programs; i++) {
        if (ic->programs[i] == last) {
            last = NULL;
        } else {
            if (!last)
                for (unsigned j = 0; j < ic->programs[i]->nb_stream_indexes; j++)
                    if (ic->programs[i]->stream_index[j] == s)
                        return ic->programs[i];
        }
    }
    return NULL;
}

int av_find_best_stream(AVFormatContext *ic, enum AVMediaType type,
                        int wanted_stream_nb, int related_stream,
                        const AVCodec **decoder_ret, int flags)
{
    int nb_streams = ic->nb_streams;
    int ret = AVERROR_STREAM_NOT_FOUND;
    int best_count = -1, best_multiframe = -1, best_disposition = -1;
    int count, multiframe, disposition;
    int64_t best_bitrate = -1;
    int64_t bitrate;
    unsigned *program = NULL;
    const AVCodec *decoder = NULL, *best_decoder = NULL;

    if (related_stream >= 0 && wanted_stream_nb < 0) {
        AVProgram *p = av_find_program_from_stream(ic, NULL, related_stream);
        if (p) {
            program    = p->stream_index;
            nb_streams = p->nb_stream_indexes;
        }
    }
    for (unsigned i = 0; i < nb_streams; i++) {
        int real_stream_index = program ? program[i] : i;
        AVStream *st          = ic->streams[real_stream_index];
        AVCodecParameters *par = st->codecpar;
        if (par->codec_type != type)
            continue;
        if (wanted_stream_nb >= 0 && real_stream_index != wanted_stream_nb)
            continue;
        if (type == AVMEDIA_TYPE_AUDIO && !(par->channels && par->sample_rate))
            continue;
        if (decoder_ret) {
            decoder = find_decoder(ic, st, par->codec_id);
            if (!decoder) {
                if (ret < 0)
                    ret = AVERROR_DECODER_NOT_FOUND;
                continue;
            }
        }
        disposition = !(st->disposition & (AV_DISPOSITION_HEARING_IMPAIRED | AV_DISPOSITION_VISUAL_IMPAIRED))
                      + !! (st->disposition & AV_DISPOSITION_DEFAULT);
        count = ffstream(st)->codec_info_nb_frames;
        bitrate = par->bit_rate;
        multiframe = FFMIN(5, count);
        if ((best_disposition >  disposition) ||
            (best_disposition == disposition && best_multiframe >  multiframe) ||
            (best_disposition == disposition && best_multiframe == multiframe && best_bitrate >  bitrate) ||
            (best_disposition == disposition && best_multiframe == multiframe && best_bitrate == bitrate && best_count >= count))
            continue;
        best_disposition = disposition;
        best_count   = count;
        best_bitrate = bitrate;
        best_multiframe = multiframe;
        ret          = real_stream_index;
        best_decoder = decoder;
        if (program && i == nb_streams - 1 && ret < 0) {
            program    = NULL;
            nb_streams = ic->nb_streams;
            /* no related stream found, try again with everything */
            i = 0;
        }
    }
    if (decoder_ret)
        *decoder_ret = best_decoder;
    return ret;
}

/*******************************************************/

int av_read_play(AVFormatContext *s)
{
    if (s->iformat->read_play)
        return s->iformat->read_play(s);
    if (s->pb)
        return avio_pause(s->pb, 0);
    return AVERROR(ENOSYS);
}

int av_read_pause(AVFormatContext *s)
{
    if (s->iformat->read_pause)
        return s->iformat->read_pause(s);
    if (s->pb)
        return avio_pause(s->pb, 1);
    return AVERROR(ENOSYS);
}

int ff_stream_encode_params_copy(AVStream *dst, const AVStream *src)
{
    int ret;

    dst->id                  = src->id;
    dst->time_base           = src->time_base;
    dst->nb_frames           = src->nb_frames;
    dst->disposition         = src->disposition;
    dst->sample_aspect_ratio = src->sample_aspect_ratio;
    dst->avg_frame_rate      = src->avg_frame_rate;
    dst->r_frame_rate        = src->r_frame_rate;

    av_dict_free(&dst->metadata);
    ret = av_dict_copy(&dst->metadata, src->metadata, 0);
    if (ret < 0)
        return ret;

    ret = avcodec_parameters_copy(dst->codecpar, src->codecpar);
    if (ret < 0)
        return ret;

    /* Free existing side data*/
    for (int i = 0; i < dst->nb_side_data; i++)
        av_free(dst->side_data[i].data);
    av_freep(&dst->side_data);
    dst->nb_side_data = 0;

    /* Copy side data if present */
    if (src->nb_side_data) {
        dst->side_data = av_mallocz_array(src->nb_side_data,
                                          sizeof(AVPacketSideData));
        if (!dst->side_data)
            return AVERROR(ENOMEM);
        dst->nb_side_data = src->nb_side_data;

        for (int i = 0; i < src->nb_side_data; i++) {
            uint8_t *data = av_memdup(src->side_data[i].data,
                                      src->side_data[i].size);
            if (!data)
                return AVERROR(ENOMEM);
            dst->side_data[i].type = src->side_data[i].type;
            dst->side_data[i].size = src->side_data[i].size;
            dst->side_data[i].data = data;
        }
    }

    return 0;
}

static void free_stream(AVStream **pst)
{
    AVStream *st = *pst;
    FFStream *const sti = ffstream(st);

    if (!st)
        return;

    for (int i = 0; i < st->nb_side_data; i++)
        av_freep(&st->side_data[i].data);
    av_freep(&st->side_data);

    if (st->attached_pic.data)
        av_packet_unref(&st->attached_pic);

    av_parser_close(sti->parser);
    avcodec_free_context(&sti->avctx);
    av_bsf_free(&sti->bsfc);
    av_freep(&sti->priv_pts);
    av_freep(&sti->index_entries);
    av_freep(&sti->probe_data.buf);

    av_bsf_free(&sti->extract_extradata.bsf);

    if (sti->info) {
        av_freep(&sti->info->duration_error);
        av_freep(&sti->info);
    }

    av_dict_free(&st->metadata);
    avcodec_parameters_free(&st->codecpar);
    av_freep(&st->priv_data);

    av_freep(pst);
}

void ff_free_stream(AVFormatContext *s, AVStream *st)
{
    av_assert0(s->nb_streams>0);
    av_assert0(s->streams[ s->nb_streams - 1 ] == st);

    free_stream(&s->streams[ --s->nb_streams ]);
}

void avformat_free_context(AVFormatContext *s)
{
    FFFormatContext *si;

    if (!s)
        return;
    si = ffformatcontext(s);

    if (s->oformat && s->oformat->deinit && si->initialized)
        s->oformat->deinit(s);

    av_opt_free(s);
    if (s->iformat && s->iformat->priv_class && s->priv_data)
        av_opt_free(s->priv_data);
    if (s->oformat && s->oformat->priv_class && s->priv_data)
        av_opt_free(s->priv_data);

    for (unsigned i = 0; i < s->nb_streams; i++)
        free_stream(&s->streams[i]);
    s->nb_streams = 0;

    for (unsigned i = 0; i < s->nb_programs; i++) {
        av_dict_free(&s->programs[i]->metadata);
        av_freep(&s->programs[i]->stream_index);
        av_freep(&s->programs[i]);
    }
    s->nb_programs = 0;

    av_freep(&s->programs);
    av_freep(&s->priv_data);
    while (s->nb_chapters--) {
        av_dict_free(&s->chapters[s->nb_chapters]->metadata);
        av_freep(&s->chapters[s->nb_chapters]);
    }
    av_freep(&s->chapters);
    av_dict_free(&s->metadata);
    av_dict_free(&si->id3v2_meta);
    av_packet_free(&si->pkt);
    av_packet_free(&si->parse_pkt);
    av_freep(&s->streams);
    ff_flush_packet_queue(s);
    av_freep(&s->url);
    av_free(s);
}

void avformat_close_input(AVFormatContext **ps)
{
    AVFormatContext *s;
    AVIOContext *pb;

    if (!ps || !*ps)
        return;

    s  = *ps;
    pb = s->pb;

    if ((s->iformat && strcmp(s->iformat->name, "image2") && s->iformat->flags & AVFMT_NOFILE) ||
        (s->flags & AVFMT_FLAG_CUSTOM_IO))
        pb = NULL;

    if (s->iformat)
        if (s->iformat->read_close)
            s->iformat->read_close(s);

    avformat_free_context(s);

    *ps = NULL;

    avio_close(pb);
}

AVStream *avformat_new_stream(AVFormatContext *s, const AVCodec *c)
{
    FFFormatContext *const si = ffformatcontext(s);
    FFStream *sti;
    AVStream *st;
    AVStream **streams;

    if (s->nb_streams >= s->max_streams) {
        av_log(s, AV_LOG_ERROR, "Number of streams exceeds max_streams parameter"
               " (%d), see the documentation if you wish to increase it\n",
               s->max_streams);
        return NULL;
    }
    streams = av_realloc_array(s->streams, s->nb_streams + 1, sizeof(*streams));
    if (!streams)
        return NULL;
    s->streams = streams;


    sti = av_mallocz(sizeof(*sti));
    if (!sti)
        return NULL;
    st = &sti->pub;

    st->codecpar = avcodec_parameters_alloc();
    if (!st->codecpar)
        goto fail;

    sti->avctx = avcodec_alloc_context3(NULL);
    if (!sti->avctx)
        goto fail;

    if (s->iformat) {
        sti->info = av_mallocz(sizeof(*sti->info));
        if (!sti->info)
            goto fail;

#if FF_API_R_FRAME_RATE
        sti->info->last_dts      = AV_NOPTS_VALUE;
#endif
        sti->info->fps_first_dts = AV_NOPTS_VALUE;
        sti->info->fps_last_dts  = AV_NOPTS_VALUE;

        /* default pts setting is MPEG-like */
        avpriv_set_pts_info(st, 33, 1, 90000);
        /* we set the current DTS to 0 so that formats without any timestamps
         * but durations get some timestamps, formats with some unknown
         * timestamps have their first few packets buffered and the
         * timestamps corrected before they are returned to the user */
        sti->cur_dts = RELATIVE_TS_BASE;
    } else {
        sti->cur_dts = AV_NOPTS_VALUE;
    }

    st->index      = s->nb_streams;
    st->start_time = AV_NOPTS_VALUE;
    st->duration   = AV_NOPTS_VALUE;
    sti->first_dts     = AV_NOPTS_VALUE;
    sti->probe_packets = s->max_probe_packets;
    sti->pts_wrap_reference = AV_NOPTS_VALUE;
    sti->pts_wrap_behavior  = AV_PTS_WRAP_IGNORE;

    sti->last_IP_pts = AV_NOPTS_VALUE;
    sti->last_dts_for_order_check = AV_NOPTS_VALUE;
    for (int i = 0; i < MAX_REORDER_DELAY + 1; i++)
        sti->pts_buffer[i] = AV_NOPTS_VALUE;

    st->sample_aspect_ratio = (AVRational) { 0, 1 };

    sti->inject_global_side_data = si->inject_global_side_data;

    sti->need_context_update = 1;

    s->streams[s->nb_streams++] = st;
    return st;
fail:
    free_stream(&st);
    return NULL;
}

AVProgram *av_new_program(AVFormatContext *ac, int id)
{
    AVProgram *program = NULL;
    int ret;

    av_log(ac, AV_LOG_TRACE, "new_program: id=0x%04x\n", id);

    for (unsigned i = 0; i < ac->nb_programs; i++)
        if (ac->programs[i]->id == id)
            program = ac->programs[i];

    if (!program) {
        program = av_mallocz(sizeof(AVProgram));
        if (!program)
            return NULL;
        ret = av_dynarray_add_nofree(&ac->programs, &ac->nb_programs, program);
        if (ret < 0) {
            av_free(program);
            return NULL;
        }
        program->discard = AVDISCARD_NONE;
        program->pmt_version = -1;
        program->id = id;
        program->pts_wrap_reference = AV_NOPTS_VALUE;
        program->pts_wrap_behavior = AV_PTS_WRAP_IGNORE;
        program->start_time =
        program->end_time   = AV_NOPTS_VALUE;
    }
    return program;
}

AVChapter *avpriv_new_chapter(AVFormatContext *s, int64_t id, AVRational time_base,
                              int64_t start, int64_t end, const char *title)
{
    FFFormatContext *const si = ffformatcontext(s);
    AVChapter *chapter = NULL;
    int ret;

    if (end != AV_NOPTS_VALUE && start > end) {
        av_log(s, AV_LOG_ERROR, "Chapter end time %"PRId64" before start %"PRId64"\n", end, start);
        return NULL;
    }

    if (!s->nb_chapters) {
        si->chapter_ids_monotonic = 1;
    } else if (!si->chapter_ids_monotonic || s->chapters[s->nb_chapters-1]->id >= id) {
        si->chapter_ids_monotonic = 0;
        for (unsigned i = 0; i < s->nb_chapters; i++)
            if (s->chapters[i]->id == id)
                chapter = s->chapters[i];
    }

    if (!chapter) {
        chapter = av_mallocz(sizeof(AVChapter));
        if (!chapter)
            return NULL;
        ret = av_dynarray_add_nofree(&s->chapters, &s->nb_chapters, chapter);
        if (ret < 0) {
            av_free(chapter);
            return NULL;
        }
    }
    av_dict_set(&chapter->metadata, "title", title, 0);
    chapter->id        = id;
    chapter->time_base = time_base;
    chapter->start     = start;
    chapter->end       = end;

    return chapter;
}

void av_program_add_stream_index(AVFormatContext *ac, int progid, unsigned idx)
{
    AVProgram *program = NULL;
    void *tmp;

    if (idx >= ac->nb_streams) {
        av_log(ac, AV_LOG_ERROR, "stream index %d is not valid\n", idx);
        return;
    }

    for (unsigned i = 0; i < ac->nb_programs; i++) {
        if (ac->programs[i]->id != progid)
            continue;
        program = ac->programs[i];
        for (unsigned j = 0; j < program->nb_stream_indexes; j++)
            if (program->stream_index[j] == idx)
                return;

        tmp = av_realloc_array(program->stream_index, program->nb_stream_indexes+1, sizeof(unsigned int));
        if (!tmp)
            return;
        program->stream_index = tmp;
        program->stream_index[program->nb_stream_indexes++] = idx;
        return;
    }
}

uint64_t ff_ntp_time(void)
{
    return (av_gettime() / 1000) * 1000 + NTP_OFFSET_US;
}

uint64_t ff_get_formatted_ntp_time(uint64_t ntp_time_us)
{
    uint64_t ntp_ts, frac_part, sec;
    uint32_t usec;

    //current ntp time in seconds and micro seconds
    sec = ntp_time_us / 1000000;
    usec = ntp_time_us % 1000000;

    //encoding in ntp timestamp format
    frac_part = usec * 0xFFFFFFFFULL;
    frac_part /= 1000000;

    if (sec > 0xFFFFFFFFULL)
        av_log(NULL, AV_LOG_WARNING, "NTP time format roll over detected\n");

    ntp_ts = sec << 32;
    ntp_ts |= frac_part;

    return ntp_ts;
}

uint64_t ff_parse_ntp_time(uint64_t ntp_ts)
{
    uint64_t sec = ntp_ts >> 32;
    uint64_t frac_part = ntp_ts & 0xFFFFFFFFULL;
    uint64_t usec = (frac_part * 1000000) / 0xFFFFFFFFULL;

    return (sec * 1000000) + usec;
}

int av_get_frame_filename2(char *buf, int buf_size, const char *path, int number, int flags)
{
    const char *p;
    char *q, buf1[20], c;
    int nd, len, percentd_found;

    q = buf;
    p = path;
    percentd_found = 0;
    for (;;) {
        c = *p++;
        if (c == '\0')
            break;
        if (c == '%') {
            do {
                nd = 0;
                while (av_isdigit(*p)) {
                    if (nd >= INT_MAX / 10 - 255)
                        goto fail;
                    nd = nd * 10 + *p++ - '0';
                }
                c = *p++;
            } while (av_isdigit(c));

            switch (c) {
            case '%':
                goto addchar;
            case 'd':
                if (!(flags & AV_FRAME_FILENAME_FLAGS_MULTIPLE) && percentd_found)
                    goto fail;
                percentd_found = 1;
                if (number < 0)
                    nd += 1;
                snprintf(buf1, sizeof(buf1), "%0*d", nd, number);
                len = strlen(buf1);
                if ((q - buf + len) > buf_size - 1)
                    goto fail;
                memcpy(q, buf1, len);
                q += len;
                break;
            default:
                goto fail;
            }
        } else {
addchar:
            if ((q - buf) < buf_size - 1)
                *q++ = c;
        }
    }
    if (!percentd_found)
        goto fail;
    *q = '\0';
    return 0;
fail:
    *q = '\0';
    return -1;
}

int av_get_frame_filename(char *buf, int buf_size, const char *path, int number)
{
    return av_get_frame_filename2(buf, buf_size, path, number, 0);
}

void av_url_split(char *proto, int proto_size,
                  char *authorization, int authorization_size,
                  char *hostname, int hostname_size,
                  int *port_ptr, char *path, int path_size, const char *url)
{
    const char *p, *ls, *at, *at2, *col, *brk;

    if (port_ptr)
        *port_ptr = -1;
    if (proto_size > 0)
        proto[0] = 0;
    if (authorization_size > 0)
        authorization[0] = 0;
    if (hostname_size > 0)
        hostname[0] = 0;
    if (path_size > 0)
        path[0] = 0;

    /* parse protocol */
    if ((p = strchr(url, ':'))) {
        av_strlcpy(proto, url, FFMIN(proto_size, p + 1 - url));
        p++; /* skip ':' */
        if (*p == '/')
            p++;
        if (*p == '/')
            p++;
    } else {
        /* no protocol means plain filename */
        av_strlcpy(path, url, path_size);
        return;
    }

    /* separate path from hostname */
    ls = p + strcspn(p, "/?#");
    av_strlcpy(path, ls, path_size);

    /* the rest is hostname, use that to parse auth/port */
    if (ls != p) {
        /* authorization (user[:pass]@hostname) */
        at2 = p;
        while ((at = strchr(p, '@')) && at < ls) {
            av_strlcpy(authorization, at2,
                       FFMIN(authorization_size, at + 1 - at2));
            p = at + 1; /* skip '@' */
        }

        if (*p == '[' && (brk = strchr(p, ']')) && brk < ls) {
            /* [host]:port */
            av_strlcpy(hostname, p + 1,
                       FFMIN(hostname_size, brk - p));
            if (brk[1] == ':' && port_ptr)
                *port_ptr = atoi(brk + 2);
        } else if ((col = strchr(p, ':')) && col < ls) {
            av_strlcpy(hostname, p,
                       FFMIN(col + 1 - p, hostname_size));
            if (port_ptr)
                *port_ptr = atoi(col + 1);
        } else
            av_strlcpy(hostname, p,
                       FFMIN(ls + 1 - p, hostname_size));
    }
}

int ff_mkdir_p(const char *path)
{
    int ret = 0;
    char *temp = av_strdup(path);
    char *pos = temp;
    char tmp_ch = '\0';

    if (!path || !temp) {
        return -1;
    }

    if (!av_strncasecmp(temp, "/", 1) || !av_strncasecmp(temp, "\\", 1)) {
        pos++;
    } else if (!av_strncasecmp(temp, "./", 2) || !av_strncasecmp(temp, ".\\", 2)) {
        pos += 2;
    }

    for ( ; *pos != '\0'; ++pos) {
        if (*pos == '/' || *pos == '\\') {
            tmp_ch = *pos;
            *pos = '\0';
            ret = mkdir(temp, 0755);
            *pos = tmp_ch;
        }
    }

    if ((*(pos - 1) != '/') || (*(pos - 1) != '\\')) {
        ret = mkdir(temp, 0755);
    }

    av_free(temp);
    return ret;
}

char *ff_data_to_hex(char *buff, const uint8_t *src, int s, int lowercase)
{
    static const char hex_table_uc[16] = { '0', '1', '2', '3',
                                           '4', '5', '6', '7',
                                           '8', '9', 'A', 'B',
                                           'C', 'D', 'E', 'F' };
    static const char hex_table_lc[16] = { '0', '1', '2', '3',
                                           '4', '5', '6', '7',
                                           '8', '9', 'a', 'b',
                                           'c', 'd', 'e', 'f' };
    const char *hex_table = lowercase ? hex_table_lc : hex_table_uc;

    for (int i = 0; i < s; i++) {
        buff[i * 2]     = hex_table[src[i] >> 4];
        buff[i * 2 + 1] = hex_table[src[i] & 0xF];
    }

    return buff;
}

int ff_hex_to_data(uint8_t *data, const char *p)
{
    int c, len, v;

    len = 0;
    v   = 1;
    for (;;) {
        p += strspn(p, SPACE_CHARS);
        if (*p == '\0')
            break;
        c = av_toupper((unsigned char) *p++);
        if (c >= '0' && c <= '9')
            c = c - '0';
        else if (c >= 'A' && c <= 'F')
            c = c - 'A' + 10;
        else
            break;
        v = (v << 4) | c;
        if (v & 0x100) {
            if (data)
                data[len] = v;
            len++;
            v = 1;
        }
    }
    return len;
}

void avpriv_set_pts_info(AVStream *s, int pts_wrap_bits,
                         unsigned int pts_num, unsigned int pts_den)
{
    FFStream *const sti = ffstream(s);
    AVRational new_tb;
    if (av_reduce(&new_tb.num, &new_tb.den, pts_num, pts_den, INT_MAX)) {
        if (new_tb.num != pts_num)
            av_log(NULL, AV_LOG_DEBUG,
                   "st:%d removing common factor %d from timebase\n",
                   s->index, pts_num / new_tb.num);
    } else
        av_log(NULL, AV_LOG_WARNING,
               "st:%d has too large timebase, reducing\n", s->index);

    if (new_tb.num <= 0 || new_tb.den <= 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Ignoring attempt to set invalid timebase %d/%d for st:%d\n",
               new_tb.num, new_tb.den,
               s->index);
        return;
    }
    s->time_base     = new_tb;
    sti->avctx->pkt_timebase = new_tb;
    s->pts_wrap_bits = pts_wrap_bits;
}

void ff_parse_key_value(const char *str, ff_parse_key_val_cb callback_get_buf,
                        void *context)
{
    const char *ptr = str;

    /* Parse key=value pairs. */
    for (;;) {
        const char *key;
        char *dest = NULL, *dest_end;
        int key_len, dest_len = 0;

        /* Skip whitespace and potential commas. */
        while (*ptr && (av_isspace(*ptr) || *ptr == ','))
            ptr++;
        if (!*ptr)
            break;

        key = ptr;

        if (!(ptr = strchr(key, '=')))
            break;
        ptr++;
        key_len = ptr - key;

        callback_get_buf(context, key, key_len, &dest, &dest_len);
        dest_end = dest + dest_len - 1;

        if (*ptr == '\"') {
            ptr++;
            while (*ptr && *ptr != '\"') {
                if (*ptr == '\\') {
                    if (!ptr[1])
                        break;
                    if (dest && dest < dest_end)
                        *dest++ = ptr[1];
                    ptr += 2;
                } else {
                    if (dest && dest < dest_end)
                        *dest++ = *ptr;
                    ptr++;
                }
            }
            if (*ptr == '\"')
                ptr++;
        } else {
            for (; *ptr && !(av_isspace(*ptr) || *ptr == ','); ptr++)
                if (dest && dest < dest_end)
                    *dest++ = *ptr;
        }
        if (dest)
            *dest = 0;
    }
}

int ff_find_stream_index(AVFormatContext *s, int id)
{
    for (unsigned i = 0; i < s->nb_streams; i++)
        if (s->streams[i]->id == id)
            return i;
    return -1;
}

int avformat_query_codec(const AVOutputFormat *ofmt, enum AVCodecID codec_id,
                         int std_compliance)
{
    if (ofmt) {
        unsigned int codec_tag;
        if (ofmt->query_codec)
            return ofmt->query_codec(codec_id, std_compliance);
        else if (ofmt->codec_tag)
            return !!av_codec_get_tag2(ofmt->codec_tag, codec_id, &codec_tag);
        else if (codec_id == ofmt->video_codec ||
                 codec_id == ofmt->audio_codec ||
                 codec_id == ofmt->subtitle_codec ||
                 codec_id == ofmt->data_codec)
            return 1;
    }
    return AVERROR_PATCHWELCOME;
}

int avformat_network_init(void)
{
#if CONFIG_NETWORK
    int ret;
    if ((ret = ff_network_init()) < 0)
        return ret;
    if ((ret = ff_tls_init()) < 0)
        return ret;
#endif
    return 0;
}

int avformat_network_deinit(void)
{
#if CONFIG_NETWORK
    ff_network_close();
    ff_tls_deinit();
#endif
    return 0;
}

int ff_add_param_change(AVPacket *pkt, int32_t channels,
                        uint64_t channel_layout, int32_t sample_rate,
                        int32_t width, int32_t height)
{
    uint32_t flags = 0;
    int size = 4;
    uint8_t *data;
    if (!pkt)
        return AVERROR(EINVAL);
    if (channels) {
        size  += 4;
        flags |= AV_SIDE_DATA_PARAM_CHANGE_CHANNEL_COUNT;
    }
    if (channel_layout) {
        size  += 8;
        flags |= AV_SIDE_DATA_PARAM_CHANGE_CHANNEL_LAYOUT;
    }
    if (sample_rate) {
        size  += 4;
        flags |= AV_SIDE_DATA_PARAM_CHANGE_SAMPLE_RATE;
    }
    if (width || height) {
        size  += 8;
        flags |= AV_SIDE_DATA_PARAM_CHANGE_DIMENSIONS;
    }
    data = av_packet_new_side_data(pkt, AV_PKT_DATA_PARAM_CHANGE, size);
    if (!data)
        return AVERROR(ENOMEM);
    bytestream_put_le32(&data, flags);
    if (channels)
        bytestream_put_le32(&data, channels);
    if (channel_layout)
        bytestream_put_le64(&data, channel_layout);
    if (sample_rate)
        bytestream_put_le32(&data, sample_rate);
    if (width || height) {
        bytestream_put_le32(&data, width);
        bytestream_put_le32(&data, height);
    }
    return 0;
}

AVRational av_guess_sample_aspect_ratio(AVFormatContext *format, AVStream *stream, AVFrame *frame)
{
    AVRational undef = {0, 1};
    AVRational stream_sample_aspect_ratio = stream ? stream->sample_aspect_ratio : undef;
    AVRational codec_sample_aspect_ratio  = stream && stream->codecpar ? stream->codecpar->sample_aspect_ratio : undef;
    AVRational frame_sample_aspect_ratio  = frame  ? frame->sample_aspect_ratio  : codec_sample_aspect_ratio;

    av_reduce(&stream_sample_aspect_ratio.num, &stream_sample_aspect_ratio.den,
               stream_sample_aspect_ratio.num,  stream_sample_aspect_ratio.den, INT_MAX);
    if (stream_sample_aspect_ratio.num <= 0 || stream_sample_aspect_ratio.den <= 0)
        stream_sample_aspect_ratio = undef;

    av_reduce(&frame_sample_aspect_ratio.num, &frame_sample_aspect_ratio.den,
               frame_sample_aspect_ratio.num,  frame_sample_aspect_ratio.den, INT_MAX);
    if (frame_sample_aspect_ratio.num <= 0 || frame_sample_aspect_ratio.den <= 0)
        frame_sample_aspect_ratio = undef;

    if (stream_sample_aspect_ratio.num)
        return stream_sample_aspect_ratio;
    else
        return frame_sample_aspect_ratio;
}

AVRational av_guess_frame_rate(AVFormatContext *format, AVStream *st, AVFrame *frame)
{
    AVRational fr = st->r_frame_rate;
    AVCodecContext *const avctx = ffstream(st)->avctx;
    AVRational codec_fr = avctx->framerate;
    AVRational   avg_fr = st->avg_frame_rate;

    if (avg_fr.num > 0 && avg_fr.den > 0 && fr.num > 0 && fr.den > 0 &&
        av_q2d(avg_fr) < 70 && av_q2d(fr) > 210) {
        fr = avg_fr;
    }


    if (avctx->ticks_per_frame > 1) {
        if (   codec_fr.num > 0 && codec_fr.den > 0 &&
            (fr.num == 0 || av_q2d(codec_fr) < av_q2d(fr)*0.7 && fabs(1.0 - av_q2d(av_div_q(avg_fr, fr))) > 0.1))
            fr = codec_fr;
    }

    return fr;
}

/**
 * Matches a stream specifier (but ignores requested index).
 *
 * @param indexptr set to point to the requested stream index if there is one
 *
 * @return <0 on error
 *         0  if st is NOT a matching stream
 *         >0 if st is a matching stream
 */
static int match_stream_specifier(AVFormatContext *s, AVStream *st,
                                  const char *spec, const char **indexptr, AVProgram **p)
{
    int match = 1;                      /* Stores if the specifier matches so far. */
    while (*spec) {
        if (*spec <= '9' && *spec >= '0') { /* opt:index */
            if (indexptr)
                *indexptr = spec;
            return match;
        } else if (*spec == 'v' || *spec == 'a' || *spec == 's' || *spec == 'd' ||
                   *spec == 't' || *spec == 'V') { /* opt:[vasdtV] */
            enum AVMediaType type;
            int nopic = 0;

            switch (*spec++) {
            case 'v': type = AVMEDIA_TYPE_VIDEO;      break;
            case 'a': type = AVMEDIA_TYPE_AUDIO;      break;
            case 's': type = AVMEDIA_TYPE_SUBTITLE;   break;
            case 'd': type = AVMEDIA_TYPE_DATA;       break;
            case 't': type = AVMEDIA_TYPE_ATTACHMENT; break;
            case 'V': type = AVMEDIA_TYPE_VIDEO; nopic = 1; break;
            default:  av_assert0(0);
            }
            if (*spec && *spec++ != ':')         /* If we are not at the end, then another specifier must follow. */
                return AVERROR(EINVAL);

            if (type != st->codecpar->codec_type)
                match = 0;
            if (nopic && (st->disposition & AV_DISPOSITION_ATTACHED_PIC))
                match = 0;
        } else if (*spec == 'p' && *(spec + 1) == ':') {
            int prog_id;
            int found = 0;
            char *endptr;
            spec += 2;
            prog_id = strtol(spec, &endptr, 0);
            /* Disallow empty id and make sure that if we are not at the end, then another specifier must follow. */
            if (spec == endptr || (*endptr && *endptr++ != ':'))
                return AVERROR(EINVAL);
            spec = endptr;
            if (match) {
                for (unsigned i = 0; i < s->nb_programs; i++) {
                    if (s->programs[i]->id != prog_id)
                        continue;

                    for (unsigned j = 0; j < s->programs[i]->nb_stream_indexes; j++) {
                        if (st->index == s->programs[i]->stream_index[j]) {
                            found = 1;
                            if (p)
                                *p = s->programs[i];
                            i = s->nb_programs;
                            break;
                        }
                    }
                }
            }
            if (!found)
                match = 0;
        } else if (*spec == '#' ||
                   (*spec == 'i' && *(spec + 1) == ':')) {
            int stream_id;
            char *endptr;
            spec += 1 + (*spec == 'i');
            stream_id = strtol(spec, &endptr, 0);
            if (spec == endptr || *endptr)                /* Disallow empty id and make sure we are at the end. */
                return AVERROR(EINVAL);
            return match && (stream_id == st->id);
        } else if (*spec == 'm' && *(spec + 1) == ':') {
            AVDictionaryEntry *tag;
            char *key, *val;
            int ret;

            if (match) {
               spec += 2;
               val = strchr(spec, ':');

               key = val ? av_strndup(spec, val - spec) : av_strdup(spec);
               if (!key)
                   return AVERROR(ENOMEM);

               tag = av_dict_get(st->metadata, key, NULL, 0);
               if (tag) {
                   if (!val || !strcmp(tag->value, val + 1))
                       ret = 1;
                   else
                       ret = 0;
               } else
                   ret = 0;

               av_freep(&key);
            }
            return match && ret;
        } else if (*spec == 'u' && *(spec + 1) == '\0') {
            AVCodecParameters *par = st->codecpar;
            int val;
            switch (par->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                val = par->sample_rate && par->channels;
                if (par->format == AV_SAMPLE_FMT_NONE)
                    return 0;
                break;
            case AVMEDIA_TYPE_VIDEO:
                val = par->width && par->height;
                if (par->format == AV_PIX_FMT_NONE)
                    return 0;
                break;
            case AVMEDIA_TYPE_UNKNOWN:
                val = 0;
                break;
            default:
                val = 1;
                break;
            }
            return match && (par->codec_id != AV_CODEC_ID_NONE && val != 0);
        } else {
            return AVERROR(EINVAL);
        }
    }

    return match;
}


int avformat_match_stream_specifier(AVFormatContext *s, AVStream *st,
                                    const char *spec)
{
    int ret, index;
    char *endptr;
    const char *indexptr = NULL;
    AVProgram *p = NULL;
    int nb_streams;

    ret = match_stream_specifier(s, st, spec, &indexptr, &p);
    if (ret < 0)
        goto error;

    if (!indexptr)
        return ret;

    index = strtol(indexptr, &endptr, 0);
    if (*endptr) {                  /* We can't have anything after the requested index. */
        ret = AVERROR(EINVAL);
        goto error;
    }

    /* This is not really needed but saves us a loop for simple stream index specifiers. */
    if (spec == indexptr)
        return (index == st->index);

    /* If we requested a matching stream index, we have to ensure st is that. */
    nb_streams = p ? p->nb_stream_indexes : s->nb_streams;
    for (int i = 0; i < nb_streams && index >= 0; i++) {
        AVStream *candidate = p ? s->streams[p->stream_index[i]] : s->streams[i];
        ret = match_stream_specifier(s, candidate, spec, NULL, NULL);
        if (ret < 0)
            goto error;
        if (ret > 0 && index-- == 0 && st == candidate)
            return 1;
    }
    return 0;

error:
    if (ret == AVERROR(EINVAL))
        av_log(s, AV_LOG_ERROR, "Invalid stream specifier: %s.\n", spec);
    return ret;
}

int ff_generate_avci_extradata(AVStream *st)
{
    static const uint8_t avci100_1080p_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x7a, 0x10, 0x29,
        0xb6, 0xd4, 0x20, 0x22, 0x33, 0x19, 0xc6, 0x63,
        0x23, 0x21, 0x01, 0x11, 0x98, 0xce, 0x33, 0x19,
        0x18, 0x21, 0x02, 0x56, 0xb9, 0x3d, 0x7d, 0x7e,
        0x4f, 0xe3, 0x3f, 0x11, 0xf1, 0x9e, 0x08, 0xb8,
        0x8c, 0x54, 0x43, 0xc0, 0x78, 0x02, 0x27, 0xe2,
        0x70, 0x1e, 0x30, 0x10, 0x10, 0x14, 0x00, 0x00,
        0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xca,
        0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x33, 0x48,
        0xd0
    };
    static const uint8_t avci100_1080i_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x7a, 0x10, 0x29,
        0xb6, 0xd4, 0x20, 0x22, 0x33, 0x19, 0xc6, 0x63,
        0x23, 0x21, 0x01, 0x11, 0x98, 0xce, 0x33, 0x19,
        0x18, 0x21, 0x03, 0x3a, 0x46, 0x65, 0x6a, 0x65,
        0x24, 0xad, 0xe9, 0x12, 0x32, 0x14, 0x1a, 0x26,
        0x34, 0xad, 0xa4, 0x41, 0x82, 0x23, 0x01, 0x50,
        0x2b, 0x1a, 0x24, 0x69, 0x48, 0x30, 0x40, 0x2e,
        0x11, 0x12, 0x08, 0xc6, 0x8c, 0x04, 0x41, 0x28,
        0x4c, 0x34, 0xf0, 0x1e, 0x01, 0x13, 0xf2, 0xe0,
        0x3c, 0x60, 0x20, 0x20, 0x28, 0x00, 0x00, 0x03,
        0x00, 0x08, 0x00, 0x00, 0x03, 0x01, 0x94, 0x20,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x33, 0x48,
        0xd0
    };
    static const uint8_t avci50_1080p_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x6e, 0x10, 0x28,
        0xa6, 0xd4, 0x20, 0x32, 0x33, 0x0c, 0x71, 0x18,
        0x88, 0x62, 0x10, 0x19, 0x19, 0x86, 0x38, 0x8c,
        0x44, 0x30, 0x21, 0x02, 0x56, 0x4e, 0x6f, 0x37,
        0xcd, 0xf9, 0xbf, 0x81, 0x6b, 0xf3, 0x7c, 0xde,
        0x6e, 0x6c, 0xd3, 0x3c, 0x05, 0xa0, 0x22, 0x7e,
        0x5f, 0xfc, 0x00, 0x0c, 0x00, 0x13, 0x8c, 0x04,
        0x04, 0x05, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00,
        0x00, 0x03, 0x00, 0x32, 0x84, 0x00, 0x00, 0x00,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x31, 0x12,
        0x11
    };
    static const uint8_t avci50_1080i_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x6e, 0x10, 0x28,
        0xa6, 0xd4, 0x20, 0x32, 0x33, 0x0c, 0x71, 0x18,
        0x88, 0x62, 0x10, 0x19, 0x19, 0x86, 0x38, 0x8c,
        0x44, 0x30, 0x21, 0x02, 0x56, 0x4e, 0x6e, 0x61,
        0x87, 0x3e, 0x73, 0x4d, 0x98, 0x0c, 0x03, 0x06,
        0x9c, 0x0b, 0x73, 0xe6, 0xc0, 0xb5, 0x18, 0x63,
        0x0d, 0x39, 0xe0, 0x5b, 0x02, 0xd4, 0xc6, 0x19,
        0x1a, 0x79, 0x8c, 0x32, 0x34, 0x24, 0xf0, 0x16,
        0x81, 0x13, 0xf7, 0xff, 0x80, 0x02, 0x00, 0x01,
        0xf1, 0x80, 0x80, 0x80, 0xa0, 0x00, 0x00, 0x03,
        0x00, 0x20, 0x00, 0x00, 0x06, 0x50, 0x80, 0x00,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x31, 0x12,
        0x11
    };
    static const uint8_t avci100_720p_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x7a, 0x10, 0x29,
        0xb6, 0xd4, 0x20, 0x2a, 0x33, 0x1d, 0xc7, 0x62,
        0xa1, 0x08, 0x40, 0x54, 0x66, 0x3b, 0x8e, 0xc5,
        0x42, 0x02, 0x10, 0x25, 0x64, 0x2c, 0x89, 0xe8,
        0x85, 0xe4, 0x21, 0x4b, 0x90, 0x83, 0x06, 0x95,
        0xd1, 0x06, 0x46, 0x97, 0x20, 0xc8, 0xd7, 0x43,
        0x08, 0x11, 0xc2, 0x1e, 0x4c, 0x91, 0x0f, 0x01,
        0x40, 0x16, 0xec, 0x07, 0x8c, 0x04, 0x04, 0x05,
        0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03,
        0x00, 0x64, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x31, 0x12,
        0x11
    };
    static const uint8_t avci50_720p_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x6e, 0x10, 0x20,
        0xa6, 0xd4, 0x20, 0x32, 0x33, 0x0c, 0x71, 0x18,
        0x88, 0x62, 0x10, 0x19, 0x19, 0x86, 0x38, 0x8c,
        0x44, 0x30, 0x21, 0x02, 0x56, 0x4e, 0x6f, 0x37,
        0xcd, 0xf9, 0xbf, 0x81, 0x6b, 0xf3, 0x7c, 0xde,
        0x6e, 0x6c, 0xd3, 0x3c, 0x0f, 0x01, 0x6e, 0xff,
        0xc0, 0x00, 0xc0, 0x01, 0x38, 0xc0, 0x40, 0x40,
        0x50, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00,
        0x06, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x31, 0x12,
        0x11
    };

    const uint8_t *data = NULL;
    int ret, size       = 0;

    if (st->codecpar->width == 1920) {
        if (st->codecpar->field_order == AV_FIELD_PROGRESSIVE) {
            data = avci100_1080p_extradata;
            size = sizeof(avci100_1080p_extradata);
        } else {
            data = avci100_1080i_extradata;
            size = sizeof(avci100_1080i_extradata);
        }
    } else if (st->codecpar->width == 1440) {
        if (st->codecpar->field_order == AV_FIELD_PROGRESSIVE) {
            data = avci50_1080p_extradata;
            size = sizeof(avci50_1080p_extradata);
        } else {
            data = avci50_1080i_extradata;
            size = sizeof(avci50_1080i_extradata);
        }
    } else if (st->codecpar->width == 1280) {
        data = avci100_720p_extradata;
        size = sizeof(avci100_720p_extradata);
    } else if (st->codecpar->width == 960) {
        data = avci50_720p_extradata;
        size = sizeof(avci50_720p_extradata);
    }

    if (!size)
        return 0;

    if ((ret = ff_alloc_extradata(st->codecpar, size)) < 0)
        return ret;
    memcpy(st->codecpar->extradata, data, size);

    return 0;
}

uint8_t *av_stream_get_side_data(const AVStream *st,
                                 enum AVPacketSideDataType type, size_t *size)
{
    for (int i = 0; i < st->nb_side_data; i++) {
        if (st->side_data[i].type == type) {
            if (size)
                *size = st->side_data[i].size;
            return st->side_data[i].data;
        }
    }
    if (size)
        *size = 0;
    return NULL;
}

int av_stream_add_side_data(AVStream *st, enum AVPacketSideDataType type,
                            uint8_t *data, size_t size)
{
    AVPacketSideData *sd, *tmp;

    for (int i = 0; i < st->nb_side_data; i++) {
        sd = &st->side_data[i];

        if (sd->type == type) {
            av_freep(&sd->data);
            sd->data = data;
            sd->size = size;
            return 0;
        }
    }

    if ((unsigned)st->nb_side_data + 1 >= INT_MAX / sizeof(*st->side_data))
        return AVERROR(ERANGE);

    tmp = av_realloc(st->side_data, (st->nb_side_data + 1) * sizeof(*tmp));
    if (!tmp) {
        return AVERROR(ENOMEM);
    }

    st->side_data = tmp;
    st->nb_side_data++;

    sd = &st->side_data[st->nb_side_data - 1];
    sd->type = type;
    sd->data = data;
    sd->size = size;

    return 0;
}

uint8_t *av_stream_new_side_data(AVStream *st, enum AVPacketSideDataType type,
                                 size_t size)
{
    int ret;
    uint8_t *data = av_malloc(size);

    if (!data)
        return NULL;

    ret = av_stream_add_side_data(st, type, data, size);
    if (ret < 0) {
        av_freep(&data);
        return NULL;
    }

    return data;
}

int ff_stream_add_bitstream_filter(AVStream *st, const char *name, const char *args)
{
    int ret;
    const AVBitStreamFilter *bsf;
    FFStream *const sti = ffstream(st);
    AVBSFContext *bsfc;

    av_assert0(!sti->bsfc);

    if (!(bsf = av_bsf_get_by_name(name))) {
        av_log(NULL, AV_LOG_ERROR, "Unknown bitstream filter '%s'\n", name);
        return AVERROR_BSF_NOT_FOUND;
    }

    if ((ret = av_bsf_alloc(bsf, &bsfc)) < 0)
        return ret;

    bsfc->time_base_in = st->time_base;
    if ((ret = avcodec_parameters_copy(bsfc->par_in, st->codecpar)) < 0) {
        av_bsf_free(&bsfc);
        return ret;
    }

    if (args && bsfc->filter->priv_class) {
        const AVOption *opt = av_opt_next(bsfc->priv_data, NULL);
        const char * shorthand[2] = {NULL};

        if (opt)
            shorthand[0] = opt->name;

        if ((ret = av_opt_set_from_string(bsfc->priv_data, args, shorthand, "=", ":")) < 0) {
            av_bsf_free(&bsfc);
            return ret;
        }
    }

    if ((ret = av_bsf_init(bsfc)) < 0) {
        av_bsf_free(&bsfc);
        return ret;
    }

    sti->bsfc = bsfc;

    av_log(NULL, AV_LOG_VERBOSE,
           "Automatically inserted bitstream filter '%s'; args='%s'\n",
           name, args ? args : "");
    return 1;
}

int ff_format_output_open(AVFormatContext *s, const char *url, AVDictionary **options)
{
    if (!s->oformat)
        return AVERROR(EINVAL);

    if (!(s->oformat->flags & AVFMT_NOFILE))
        return s->io_open(s, &s->pb, url, AVIO_FLAG_WRITE, options);
    return 0;
}

void ff_format_io_close(AVFormatContext *s, AVIOContext **pb)
{
    if (*pb)
        s->io_close(s, *pb);
    *pb = NULL;
}

int ff_is_http_proto(char *filename) {
    const char *proto = avio_find_protocol_name(filename);
    return proto ? (!av_strcasecmp(proto, "http") || !av_strcasecmp(proto, "https")) : 0;
}

int ff_parse_creation_time_metadata(AVFormatContext *s, int64_t *timestamp, int return_seconds)
{
    AVDictionaryEntry *entry;
    int64_t parsed_timestamp;
    int ret;
    if ((entry = av_dict_get(s->metadata, "creation_time", NULL, 0))) {
        if ((ret = av_parse_time(&parsed_timestamp, entry->value, 0)) >= 0) {
            *timestamp = return_seconds ? parsed_timestamp / 1000000 : parsed_timestamp;
            return 1;
        } else {
            av_log(s, AV_LOG_WARNING, "Failed to parse creation_time %s\n", entry->value);
            return ret;
        }
    }
    return 0;
}

int ff_standardize_creation_time(AVFormatContext *s)
{
    int64_t timestamp;
    int ret = ff_parse_creation_time_metadata(s, &timestamp, 0);
    if (ret == 1)
        return avpriv_dict_set_timestamp(&s->metadata, "creation_time", timestamp);
    return ret;
}

int ff_get_packet_palette(AVFormatContext *s, AVPacket *pkt, int ret, uint32_t *palette)
{
    uint8_t *side_data;
    size_t size;

    side_data = av_packet_get_side_data(pkt, AV_PKT_DATA_PALETTE, &size);
    if (side_data) {
        if (size != AVPALETTE_SIZE) {
            av_log(s, AV_LOG_ERROR, "Invalid palette side data\n");
            return AVERROR_INVALIDDATA;
        }
        memcpy(palette, side_data, AVPALETTE_SIZE);
        return 1;
    }

    if (ret == CONTAINS_PAL) {
        for (int i = 0; i < AVPALETTE_COUNT; i++)
            palette[i] = AV_RL32(pkt->data + pkt->size - AVPALETTE_SIZE + i*4);
        return 1;
    }

    return 0;
}

int ff_bprint_to_codecpar_extradata(AVCodecParameters *par, struct AVBPrint *buf)
{
    int ret;
    char *str;

    ret = av_bprint_finalize(buf, &str);
    if (ret < 0)
        return ret;
    if (!av_bprint_is_complete(buf)) {
        av_free(str);
        return AVERROR(ENOMEM);
    }

    par->extradata = str;
    /* Note: the string is NUL terminated (so extradata can be read as a
     * string), but the ending character is not accounted in the size (in
     * binary formats you are likely not supposed to mux that character). When
     * extradata is copied, it is also padded with AV_INPUT_BUFFER_PADDING_SIZE
     * zeros. */
    par->extradata_size = buf->len;
    return 0;
}

int avformat_transfer_internal_stream_timing_info(const AVOutputFormat *ofmt,
                                                  AVStream *ost, const AVStream *ist,
                                                  enum AVTimebaseSource copy_tb)
{
    const AVCodecContext *const dec_ctx = cffstream(ist)->avctx;
    AVCodecContext       *const enc_ctx =  ffstream(ost)->avctx;

    enc_ctx->time_base = ist->time_base;
    /*
     * Avi is a special case here because it supports variable fps but
     * having the fps and timebase differe significantly adds quite some
     * overhead
     */
    if (!strcmp(ofmt->name, "avi")) {
#if FF_API_R_FRAME_RATE
        if (copy_tb == AVFMT_TBCF_AUTO && ist->r_frame_rate.num
            && av_q2d(ist->r_frame_rate) >= av_q2d(ist->avg_frame_rate)
            && 0.5/av_q2d(ist->r_frame_rate) > av_q2d(ist->time_base)
            && 0.5/av_q2d(ist->r_frame_rate) > av_q2d(dec_ctx->time_base)
            && av_q2d(ist->time_base) < 1.0/500 && av_q2d(dec_ctx->time_base) < 1.0/500
            || copy_tb == AVFMT_TBCF_R_FRAMERATE) {
            enc_ctx->time_base.num = ist->r_frame_rate.den;
            enc_ctx->time_base.den = 2*ist->r_frame_rate.num;
            enc_ctx->ticks_per_frame = 2;
        } else
#endif
            if (copy_tb == AVFMT_TBCF_AUTO && av_q2d(dec_ctx->time_base)*dec_ctx->ticks_per_frame > 2*av_q2d(ist->time_base)
                   && av_q2d(ist->time_base) < 1.0/500
                   || copy_tb == AVFMT_TBCF_DECODER) {
            enc_ctx->time_base = dec_ctx->time_base;
            enc_ctx->time_base.num *= dec_ctx->ticks_per_frame;
            enc_ctx->time_base.den *= 2;
            enc_ctx->ticks_per_frame = 2;
        }
    } else if (!(ofmt->flags & AVFMT_VARIABLE_FPS)
               && !av_match_name(ofmt->name, "mov,mp4,3gp,3g2,psp,ipod,ismv,f4v")) {
        if (copy_tb == AVFMT_TBCF_AUTO && dec_ctx->time_base.den
            && av_q2d(dec_ctx->time_base)*dec_ctx->ticks_per_frame > av_q2d(ist->time_base)
            && av_q2d(ist->time_base) < 1.0/500
            || copy_tb == AVFMT_TBCF_DECODER) {
            enc_ctx->time_base = dec_ctx->time_base;
            enc_ctx->time_base.num *= dec_ctx->ticks_per_frame;
        }
    }

    if ((enc_ctx->codec_tag == AV_RL32("tmcd") || ost->codecpar->codec_tag == AV_RL32("tmcd"))
        && dec_ctx->time_base.num < dec_ctx->time_base.den
        && dec_ctx->time_base.num > 0
        && 121LL*dec_ctx->time_base.num > dec_ctx->time_base.den) {
        enc_ctx->time_base = dec_ctx->time_base;
    }

    av_reduce(&enc_ctx->time_base.num, &enc_ctx->time_base.den,
              enc_ctx->time_base.num, enc_ctx->time_base.den, INT_MAX);

    return 0;
}

AVRational av_stream_get_codec_timebase(const AVStream *st)
{
    // See avformat_transfer_internal_stream_timing_info() TODO.
    return cffstream(st)->avctx->time_base;
}

void ff_format_set_url(AVFormatContext *s, char *url)
{
    av_assert0(url);
    av_freep(&s->url);
    s->url = url;
}

/**
 * @brief Remove a stream from a media stream.
 *
 * This is used by mpegts, so we can track streams as indicated by the PMT.
 *
 * @param s MPEG media stream handle
 * @param id stream id of stream to remove
 * @param remove_ts if true, remove any matching MPEG-TS filter as well
 */
void av_remove_stream(AVFormatContext *s, int id, int remove_ts) {
    int i;
    int changes = 0;

    for (i=0; i<s->nb_streams; i++) {
        AVCodecContext *codec_ctx;
        if (s->streams[i]->id != id)
            continue;

        av_log(NULL, AV_LOG_DEBUG, "av_remove_stream 0x%x\n", id);

#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
        /* close codec context */
        codec_ctx = s->streams[i]->codec;
        if (codec_ctx->codec) {
            avcodec_close(codec_ctx);
            av_free(codec_ctx);
        }
FF_ENABLE_DEPRECATION_WARNINGS
#endif
#if 0
        /* make sure format context is not using the codec context */
        if (&s->streams[i] == s->cur_st) {
            av_log(NULL, AV_LOG_DEBUG, "av_remove_stream cur_st = NULL\n");
            s->cur_st = NULL;
        }
     /*   else if (s->cur_st > &s->streams[i]) {
            av_log(NULL, AV_LOG_DEBUG, "av_remove_stream cur_st -= 1\n");
            s->cur_st -= sizeof(AVFormatContext *);
        } */
        else {
            av_log(NULL, AV_LOG_DEBUG,
                   "av_remove_stream: no change to cur_st\n");
        }
#endif

        av_log(NULL, AV_LOG_DEBUG, "av_remove_stream: removing... "
               "s->nb_streams=%d i=%d\n", s->nb_streams, i);
        /* actually remove av stream */
        s->nb_streams--;
        if ((s->nb_streams - i) > 0) {
            memmove(&s->streams[i], &s->streams[i+1],
                    (s->nb_streams-i)*sizeof(AVFormatContext *));
        }
        else
            s->streams[i] = NULL;

        changes = 1;
    }
    if (changes)
    {
        // flush queued packets after a stream change (might need to make smarter)
        ff_flush_packet_queue(s);

        /* renumber the streams */
        av_log(NULL, AV_LOG_DEBUG, "av_remove_stream: renumbering streams\n");
        for (i=0; i<s->nb_streams; i++)
            s->streams[i]->index=i;
    }

    (void)remove_ts;
}
