/*
 * MPEG2 transport utilities
 * Copyright (c) 2002-2012 The MythTV Team
 *
 * This file is part of MythTV.
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

#include "avformat.h"
#include "internal.h"

/**
 * @brief Remove a stream from a media stream.
 *
 * This is used by mpegts, so we can track streams as indicated by the PMT.
 *
 * @param s MPEG media stream handle
 * @param id stream id of stream to remove
 * @param remove_ts if true, remove any matching MPEG-TS filter as well
 */
void av_remove_stream(AVFormatContext *s, int id, int /* remove_ts */) {
    int i;
    int changes = 0;

    for (i=0; i<s->nb_streams; i++) {
        if (s->streams[i]->id != id)
            continue;

        av_log(NULL, AV_LOG_DEBUG, "av_remove_stream 0x%x\n", id);

        /* close codec context */
        AVCodecContext *codec_ctx = s->streams[i]->codec;
        if (codec_ctx->codec) {
            avcodec_close(codec_ctx);
            av_free(codec_ctx);
        }
#if 0
        /* make sure format context is not using the codec context */
        if (&s->streams[i] == s->cur_st) {
            av_log(NULL, AV_LOG_DEBUG, "av_remove_stream cur_st = NULL\n");
            s->cur_st = NULL;
        }
#endif
     /*   else if (s->cur_st > &s->streams[i]) {
            av_log(NULL, AV_LOG_DEBUG, "av_remove_stream cur_st -= 1\n");
            s->cur_st -= sizeof(AVFormatContext *);
        } */
        else {
            av_log(NULL, AV_LOG_DEBUG,
                   "av_remove_stream: no change to cur_st\n");
        }

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
        flush_packet_queue(s);

        /* renumber the streams */
        av_log(NULL, AV_LOG_DEBUG, "av_remove_stream: renumbering streams\n");
        for (i=0; i<s->nb_streams; i++)
            s->streams[i]->index=i;
    }
}
