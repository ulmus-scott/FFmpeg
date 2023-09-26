/*
 * MPEG-4 Parametric Stereo data tables
 * Copyright (c) 2010 Alex Converse <alex.converse@gmail.com>
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

static const uint8_t huff_iid_df1_tab[][2] = {
    /* huff_iid_df1 - 61 entries */
    {  28,   4 }, {  32,   4 }, {  29,   3 }, {  31,   3 }, {  27,   5 },
    {  33,   5 }, {  26,   6 }, {  34,   6 }, {  25,   7 }, {  35,   7 },
    {  24,   8 }, {  36,   8 }, {  37,   9 }, {  40,  11 }, {  19,  12 },
    {  41,  12 }, {  22,  10 }, {  38,  10 }, {   9,  17 }, {  51,  17 },
    {  11,  17 }, {  49,  17 }, {  13,  16 }, {  47,  16 }, {  16,  14 },
    {  18,  13 }, {  42,  13 }, {  44,  14 }, {  12,  17 }, {  48,  17 },
    {   4,  18 }, {   5,  18 }, {   2,  18 }, {   3,  18 }, {  15,  15 },
    {  21,  11 }, {  39,  11 }, {  45,  15 }, {   8,  18 }, {  52,  18 },
    {   6,  18 }, {   7,  18 }, {  55,  18 }, {  56,  18 }, {  53,  18 },
    {  54,  18 }, {  17,  14 }, {  43,  14 }, {  59,  18 }, {  60,  18 },
    {  57,  18 }, {  58,  18 }, {   0,  18 }, {   1,  18 }, {  10,  18 },
    {  50,  18 }, {  14,  16 }, {  46,  16 }, {  20,  12 }, {  23,  10 },
    {  30,   1 },
};

static const uint8_t huff_iid_dt1_tab[][2] = {
    /* huff_iid_dt1 - 61 entries */
    {  31,   2 }, {  26,   7 }, {  34,   7 }, {  27,   6 }, {  33,   6 },
    {  35,   8 }, {  24,   9 }, {  36,   9 }, {  39,  11 }, {  41,  12 },
    {   9,  15 }, {  10,  15 }, {  48,  15 }, {  49,  15 }, {  17,  13 },
    {  23,  10 }, {  37,  10 }, {  43,  13 }, {  11,  15 }, {  12,  15 },
    {   4,  16 }, {  56,  16 }, {   2,  16 }, {   3,  16 }, {  59,  16 },
    {  60,  16 }, {  57,  16 }, {  58,  16 }, {   0,  16 }, {   1,  16 },
    {   5,  16 }, {  55,  16 }, {   6,  16 }, {  54,  16 }, {  13,  15 },
    {  15,  14 }, {  20,  12 }, {  40,  12 }, {  22,  11 }, {  38,  11 },
    {  45,  14 }, {  47,  15 }, {   7,  16 }, {  53,  16 }, {  18,  13 },
    {  42,  13 }, {  16,  14 }, {  44,  14 }, {   8,  16 }, {  52,  16 },
    {  14,  15 }, {  46,  15 }, {  50,  16 }, {  51,  16 }, {  19,  13 },
    {  21,  12 }, {  25,   9 }, {  28,   5 }, {  32,   5 }, {  29,   3 },
    {  30,   1 },
};

static const uint8_t huff_iid_df0_tab[][2] = {
    /* huff_iid_df0 - 29 entries */
    {  14,   1 }, {  15,   3 }, {  13,   3 }, {  16,   4 }, {  12,   4 },
    {  17,   5 }, {  11,   5 }, {  10,   6 }, {  18,   6 }, {  19,   6 },
    {   9,   7 }, {  20,   8 }, {   8,   9 }, {   7,  10 }, {  21,  11 },
    {  22,  13 }, {   6,  13 }, {  23,  14 }, {  24,  14 }, {   5,  15 },
    {  25,  15 }, {   4,  16 }, {   3,  17 }, {   0,  17 }, {   1,  17 },
    {   2,  17 }, {  26,  17 }, {  27,  18 }, {  28,  18 },
};

