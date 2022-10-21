/*
 * MPEG-1/2 VLC
 * copyright (c) 2000,2001 Fabrice Bellard
 * copyright (c) 2002-2004 Michael Niedermayer <michaelni@gmx.at>
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

/**
 * @file
 * MPEG-1/2 VLC.
 */

#ifndef AVCODEC_MPEG12VLC_H
#define AVCODEC_MPEG12VLC_H

#include "rl.h"
#include "vlc.h"

#define DC_VLC_BITS 9
#define MV_VLC_BITS 8
#define TEX_VLC_BITS 9

#define MBINCR_VLC_BITS 9
#define MB_PAT_VLC_BITS 9
#define MB_PTYPE_VLC_BITS 6
#define MB_BTYPE_VLC_BITS 6

extern VLC ff_dc_lum_vlc;
extern VLC ff_dc_chroma_vlc;
extern VLC ff_mbincr_vlc;
extern VLC ff_mb_ptype_vlc;
extern VLC ff_mb_btype_vlc;
extern VLC ff_mb_pat_vlc;
extern VLC ff_mv_vlc;

void ff_mpeg12_init_vlcs(void);

#define INIT_2D_VLC_RL(rl, rl_vlc, flags)\
    ff_init_2d_vlc_rl(&rl, rl_vlc, FF_ARRAY_ELEMS(rl_vlc), flags)

#define MPEG12_RL_NB_ELEMS 111

extern RLTable ff_rl_mpeg1;
extern RLTable ff_rl_mpeg2;

extern const uint16_t ff_mpeg1_vlc_table[MPEG12_RL_NB_ELEMS + 2][2];
extern const uint16_t ff_mpeg2_vlc_table[MPEG12_RL_NB_ELEMS + 2][2];

extern RL_VLC_ELEM ff_mpeg1_rl_vlc[];
extern RL_VLC_ELEM ff_mpeg2_rl_vlc[];

void ff_init_2d_vlc_rl(const RLTable *rl, RL_VLC_ELEM rl_vlc[],
                       unsigned static_size, int flags);

void ff_mpeg1_init_uni_ac_vlc(const RLTable *rl, uint8_t *uni_ac_vlc_len);

#endif /* AVCODEC_MPEG12VLC_H */