static const uint8_t huff_iid_dt0_tab[][2] = {
    /* huff_iid_dt0 - 29 entries */
    {  14,   1 }, {  13,   2 }, {  15,   3 }, {  12,   4 }, {  16,   5 },
    {  11,   6 }, {  17,   7 }, {  10,   8 }, {  18,   9 }, {   9,  10 },
    {  19,  11 }, {   8,  12 }, {  20,  13 }, {  21,  14 }, {   7,  15 },
    {  22,  17 }, {   6,  17 }, {  23,  19 }, {   0,  19 }, {   1,  19 },
    {   2,  19 }, {   3,  20 }, {   4,  20 }, {   5,  20 }, {  24,  20 },
    {  25,  20 }, {  26,  20 }, {  27,  20 }, {  28,  20 },
};

static const uint8_t huff_icc_df_tab[][2] = {
    /* huff_icc_df - 15 entries */
    {   7,   1 }, {   8,   2 }, {   6,   3 }, {   9,   4 }, {   5,   5 },
    {  10,   6 }, {   4,   7 }, {  11,   8 }, {  12,   9 }, {   3,  10 },
    {  13,  11 }, {   2,  12 }, {  14,  13 }, {   1,  14 }, {   0,  14 },
};

static const uint8_t huff_icc_dt_tab[][2] = {
    /* huff_icc_dt - 15 entries */
    {   7,   1 }, {   8,   2 }, {   6,   3 }, {   9,   4 }, {   5,   5 },
    {  10,   6 }, {   4,   7 }, {  11,   8 }, {   3,   9 }, {  12,  10 },
    {   2,  11 }, {  13,  12 }, {   1,  13 }, {   0,  14 }, {  14,  14 },
};

static const uint8_t huff_ipd_df_tab[][2] = {
    /* huff_ipd_df - 8 entries */
    {   1,   3 }, {   4,   4 }, {   5,   4 }, {   3,   4 }, {   6,   4 },
    {   2,   4 }, {   7,   4 }, {   0,   1 },
};

static const uint8_t huff_ipd_dt_tab[][2] = {
    /* huff_ipd_dt - 8 entries */
    {   5,   4 }, {   4,   5 }, {   3,   5 }, {   2,   4 }, {   6,   4 },
    {   1,   3 }, {   7,   3 }, {   0,   1 },
};

static const uint8_t huff_opd_df_tab[][2] = {
    /* huff_opd_df - 8 entries */
    {   7,   3 }, {   1,   3 }, {   3,   4 }, {   6,   4 }, {   2,   4 },
    {   5,   5 }, {   4,   5 }, {   0,   1 },
};

static const uint8_t huff_opd_dt_tab[][2] = {
    /* huff_opd_dt - 8 entries */
    {   5,   4 }, {   2,   4 }, {   6,   4 }, {   4,   5 }, {   3,   5 },
    {   1,   3 }, {   7,   3 }, {   0,   1 },
};

static const int8_t huff_offset[] = {
    30, 30,
    14, 14,
    7, 7,
    0, 0,
    0, 0,
};

///Table 8.48
const int8_t ff_k_to_i_20[] = {
     1,  0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 14, 15,
    15, 15, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18, 18,
    18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19
};
///Table 8.49
const int8_t ff_k_to_i_34[] = {
     0,  1,  2,  3,  4,  5,  6,  6,  7,  2,  1,  0, 10, 10,  4,  5,  6,  7,  8,
     9, 10, 11, 12,  9, 14, 11, 12, 13, 14, 15, 16, 13, 16, 17, 18, 19, 20, 21,
    22, 22, 23, 23, 24, 24, 25, 25, 26, 26, 27, 27, 27, 28, 28, 28, 29, 29, 29,
    30, 30, 30, 31, 31, 31, 31, 32, 32, 32, 32, 33, 33, 33, 33, 33, 33, 33, 33,
    33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33
};
