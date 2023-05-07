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
#include "libavutil/eval.h"
#include "libavutil/fifo.h"
#include "libavutil/file.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "internal.h"
#include "filters.h"
#include "vulkan_filter.h"
#include "scale_eval.h"

#include <libplacebo/renderer.h>
#include <libplacebo/utils/libav.h>
#include <libplacebo/utils/frame_queue.h>
#include <libplacebo/vulkan.h>

/* Backwards compatibility with older libplacebo */
#if PL_API_VER < 276
static inline AVFrame *pl_get_mapped_avframe(const struct pl_frame *frame)
{
    return frame->user_data;
}
#endif

enum {
    TONE_MAP_AUTO,
    TONE_MAP_CLIP,
    TONE_MAP_ST2094_40,
    TONE_MAP_ST2094_10,
    TONE_MAP_BT2390,
    TONE_MAP_BT2446A,
    TONE_MAP_SPLINE,
    TONE_MAP_REINHARD,
    TONE_MAP_MOBIUS,
    TONE_MAP_HABLE,
    TONE_MAP_GAMMA,
    TONE_MAP_LINEAR,
    TONE_MAP_COUNT,
};

static const struct pl_tone_map_function * const tonemapping_funcs[TONE_MAP_COUNT] = {
    [TONE_MAP_AUTO]      = &pl_tone_map_auto,
    [TONE_MAP_CLIP]      = &pl_tone_map_clip,
#if PL_API_VER >= 246
    [TONE_MAP_ST2094_40] = &pl_tone_map_st2094_40,
    [TONE_MAP_ST2094_10] = &pl_tone_map_st2094_10,
#endif
    [TONE_MAP_BT2390]    = &pl_tone_map_bt2390,
    [TONE_MAP_BT2446A]   = &pl_tone_map_bt2446a,
    [TONE_MAP_SPLINE]    = &pl_tone_map_spline,
    [TONE_MAP_REINHARD]  = &pl_tone_map_reinhard,
    [TONE_MAP_MOBIUS]    = &pl_tone_map_mobius,
    [TONE_MAP_HABLE]     = &pl_tone_map_hable,
    [TONE_MAP_GAMMA]     = &pl_tone_map_gamma,
    [TONE_MAP_LINEAR]    = &pl_tone_map_linear,
};

static const char *const var_names[] = {
    "in_w", "iw",   ///< width  of the input video frame
    "in_h", "ih",   ///< height of the input video frame
    "out_w", "ow",  ///< width  of the output video frame
    "out_h", "oh",  ///< height of the output video frame
    "crop_w", "cw", ///< evaluated input crop width
    "crop_h", "ch", ///< evaluated input crop height
    "pos_w", "pw",  ///< evaluated output placement width
    "pos_h", "ph",  ///< evaluated output placement height
    "a",            ///< iw/ih
    "sar",          ///< input pixel aspect ratio
    "dar",          ///< output pixel aspect ratio
    "hsub",         ///< input horizontal subsampling factor
    "vsub",         ///< input vertical subsampling factor
    "ohsub",        ///< output horizontal subsampling factor
    "ovsub",        ///< output vertical subsampling factor
    "in_t", "t",    ///< input frame pts
    "out_t", "ot",  ///< output frame pts
    "n",            ///< number of frame
    NULL,
};

enum var_name {
    VAR_IN_W,   VAR_IW,
    VAR_IN_H,   VAR_IH,
    VAR_OUT_W,  VAR_OW,
    VAR_OUT_H,  VAR_OH,
    VAR_CROP_W, VAR_CW,
    VAR_CROP_H, VAR_CH,
    VAR_POS_W,  VAR_PW,
    VAR_POS_H,  VAR_PH,
    VAR_A,
    VAR_SAR,
    VAR_DAR,
    VAR_HSUB,
    VAR_VSUB,
    VAR_OHSUB,
    VAR_OVSUB,
    VAR_IN_T,   VAR_T,
    VAR_OUT_T,  VAR_OT,
    VAR_N,
    VAR_VARS_NB
};

typedef struct LibplaceboContext {
    /* lavfi vulkan*/
    FFVulkanContext vkctx;

    /* libplacebo */
    pl_log log;
    pl_vulkan vulkan;
    pl_gpu gpu;
    pl_renderer renderer;
    pl_queue queue;
    pl_tex tex[4];

    /* filter state */
    AVFifo *out_pts; ///< timestamps of wanted output frames

    /* settings */
    char *out_format_string;
    enum AVPixelFormat out_format;
    char *fillcolor;
    double var_values[VAR_VARS_NB];
    char *w_expr;
    char *h_expr;
    char *fps_string;
    AVRational fps; ///< parsed FPS, or 0/0 for "none"
    char *crop_x_expr, *crop_y_expr;
    char *crop_w_expr, *crop_h_expr;
    char *pos_x_expr, *pos_y_expr;
    char *pos_w_expr, *pos_h_expr;
    // Parsed expressions for input/output crop
    AVExpr *crop_x_pexpr, *crop_y_pexpr, *crop_w_pexpr, *crop_h_pexpr;
    AVExpr *pos_x_pexpr, *pos_y_pexpr, *pos_w_pexpr, *pos_h_pexpr;
    AVRational target_sar;
    float pad_crop_ratio;
    int force_original_aspect_ratio;
    int force_divisible_by;
    int normalize_sar;
    int apply_filmgrain;
    int apply_dovi;
    int colorspace;
    int color_range;
    int color_primaries;
    int color_trc;

    /* pl_render_params */
    struct pl_render_params params;
    char *upscaler;
    char *downscaler;
    int lut_entries;
    float antiringing;
    int sigmoid;
    int skip_aa;
    int skip_cache;
    float polar_cutoff;
    int disable_linear;
    int disable_builtin;
    int force_dither;
    int disable_fbos;

    /* pl_deband_params */
    struct pl_deband_params deband_params;
    int deband;
    int deband_iterations;
    float deband_threshold;
    float deband_radius;
    float deband_grain;

    /* pl_color_adjustment */
    struct pl_color_adjustment color_adjustment;
    float brightness;
    float contrast;
    float saturation;
    float hue;
    float gamma;

    /* pl_peak_detect_params */
    struct pl_peak_detect_params peak_detect_params;
    int peakdetect;
    float smoothing;
    float min_peak;
    float scene_low;
    float scene_high;
    float overshoot;

    /* pl_color_map_params */
    struct pl_color_map_params color_map_params;
    int intent;
    int gamut_mode;
    int tonemapping;
    float tonemapping_param;
    int tonemapping_mode;
    int inverse_tonemapping;
    float crosstalk;
    int tonemapping_lut_size;

#if FF_API_LIBPLACEBO_OPTS
    /* for backwards compatibility */
    float desat_str;
    float desat_exp;
    int gamut_warning;
    int gamut_clipping;
    int force_icc_lut;
#endif

    /* pl_dither_params */
    struct pl_dither_params dither_params;
    int dithering;
    int dither_lut_size;
    int dither_temporal;

    /* pl_cone_params */
    struct pl_cone_params cone_params;
    int cones;
    float cone_str;

    /* custom shaders */
    char *shader_path;
    void *shader_bin;
    int shader_bin_len;
    const struct pl_hook *hooks[2];
    int num_hooks;
} LibplaceboContext;

static inline enum pl_log_level get_log_level(void)
{
    int av_lev = av_log_get_level();
    return av_lev >= AV_LOG_TRACE   ? PL_LOG_TRACE :
           av_lev >= AV_LOG_DEBUG   ? PL_LOG_DEBUG :
           av_lev >= AV_LOG_VERBOSE ? PL_LOG_INFO :
           av_lev >= AV_LOG_WARNING ? PL_LOG_WARN :
           av_lev >= AV_LOG_ERROR   ? PL_LOG_ERR :
           av_lev >= AV_LOG_FATAL   ? PL_LOG_FATAL :
                                      PL_LOG_NONE;
}

static void pl_av_log(void *log_ctx, enum pl_log_level level, const char *msg)
{
    int av_lev;

    switch (level) {
    case PL_LOG_FATAL:  av_lev = AV_LOG_FATAL;   break;
    case PL_LOG_ERR:    av_lev = AV_LOG_ERROR;   break;
    case PL_LOG_WARN:   av_lev = AV_LOG_WARNING; break;
    case PL_LOG_INFO:   av_lev = AV_LOG_VERBOSE; break;
    case PL_LOG_DEBUG:  av_lev = AV_LOG_DEBUG;   break;
    case PL_LOG_TRACE:  av_lev = AV_LOG_TRACE;   break;
    default: return;
    }

    av_log(log_ctx, av_lev, "%s\n", msg);
}

static int parse_shader(AVFilterContext *avctx, const void *shader, size_t len)
{
    LibplaceboContext *s = avctx->priv;
    const struct pl_hook *hook;

    hook = pl_mpv_user_shader_parse(s->gpu, shader, len);
    if (!hook) {
        av_log(s, AV_LOG_ERROR, "Failed parsing custom shader!\n");
        return AVERROR(EINVAL);
    }

    s->hooks[s->num_hooks++] = hook;
    return 0;
}

static int find_scaler(AVFilterContext *avctx,
                       const struct pl_filter_config **opt,
                       const char *name)
{
    const struct pl_filter_preset *preset;
    if (!strcmp(name, "help")) {
        av_log(avctx, AV_LOG_INFO, "Available scaler presets:\n");
        for (preset = pl_scale_filters; preset->name; preset++)
            av_log(avctx, AV_LOG_INFO, "    %s\n", preset->name);
        return AVERROR_EXIT;
    }

    for (preset = pl_scale_filters; preset->name; preset++) {
        if (!strcmp(name, preset->name)) {
            *opt = preset->filter;
            return 0;
        }
    }

    av_log(avctx, AV_LOG_ERROR, "No such scaler preset '%s'.\n", name);
    return AVERROR(EINVAL);
}

static int update_settings(AVFilterContext *ctx)
{
    int err = 0;
    LibplaceboContext *s = ctx->priv;
    enum pl_tone_map_mode tonemapping_mode = s->tonemapping_mode;
    enum pl_gamut_mode gamut_mode = s->gamut_mode;
    uint8_t color_rgba[4];

    RET(av_parse_color(color_rgba, s->fillcolor, -1, s));

#if FF_API_LIBPLACEBO_OPTS
    /* backwards compatibility with older API */
    if (!tonemapping_mode && (s->desat_str >= 0.0f || s->desat_exp >= 0.0f)) {
        float str = s->desat_str < 0.0f ? 0.9f : s->desat_str;
        float exp = s->desat_exp < 0.0f ? 0.2f : s->desat_exp;
        if (str >= 0.9f && exp <= 0.1f) {
            tonemapping_mode = PL_TONE_MAP_RGB;
        } else if (str > 0.1f) {
            tonemapping_mode = PL_TONE_MAP_HYBRID;
        } else {
            tonemapping_mode = PL_TONE_MAP_LUMA;
        }
    }

    if (s->gamut_warning)
        gamut_mode = PL_GAMUT_WARN;
    if (s->gamut_clipping)
        gamut_mode = PL_GAMUT_DESATURATE;
#endif

    s->deband_params = *pl_deband_params(
        .iterations = s->deband_iterations,
        .threshold = s->deband_threshold,
        .radius = s->deband_radius,
        .grain = s->deband_grain,
    );

    s->color_adjustment = (struct pl_color_adjustment) {
        .brightness = s->brightness,
        .contrast = s->contrast,
        .saturation = s->saturation,
        .hue = s->hue,
        .gamma = s->gamma,
    };

    s->peak_detect_params = *pl_peak_detect_params(
        .smoothing_period = s->smoothing,
        .minimum_peak = s->min_peak,
        .scene_threshold_low = s->scene_low,
        .scene_threshold_high = s->scene_high,
        .overshoot_margin = s->overshoot,
    );

    s->color_map_params = *pl_color_map_params(
        .intent = s->intent,
        .gamut_mode = gamut_mode,
        .tone_mapping_function = tonemapping_funcs[s->tonemapping],
        .tone_mapping_param = s->tonemapping_param,
        .tone_mapping_mode = tonemapping_mode,
        .inverse_tone_mapping = s->inverse_tonemapping,
        .tone_mapping_crosstalk = s->crosstalk,
        .lut_size = s->tonemapping_lut_size,
    );

    s->dither_params = *pl_dither_params(
        .method = s->dithering,
        .lut_size = s->dither_lut_size,
        .temporal = s->dither_temporal,
    );

    s->cone_params = *pl_cone_params(
        .cones = s->cones,
        .strength = s->cone_str,
    );

    s->params = *pl_render_params(
        .lut_entries = s->lut_entries,
        .antiringing_strength = s->antiringing,
        .background_transparency = 1.0f - (float) color_rgba[3] / UINT8_MAX,
        .background_color = {
            (float) color_rgba[0] / UINT8_MAX,
            (float) color_rgba[1] / UINT8_MAX,
            (float) color_rgba[2] / UINT8_MAX,
        },

        .deband_params = s->deband ? &s->deband_params : NULL,
        .sigmoid_params = s->sigmoid ? &pl_sigmoid_default_params : NULL,
        .color_adjustment = &s->color_adjustment,
        .peak_detect_params = s->peakdetect ? &s->peak_detect_params : NULL,
        .color_map_params = &s->color_map_params,
        .dither_params = s->dithering >= 0 ? &s->dither_params : NULL,
        .cone_params = s->cones ? &s->cone_params : NULL,

        .hooks = s->hooks,
        .num_hooks = s->num_hooks,

        .skip_anti_aliasing = s->skip_aa,
        .skip_caching_single_frame = s->skip_cache,
        .polar_cutoff = s->polar_cutoff,
        .disable_linear_scaling = s->disable_linear,
        .disable_builtin_scalers = s->disable_builtin,
        .force_dither = s->force_dither,
        .disable_fbos = s->disable_fbos,
    );

    RET(find_scaler(ctx, &s->params.upscaler, s->upscaler));
    RET(find_scaler(ctx, &s->params.downscaler, s->downscaler));
    return 0;

fail:
    return err;
}

static void libplacebo_uninit(AVFilterContext *avctx);

static int libplacebo_init(AVFilterContext *avctx)
{
    int err = 0;
    LibplaceboContext *s = avctx->priv;

    /* Create libplacebo log context */
    s->log = pl_log_create(PL_API_VER, pl_log_params(
        .log_level = get_log_level(),
        .log_cb = pl_av_log,
        .log_priv = s,
    ));

    if (!s->log)
        return AVERROR(ENOMEM);

    if (s->out_format_string) {
        s->out_format = av_get_pix_fmt(s->out_format_string);
        if (s->out_format == AV_PIX_FMT_NONE) {
            av_log(avctx, AV_LOG_ERROR, "Invalid output format: %s\n",
                   s->out_format_string);
            libplacebo_uninit(avctx);
            return AVERROR(EINVAL);
        }
    } else {
        s->out_format = AV_PIX_FMT_NONE;
    }

    RET(update_settings(avctx));
    RET(av_expr_parse(&s->crop_x_pexpr, s->crop_x_expr, var_names,
                      NULL, NULL, NULL, NULL, 0, s));
    RET(av_expr_parse(&s->crop_y_pexpr, s->crop_y_expr, var_names,
                      NULL, NULL, NULL, NULL, 0, s));
    RET(av_expr_parse(&s->crop_w_pexpr, s->crop_w_expr, var_names,
                      NULL, NULL, NULL, NULL, 0, s));
    RET(av_expr_parse(&s->crop_h_pexpr, s->crop_h_expr, var_names,
                      NULL, NULL, NULL, NULL, 0, s));
    RET(av_expr_parse(&s->pos_x_pexpr, s->pos_x_expr, var_names,
                      NULL, NULL, NULL, NULL, 0, s));
    RET(av_expr_parse(&s->pos_y_pexpr, s->pos_y_expr, var_names,
                      NULL, NULL, NULL, NULL, 0, s));
    RET(av_expr_parse(&s->pos_w_pexpr, s->pos_w_expr, var_names,
                      NULL, NULL, NULL, NULL, 0, s));
    RET(av_expr_parse(&s->pos_h_pexpr, s->pos_h_expr, var_names,
                      NULL, NULL, NULL, NULL, 0, s));

    /* Initialize dynamic filter state */
    s->out_pts = av_fifo_alloc2(1, sizeof(int64_t), AV_FIFO_FLAG_AUTO_GROW);
    if (strcmp(s->fps_string, "none") != 0)
        RET(av_parse_video_rate(&s->fps, s->fps_string));

    /* Note: s->vulkan etc. are initialized later, when hwctx is available */
    return 0;

fail:
    return err;
}

static int init_vulkan(AVFilterContext *avctx, const AVVulkanDeviceContext *hwctx)
{
    int err = 0;
    LibplaceboContext *s = avctx->priv;
    uint8_t *buf = NULL;
    size_t buf_len;

    if (hwctx) {
        /* Import libavfilter vulkan context into libplacebo */
        s->vulkan = pl_vulkan_import(s->log, pl_vulkan_import_params(
            .instance       = hwctx->inst,
            .get_proc_addr  = hwctx->get_proc_addr,
            .phys_device    = hwctx->phys_dev,
            .device         = hwctx->act_dev,
            .extensions     = hwctx->enabled_dev_extensions,
            .num_extensions = hwctx->nb_enabled_dev_extensions,
            .features       = &hwctx->device_features,
            .queue_graphics = {
                .index = hwctx->queue_family_index,
                .count = hwctx->nb_graphics_queues,
            },
            .queue_compute = {
                .index = hwctx->queue_family_comp_index,
                .count = hwctx->nb_comp_queues,
            },
            .queue_transfer = {
                .index = hwctx->queue_family_tx_index,
                .count = hwctx->nb_tx_queues,
            },
            /* This is the highest version created by hwcontext_vulkan.c */
            .max_api_version = VK_API_VERSION_1_2,
        ));
    } else {
        s->vulkan = pl_vulkan_create(s->log, pl_vulkan_params(
            .queue_count = 0, /* enable all queues for parallelization */
        ));
    }

    if (!s->vulkan) {
        av_log(s, AV_LOG_ERROR, "Failed %s Vulkan device!\n",
               hwctx ? "importing" : "creating");
        err = AVERROR_EXTERNAL;
        goto fail;
    }

    /* Create the renderer */
    s->gpu = s->vulkan->gpu;
    s->renderer = pl_renderer_create(s->log, s->gpu);
    s->queue = pl_queue_create(s->gpu);

    /* Parse the user shaders, if requested */
    if (s->shader_bin_len)
        RET(parse_shader(avctx, s->shader_bin, s->shader_bin_len));

    if (s->shader_path && s->shader_path[0]) {
        RET(av_file_map(s->shader_path, &buf, &buf_len, 0, s));
        RET(parse_shader(avctx, buf, buf_len));
    }

    /* fall through */
fail:
    if (buf)
        av_file_unmap(buf, buf_len);
    return err;
}

static void libplacebo_uninit(AVFilterContext *avctx)
{
    LibplaceboContext *s = avctx->priv;

    for (int i = 0; i < FF_ARRAY_ELEMS(s->tex); i++)
        pl_tex_destroy(s->gpu, &s->tex[i]);
    for (int i = 0; i < s->num_hooks; i++)
        pl_mpv_user_shader_destroy(&s->hooks[i]);
    pl_renderer_destroy(&s->renderer);
    pl_queue_destroy(&s->queue);
    pl_vulkan_destroy(&s->vulkan);
    pl_log_destroy(&s->log);
    ff_vk_uninit(&s->vkctx);
    s->gpu = NULL;

    av_expr_free(s->crop_x_pexpr);
    av_expr_free(s->crop_y_pexpr);
    av_expr_free(s->crop_w_pexpr);
    av_expr_free(s->crop_h_pexpr);
    av_expr_free(s->pos_x_pexpr);
    av_expr_free(s->pos_y_pexpr);
    av_expr_free(s->pos_w_pexpr);
    av_expr_free(s->pos_h_pexpr);
    av_fifo_freep2(&s->out_pts);
}

static int libplacebo_process_command(AVFilterContext *ctx, const char *cmd,
                                      const char *arg, char *res, int res_len,
                                      int flags)
{
    int err = 0;
    RET(ff_filter_process_command(ctx, cmd, arg, res, res_len, flags));
    RET(update_settings(ctx));
    return 0;

fail:
    return err;
}

static void update_crops(AVFilterContext *ctx,
                         struct pl_frame_mix *mix, struct pl_frame *target,
                         uint64_t ref_sig, double base_pts)
{
    LibplaceboContext *s = ctx->priv;

    for (int i = 0; i < mix->num_frames; i++) {
        // Mutate the `pl_frame.crop` fields in-place. This is fine because we
        // own the entire pl_queue, and hence, the pointed-at frames.
        struct pl_frame *image = (struct pl_frame *) mix->frames[i];
        double image_pts = base_pts + mix->timestamps[i];

        /* Update dynamic variables */
        s->var_values[VAR_IN_T]   = s->var_values[VAR_T]  = image_pts;
        s->var_values[VAR_OUT_T]  = s->var_values[VAR_OT] = base_pts;
        s->var_values[VAR_N]      = ctx->outputs[0]->frame_count_out;

        /* Clear these explicitly to avoid leaking previous frames' state */
        s->var_values[VAR_CROP_W] = s->var_values[VAR_CW] = NAN;
        s->var_values[VAR_CROP_H] = s->var_values[VAR_CH] = NAN;
        s->var_values[VAR_POS_W]  = s->var_values[VAR_PW] = NAN;
        s->var_values[VAR_POS_H]  = s->var_values[VAR_PH] = NAN;

        /* Compute dimensions first and placement second */
        s->var_values[VAR_CROP_W] = s->var_values[VAR_CW] =
            av_expr_eval(s->crop_w_pexpr, s->var_values, NULL);
        s->var_values[VAR_CROP_H] = s->var_values[VAR_CH] =
            av_expr_eval(s->crop_h_pexpr, s->var_values, NULL);
        s->var_values[VAR_POS_W]  = s->var_values[VAR_PW] =
            av_expr_eval(s->pos_w_pexpr, s->var_values, NULL);
        s->var_values[VAR_POS_H]  = s->var_values[VAR_PH] =
            av_expr_eval(s->pos_h_pexpr, s->var_values, NULL);

        image->crop.x0 = av_expr_eval(s->crop_x_pexpr, s->var_values, NULL);
        image->crop.y0 = av_expr_eval(s->crop_y_pexpr, s->var_values, NULL);
        image->crop.x1 = image->crop.x0 + s->var_values[VAR_CROP_W];
        image->crop.y1 = image->crop.y0 + s->var_values[VAR_CROP_H];

        if (mix->signatures[i] == ref_sig) {
            /* Only update the target crop once, for the 'reference' frame */
            target->crop.x0 = av_expr_eval(s->pos_x_pexpr, s->var_values, NULL);
            target->crop.y0 = av_expr_eval(s->pos_y_pexpr, s->var_values, NULL);
            target->crop.x1 = target->crop.x0 + s->var_values[VAR_POS_W];
            target->crop.y1 = target->crop.y0 + s->var_values[VAR_POS_H];

            if (s->target_sar.num) {
                float aspect = pl_rect2df_aspect(&target->crop) * av_q2d(s->target_sar);
                pl_rect2df_aspect_set(&target->crop, aspect, s->pad_crop_ratio);
            }
        }
    }
}

/* Construct and emit an output frame for a given frame mix */
static int output_frame_mix(AVFilterContext *ctx,
                            struct pl_frame_mix *mix,
                            int64_t pts)
{
    int err = 0, ok, changed_csp;
    LibplaceboContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    const AVPixFmtDescriptor *outdesc = av_pix_fmt_desc_get(outlink->format);
    struct pl_frame target;
    const AVFrame *ref;
    AVFrame *out;
    uint64_t ref_sig;
    if (!mix->num_frames)
        return 0;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out)
        return AVERROR(ENOMEM);

    /* Use the last frame before current PTS value as reference */
    for (int i = 0; i < mix->num_frames; i++) {
        if (i && mix->timestamps[i] > 0.0f)
            break;
        ref = pl_get_mapped_avframe(mix->frames[i]);
        ref_sig = mix->signatures[i];
    }

    RET(av_frame_copy_props(out, ref));
    out->pts = pts;
    out->width = outlink->w;
    out->height = outlink->h;
    if (s->fps.num)
        out->duration = 1;

    if (s->apply_dovi && av_frame_get_side_data(ref, AV_FRAME_DATA_DOVI_METADATA)) {
        /* Output of dovi reshaping is always BT.2020+PQ, so infer the correct
         * output colorspace defaults */
        out->colorspace = AVCOL_SPC_BT2020_NCL;
        out->color_primaries = AVCOL_PRI_BT2020;
        out->color_trc = AVCOL_TRC_SMPTE2084;
    }

    if (s->colorspace >= 0)
        out->colorspace = s->colorspace;
    if (s->color_range >= 0)
        out->color_range = s->color_range;
    if (s->color_trc >= 0)
        out->color_trc = s->color_trc;
    if (s->color_primaries >= 0)
        out->color_primaries = s->color_primaries;

    changed_csp = ref->colorspace      != out->colorspace     ||
                  ref->color_range     != out->color_range    ||
                  ref->color_trc       != out->color_trc      ||
                  ref->color_primaries != out->color_primaries;

    /* Strip side data if no longer relevant */
    if (changed_csp) {
        av_frame_remove_side_data(out, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        av_frame_remove_side_data(out, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
        av_frame_remove_side_data(out, AV_FRAME_DATA_ICC_PROFILE);
    }
    if (s->apply_dovi || changed_csp) {
        av_frame_remove_side_data(out, AV_FRAME_DATA_DOVI_RPU_BUFFER);
        av_frame_remove_side_data(out, AV_FRAME_DATA_DOVI_METADATA);
    }
    if (s->apply_filmgrain)
        av_frame_remove_side_data(out, AV_FRAME_DATA_FILM_GRAIN_PARAMS);

    /* Map, render and unmap output frame */
    if (outdesc->flags & AV_PIX_FMT_FLAG_HWACCEL) {
        ok = pl_map_avframe_ex(s->gpu, &target, pl_avframe_params(
            .frame    = out,
            .map_dovi = false,
        ));
    } else {
        ok = pl_frame_recreate_from_avframe(s->gpu, &target, s->tex, out);
    }
    if (!ok) {
        err = AVERROR_EXTERNAL;
        goto fail;
    }

    update_crops(ctx, mix, &target, ref_sig, out->pts * av_q2d(outlink->time_base));
    pl_render_image_mix(s->renderer, mix, &target, &s->params);

    if (outdesc->flags & AV_PIX_FMT_FLAG_HWACCEL) {
        pl_unmap_avframe(s->gpu, &target);
    } else if (!pl_download_avframe(s->gpu, &target, out)) {
        err = AVERROR_EXTERNAL;
        goto fail;
    }
    return ff_filter_frame(outlink, out);

fail:
    av_frame_free(&out);
    return err;
}

static bool map_frame(pl_gpu gpu, pl_tex *tex,
                      const struct pl_source_frame *src,
                      struct pl_frame *out)
{
    AVFrame *avframe = src->frame_data;
    LibplaceboContext *s = avframe->opaque;
    bool ok = pl_map_avframe_ex(gpu, out, pl_avframe_params(
        .frame      = avframe,
        .tex        = tex,
        .map_dovi   = s->apply_dovi,
    ));

    if (!s->apply_filmgrain)
        out->film_grain.type = PL_FILM_GRAIN_NONE;

    av_frame_free(&avframe);
    return ok;
}

static void unmap_frame(pl_gpu gpu, struct pl_frame *frame,
                        const struct pl_source_frame *src)
{
    pl_unmap_avframe(gpu, frame);
}

static void discard_frame(const struct pl_source_frame *src)
{
    AVFrame *avframe = src->frame_data;
    av_frame_free(&avframe);
}

static int libplacebo_activate(AVFilterContext *ctx)
{
    int ret, status;
    LibplaceboContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *in;
    int64_t pts;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);
    pl_log_level_update(s->log, get_log_level());

    while ((ret = ff_inlink_consume_frame(inlink, &in)) > 0) {
        in->opaque = s;
        pl_queue_push(s->queue, &(struct pl_source_frame) {
            .pts         = in->pts * av_q2d(inlink->time_base),
            .duration    = in->duration * av_q2d(inlink->time_base),
            .first_field = pl_field_from_avframe(in),
            .frame_data  = in,
            .map         = map_frame,
            .unmap       = unmap_frame,
            .discard     = discard_frame,
        });

        if (!s->fps.num) {
            /* Internally queue an output frame for the same PTS */
            av_assert1(!av_cmp_q(link->time_base, outlink->time_base));
            av_fifo_write(s->out_pts, &in->pts, 1);
        }
    }

    if (ret < 0)
        return ret;

    if (ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        pts = av_rescale_q_rnd(pts, inlink->time_base, outlink->time_base,
                               AV_ROUND_UP);
        if (status == AVERROR_EOF) {
            /* Signal EOF to pl_queue, and enqueue this output frame to
             * make sure we see PL_QUEUE_EOF returned eventually */
            pl_queue_push(s->queue, NULL);
            if (!s->fps.num)
                av_fifo_write(s->out_pts, &pts, 1);
        } else {
            ff_outlink_set_status(outlink, status, pts);
            return 0;
        }
    }

    if (ff_outlink_frame_wanted(outlink)) {
        struct pl_frame_mix mix;
        enum pl_queue_status ret;

        if (s->fps.num) {
            pts = outlink->frame_count_out;
        } else if (av_fifo_peek(s->out_pts, &pts, 1, 0) < 0) {
            ff_inlink_request_frame(inlink);
            return 0;
        }

        ret = pl_queue_update(s->queue, &mix, pl_queue_params(
            .pts            = pts * av_q2d(outlink->time_base),
            .radius         = pl_frame_mix_radius(&s->params),
            .vsync_duration = av_q2d(av_inv_q(outlink->frame_rate)),
        ));

        switch (ret) {
        case PL_QUEUE_MORE:
            ff_inlink_request_frame(inlink);
            return 0;
        case PL_QUEUE_OK:
            if (!s->fps.num)
                av_fifo_drain2(s->out_pts, 1);
            return output_frame_mix(ctx, &mix, pts);
        case PL_QUEUE_EOF:
            ff_outlink_set_status(outlink, AVERROR_EOF, pts);
            return 0;
        case PL_QUEUE_ERR:
            return AVERROR_EXTERNAL;
        }

        return AVERROR_BUG;
    }

    return FFERROR_NOT_READY;
}

static int libplacebo_query_format(AVFilterContext *ctx)
{
    int err;
    LibplaceboContext *s = ctx->priv;
    const AVVulkanDeviceContext *vkhwctx = NULL;
    const AVPixFmtDescriptor *desc = NULL;
    AVFilterFormats *infmts = NULL, *outfmts = NULL;

    if (ctx->hw_device_ctx) {
        const AVHWDeviceContext *avhwctx = (void *) ctx->hw_device_ctx->data;
        if (avhwctx->type == AV_HWDEVICE_TYPE_VULKAN)
            vkhwctx = avhwctx->hwctx;
    }

    RET(init_vulkan(ctx, vkhwctx));

    while ((desc = av_pix_fmt_desc_next(desc))) {
        enum AVPixelFormat pixfmt = av_pix_fmt_desc_get_id(desc);

#if PL_API_VER < 232
        // Older libplacebo can't handle >64-bit pixel formats, so safe-guard
        // this to prevent triggering an assertion
        if (av_get_bits_per_pixel(desc) > 64)
            continue;
#endif

        if (pixfmt == AV_PIX_FMT_VULKAN) {
            if (!vkhwctx || vkhwctx->act_dev != s->vulkan->device)
                continue;
        }

        if (!pl_test_pixfmt(s->gpu, pixfmt))
            continue;

        RET(ff_add_format(&infmts, pixfmt));

        /* Filter for supported output pixel formats */
        if (desc->flags & AV_PIX_FMT_FLAG_BE)
            continue; /* BE formats are not supported by pl_download_avframe */

        /* Mask based on user specified format */
        if (s->out_format != AV_PIX_FMT_NONE) {
            if (pixfmt == AV_PIX_FMT_VULKAN && av_vkfmt_from_pixfmt(s->out_format)) {
                /* OK */
            } else if (pixfmt == s->out_format) {
                /* OK */
            } else {
                continue; /* Not OK */
            }
        }

        RET(ff_add_format(&outfmts, pixfmt));
    }

    if (!infmts || !outfmts) {
        if (s->out_format) {
            av_log(s, AV_LOG_ERROR, "Invalid output format '%s'!\n",
                   av_get_pix_fmt_name(s->out_format));
        }
        err = AVERROR(EINVAL);
        goto fail;
    }

    RET(ff_formats_ref(infmts, &ctx->inputs[0]->outcfg.formats));
    RET(ff_formats_ref(outfmts, &ctx->outputs[0]->incfg.formats));
    return 0;

fail:
    if (infmts && !infmts->refcount)
        ff_formats_unref(&infmts);
    if (outfmts && !outfmts->refcount)
        ff_formats_unref(&outfmts);
    return err;
}

static int libplacebo_config_input(AVFilterLink *inlink)
{
    AVFilterContext *avctx = inlink->dst;
    LibplaceboContext *s   = avctx->priv;

    if (inlink->format == AV_PIX_FMT_VULKAN)
        return ff_vk_filter_config_input(inlink);

    /* Forward this to the vkctx for format selection */
    s->vkctx.input_format = inlink->format;

    return 0;
}

static int libplacebo_config_output(AVFilterLink *outlink)
{
    int err;
    AVFilterContext *avctx = outlink->src;
    LibplaceboContext *s   = avctx->priv;
    AVFilterLink *inlink   = outlink->src->inputs[0];
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    const AVPixFmtDescriptor *out_desc = av_pix_fmt_desc_get(outlink->format);
    AVHWFramesContext *hwfc;
    AVVulkanFramesContext *vkfc;
    AVRational scale_sar;

    /* Frame dimensions */
    RET(ff_scale_eval_dimensions(s, s->w_expr, s->h_expr, inlink, outlink,
                                 &outlink->w, &outlink->h));

    ff_scale_adjust_dimensions(inlink, &outlink->w, &outlink->h,
                               s->force_original_aspect_ratio,
                               s->force_divisible_by);

    scale_sar = (AVRational){outlink->h * inlink->w, outlink->w * inlink->h};
    if (inlink->sample_aspect_ratio.num)
        scale_sar = av_mul_q(scale_sar, inlink->sample_aspect_ratio);

    if (s->normalize_sar) {
        /* Apply all SAR during scaling, so we don't need to set the out SAR */
        outlink->sample_aspect_ratio = (AVRational){ 1, 1 };
        s->target_sar = scale_sar;
    } else {
        /* This is consistent with other scale_* filters, which only
         * set the outlink SAR to be equal to the scale SAR iff the input SAR
         * was set to something nonzero */
        if (inlink->sample_aspect_ratio.num)
            outlink->sample_aspect_ratio = scale_sar;
    }

    /* Frame rate */
    if (s->fps.num) {
        outlink->frame_rate = s->fps;
        outlink->time_base = av_inv_q(s->fps);
        s->skip_cache = av_cmp_q(inlink->frame_rate, s->fps) > 0;
    } else {
        s->skip_cache = true;
    }

    /* Static variables */
    s->var_values[VAR_IN_W]     = s->var_values[VAR_IW] = inlink->w;
    s->var_values[VAR_IN_H]     = s->var_values[VAR_IH] = inlink->h;
    s->var_values[VAR_OUT_W]    = s->var_values[VAR_OW] = outlink->w;
    s->var_values[VAR_OUT_H]    = s->var_values[VAR_OH] = outlink->h;
    s->var_values[VAR_A]        = (double) inlink->w / inlink->h;
    s->var_values[VAR_SAR]      = inlink->sample_aspect_ratio.num ?
        av_q2d(inlink->sample_aspect_ratio) : 1.0;
    s->var_values[VAR_DAR]      = outlink->sample_aspect_ratio.num ?
        av_q2d(outlink->sample_aspect_ratio) : 1.0;
    s->var_values[VAR_HSUB]     = 1 << desc->log2_chroma_w;
    s->var_values[VAR_VSUB]     = 1 << desc->log2_chroma_h;
    s->var_values[VAR_OHSUB]    = 1 << out_desc->log2_chroma_w;
    s->var_values[VAR_OVSUB]    = 1 << out_desc->log2_chroma_h;

    if (outlink->format != AV_PIX_FMT_VULKAN)
        return 0;

    s->vkctx.output_width = outlink->w;
    s->vkctx.output_height = outlink->h;
    /* Default to re-using the input format */
    if (s->out_format == AV_PIX_FMT_NONE || s->out_format == AV_PIX_FMT_VULKAN) {
        s->vkctx.output_format = s->vkctx.input_format;
    } else {
        s->vkctx.output_format = s->out_format;
    }
    RET(ff_vk_filter_config_output(outlink));
    hwfc = (AVHWFramesContext *) outlink->hw_frames_ctx->data;
    vkfc = hwfc->hwctx;
    vkfc->usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    return 0;

fail:
    return err;
}

#define OFFSET(x) offsetof(LibplaceboContext, x)
#define STATIC (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
#define DYNAMIC (STATIC | AV_OPT_FLAG_RUNTIME_PARAM)

static const AVOption libplacebo_options[] = {
    { "w", "Output video frame width",  OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, .flags = STATIC },
    { "h", "Output video frame height", OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, .flags = STATIC },
    { "fps", "Output video frame rate", OFFSET(fps_string), AV_OPT_TYPE_STRING, {.str = "none"}, .flags = STATIC },
    { "crop_x", "Input video crop x", OFFSET(crop_x_expr), AV_OPT_TYPE_STRING, {.str = "(iw-cw)/2"}, .flags = DYNAMIC },
    { "crop_y", "Input video crop y", OFFSET(crop_y_expr), AV_OPT_TYPE_STRING, {.str = "(ih-ch)/2"}, .flags = DYNAMIC },
    { "crop_w", "Input video crop w", OFFSET(crop_w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, .flags = DYNAMIC },
    { "crop_h", "Input video crop h", OFFSET(crop_h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, .flags = DYNAMIC },
    { "pos_x", "Output video placement x", OFFSET(pos_x_expr), AV_OPT_TYPE_STRING, {.str = "(ow-pw)/2"}, .flags = DYNAMIC },
    { "pos_y", "Output video placement y", OFFSET(pos_y_expr), AV_OPT_TYPE_STRING, {.str = "(oh-ph)/2"}, .flags = DYNAMIC },
    { "pos_w", "Output video placement w", OFFSET(pos_w_expr), AV_OPT_TYPE_STRING, {.str = "ow"}, .flags = DYNAMIC },
    { "pos_h", "Output video placement h", OFFSET(pos_h_expr), AV_OPT_TYPE_STRING, {.str = "oh"}, .flags = DYNAMIC },
    { "format", "Output video format", OFFSET(out_format_string), AV_OPT_TYPE_STRING, .flags = STATIC },
    { "force_original_aspect_ratio", "decrease or increase w/h if necessary to keep the original AR", OFFSET(force_original_aspect_ratio), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 2, STATIC, "force_oar" },
        { "disable",  NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 0 }, 0, 0, STATIC, "force_oar" },
        { "decrease", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 1 }, 0, 0, STATIC, "force_oar" },
        { "increase", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 2 }, 0, 0, STATIC, "force_oar" },
    { "force_divisible_by", "enforce that the output resolution is divisible by a defined integer when force_original_aspect_ratio is used", OFFSET(force_divisible_by), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, 256, STATIC },
    { "normalize_sar", "force SAR normalization to 1:1 by adjusting pos_x/y/w/h", OFFSET(normalize_sar), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, STATIC },
    { "pad_crop_ratio", "ratio between padding and cropping when normalizing SAR (0=pad, 1=crop)", OFFSET(pad_crop_ratio), AV_OPT_TYPE_FLOAT, {.dbl=0.0}, 0.0, 1.0, DYNAMIC },
    { "fillcolor", "Background fill color", OFFSET(fillcolor), AV_OPT_TYPE_STRING, {.str = "black"}, .flags = DYNAMIC },

    {"colorspace", "select colorspace", OFFSET(colorspace), AV_OPT_TYPE_INT, {.i64=-1}, -1, AVCOL_SPC_NB-1, DYNAMIC, "colorspace"},
    {"auto", "keep the same colorspace",  0, AV_OPT_TYPE_CONST, {.i64=-1},                          INT_MIN, INT_MAX, STATIC, "colorspace"},
    {"gbr",                        NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_SPC_RGB},               INT_MIN, INT_MAX, STATIC, "colorspace"},
    {"bt709",                      NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_SPC_BT709},             INT_MIN, INT_MAX, STATIC, "colorspace"},
    {"unknown",                    NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_SPC_UNSPECIFIED},       INT_MIN, INT_MAX, STATIC, "colorspace"},
    {"bt470bg",                    NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_SPC_BT470BG},           INT_MIN, INT_MAX, STATIC, "colorspace"},
    {"smpte170m",                  NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_SPC_SMPTE170M},         INT_MIN, INT_MAX, STATIC, "colorspace"},
    {"smpte240m",                  NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_SPC_SMPTE240M},         INT_MIN, INT_MAX, STATIC, "colorspace"},
    {"ycgco",                      NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_SPC_YCGCO},             INT_MIN, INT_MAX, STATIC, "colorspace"},
    {"bt2020nc",                   NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_SPC_BT2020_NCL},        INT_MIN, INT_MAX, STATIC, "colorspace"},
    {"bt2020c",                    NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_SPC_BT2020_CL},         INT_MIN, INT_MAX, STATIC, "colorspace"},
    {"ictcp",                      NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_SPC_ICTCP},             INT_MIN, INT_MAX, STATIC, "colorspace"},

    {"range", "select color range", OFFSET(color_range), AV_OPT_TYPE_INT, {.i64=-1}, -1, AVCOL_RANGE_NB-1, DYNAMIC, "range"},
    {"auto",  "keep the same color range",   0, AV_OPT_TYPE_CONST, {.i64=-1},                       0, 0, STATIC, "range"},
    {"unspecified",                  NULL,   0, AV_OPT_TYPE_CONST, {.i64=AVCOL_RANGE_UNSPECIFIED},  0, 0, STATIC, "range"},
    {"unknown",                      NULL,   0, AV_OPT_TYPE_CONST, {.i64=AVCOL_RANGE_UNSPECIFIED},  0, 0, STATIC, "range"},
    {"limited",                      NULL,   0, AV_OPT_TYPE_CONST, {.i64=AVCOL_RANGE_MPEG},         0, 0, STATIC, "range"},
    {"tv",                           NULL,   0, AV_OPT_TYPE_CONST, {.i64=AVCOL_RANGE_MPEG},         0, 0, STATIC, "range"},
    {"mpeg",                         NULL,   0, AV_OPT_TYPE_CONST, {.i64=AVCOL_RANGE_MPEG},         0, 0, STATIC, "range"},
    {"full",                         NULL,   0, AV_OPT_TYPE_CONST, {.i64=AVCOL_RANGE_JPEG},         0, 0, STATIC, "range"},
    {"pc",                           NULL,   0, AV_OPT_TYPE_CONST, {.i64=AVCOL_RANGE_JPEG},         0, 0, STATIC, "range"},
    {"jpeg",                         NULL,   0, AV_OPT_TYPE_CONST, {.i64=AVCOL_RANGE_JPEG},         0, 0, STATIC, "range"},

    {"color_primaries", "select color primaries", OFFSET(color_primaries), AV_OPT_TYPE_INT, {.i64=-1}, -1, AVCOL_PRI_NB-1, DYNAMIC, "color_primaries"},
    {"auto", "keep the same color primaries",  0, AV_OPT_TYPE_CONST, {.i64=-1},                     INT_MIN, INT_MAX, STATIC, "color_primaries"},
    {"bt709",                           NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_PRI_BT709},        INT_MIN, INT_MAX, STATIC, "color_primaries"},
    {"unknown",                         NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_PRI_UNSPECIFIED},  INT_MIN, INT_MAX, STATIC, "color_primaries"},
    {"bt470m",                          NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_PRI_BT470M},       INT_MIN, INT_MAX, STATIC, "color_primaries"},
    {"bt470bg",                         NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_PRI_BT470BG},      INT_MIN, INT_MAX, STATIC, "color_primaries"},
    {"smpte170m",                       NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_PRI_SMPTE170M},    INT_MIN, INT_MAX, STATIC, "color_primaries"},
    {"smpte240m",                       NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_PRI_SMPTE240M},    INT_MIN, INT_MAX, STATIC, "color_primaries"},
    {"film",                            NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_PRI_FILM},         INT_MIN, INT_MAX, STATIC, "color_primaries"},
    {"bt2020",                          NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_PRI_BT2020},       INT_MIN, INT_MAX, STATIC, "color_primaries"},
    {"smpte428",                        NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_PRI_SMPTE428},     INT_MIN, INT_MAX, STATIC, "color_primaries"},
    {"smpte431",                        NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_PRI_SMPTE431},     INT_MIN, INT_MAX, STATIC, "color_primaries"},
    {"smpte432",                        NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_PRI_SMPTE432},     INT_MIN, INT_MAX, STATIC, "color_primaries"},
    {"jedec-p22",                       NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_PRI_JEDEC_P22},    INT_MIN, INT_MAX, STATIC, "color_primaries"},
    {"ebu3213",                         NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_PRI_EBU3213},      INT_MIN, INT_MAX, STATIC, "color_primaries"},

    {"color_trc", "select color transfer", OFFSET(color_trc), AV_OPT_TYPE_INT, {.i64=-1}, -1, AVCOL_TRC_NB-1, DYNAMIC, "color_trc"},
    {"auto", "keep the same color transfer",  0, AV_OPT_TYPE_CONST, {.i64=-1},                     INT_MIN, INT_MAX, STATIC, "color_trc"},
    {"bt709",                          NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_TRC_BT709},        INT_MIN, INT_MAX, STATIC, "color_trc"},
    {"unknown",                        NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_TRC_UNSPECIFIED},  INT_MIN, INT_MAX, STATIC, "color_trc"},
    {"bt470m",                         NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_TRC_GAMMA22},      INT_MIN, INT_MAX, STATIC, "color_trc"},
    {"bt470bg",                        NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_TRC_GAMMA28},      INT_MIN, INT_MAX, STATIC, "color_trc"},
    {"smpte170m",                      NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_TRC_SMPTE170M},    INT_MIN, INT_MAX, STATIC, "color_trc"},
    {"smpte240m",                      NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_TRC_SMPTE240M},    INT_MIN, INT_MAX, STATIC, "color_trc"},
    {"linear",                         NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_TRC_LINEAR},       INT_MIN, INT_MAX, STATIC, "color_trc"},
    {"iec61966-2-4",                   NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_TRC_IEC61966_2_4}, INT_MIN, INT_MAX, STATIC, "color_trc"},
    {"bt1361e",                        NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_TRC_BT1361_ECG},   INT_MIN, INT_MAX, STATIC, "color_trc"},
    {"iec61966-2-1",                   NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_TRC_IEC61966_2_1}, INT_MIN, INT_MAX, STATIC, "color_trc"},
    {"bt2020-10",                      NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_TRC_BT2020_10},    INT_MIN, INT_MAX, STATIC, "color_trc"},
    {"bt2020-12",                      NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_TRC_BT2020_12},    INT_MIN, INT_MAX, STATIC, "color_trc"},
    {"smpte2084",                      NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_TRC_SMPTE2084},    INT_MIN, INT_MAX, STATIC, "color_trc"},
    {"arib-std-b67",                   NULL,  0, AV_OPT_TYPE_CONST, {.i64=AVCOL_TRC_ARIB_STD_B67}, INT_MIN, INT_MAX, STATIC, "color_trc"},

    { "upscaler", "Upscaler function", OFFSET(upscaler), AV_OPT_TYPE_STRING, {.str = "spline36"}, .flags = DYNAMIC },
    { "downscaler", "Downscaler function", OFFSET(downscaler), AV_OPT_TYPE_STRING, {.str = "mitchell"}, .flags = DYNAMIC },
    { "lut_entries", "Number of scaler LUT entries", OFFSET(lut_entries), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 256, DYNAMIC },
    { "antiringing", "Antiringing strength (for non-EWA filters)", OFFSET(antiringing), AV_OPT_TYPE_FLOAT, {.dbl = 0.0}, 0.0, 1.0, DYNAMIC },
    { "sigmoid", "Enable sigmoid upscaling", OFFSET(sigmoid), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, DYNAMIC },
    { "apply_filmgrain", "Apply film grain metadata", OFFSET(apply_filmgrain), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, DYNAMIC },
    { "apply_dolbyvision", "Apply Dolby Vision metadata", OFFSET(apply_dovi), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, DYNAMIC },

    { "deband", "Enable debanding", OFFSET(deband), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DYNAMIC },
    { "deband_iterations", "Deband iterations", OFFSET(deband_iterations), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 16, DYNAMIC },
    { "deband_threshold", "Deband threshold", OFFSET(deband_threshold), AV_OPT_TYPE_FLOAT, {.dbl = 4.0}, 0.0, 1024.0, DYNAMIC },
    { "deband_radius", "Deband radius", OFFSET(deband_radius), AV_OPT_TYPE_FLOAT, {.dbl = 16.0}, 0.0, 1024.0, DYNAMIC },
    { "deband_grain", "Deband grain", OFFSET(deband_grain), AV_OPT_TYPE_FLOAT, {.dbl = 6.0}, 0.0, 1024.0, DYNAMIC },

    { "brightness", "Brightness boost", OFFSET(brightness), AV_OPT_TYPE_FLOAT, {.dbl = 0.0}, -1.0, 1.0, DYNAMIC },
    { "contrast", "Contrast gain", OFFSET(contrast), AV_OPT_TYPE_FLOAT, {.dbl = 1.0}, 0.0, 16.0, DYNAMIC },
    { "saturation", "Saturation gain", OFFSET(saturation), AV_OPT_TYPE_FLOAT, {.dbl = 1.0}, 0.0, 16.0, DYNAMIC },
    { "hue", "Hue shift", OFFSET(hue), AV_OPT_TYPE_FLOAT, {.dbl = 0.0}, -M_PI, M_PI, DYNAMIC },
    { "gamma", "Gamma adjustment", OFFSET(gamma), AV_OPT_TYPE_FLOAT, {.dbl = 1.0}, 0.0, 16.0, DYNAMIC },

    { "peak_detect", "Enable dynamic peak detection for HDR tone-mapping", OFFSET(peakdetect), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, DYNAMIC },
    { "smoothing_period", "Peak detection smoothing period", OFFSET(smoothing), AV_OPT_TYPE_FLOAT, {.dbl = 100.0}, 0.0, 1000.0, DYNAMIC },
    { "minimum_peak", "Peak detection minimum peak", OFFSET(min_peak), AV_OPT_TYPE_FLOAT, {.dbl = 1.0}, 0.0, 100.0, DYNAMIC },
    { "scene_threshold_low", "Scene change low threshold", OFFSET(scene_low), AV_OPT_TYPE_FLOAT, {.dbl = 5.5}, -1.0, 100.0, DYNAMIC },
    { "scene_threshold_high", "Scene change high threshold", OFFSET(scene_high), AV_OPT_TYPE_FLOAT, {.dbl = 10.0}, -1.0, 100.0, DYNAMIC },
    { "overshoot", "Tone-mapping overshoot margin", OFFSET(overshoot), AV_OPT_TYPE_FLOAT, {.dbl = 0.05}, 0.0, 1.0, DYNAMIC },

    { "intent", "Rendering intent", OFFSET(intent), AV_OPT_TYPE_INT, {.i64 = PL_INTENT_RELATIVE_COLORIMETRIC}, 0, 3, DYNAMIC, "intent" },
        { "perceptual", "Perceptual", 0, AV_OPT_TYPE_CONST, {.i64 = PL_INTENT_PERCEPTUAL}, 0, 0, STATIC, "intent" },
        { "relative", "Relative colorimetric", 0, AV_OPT_TYPE_CONST, {.i64 = PL_INTENT_RELATIVE_COLORIMETRIC}, 0, 0, STATIC, "intent" },
        { "absolute", "Absolute colorimetric", 0, AV_OPT_TYPE_CONST, {.i64 = PL_INTENT_ABSOLUTE_COLORIMETRIC}, 0, 0, STATIC, "intent" },
        { "saturation", "Saturation mapping", 0, AV_OPT_TYPE_CONST, {.i64 = PL_INTENT_SATURATION}, 0, 0, STATIC, "intent" },
    { "gamut_mode", "Gamut-mapping mode", OFFSET(gamut_mode), AV_OPT_TYPE_INT, {.i64 = PL_GAMUT_CLIP}, 0, PL_GAMUT_MODE_COUNT - 1, DYNAMIC, "gamut_mode" },
        { "clip", "Hard-clip gamut boundary", 0, AV_OPT_TYPE_CONST, {.i64 = PL_GAMUT_CLIP}, 0, 0, STATIC, "gamut_mode" },
        { "warn", "Highlight out-of-gamut colors", 0, AV_OPT_TYPE_CONST, {.i64 = PL_GAMUT_WARN}, 0, 0, STATIC, "gamut_mode" },
        { "darken", "Darken image to fit gamut", 0, AV_OPT_TYPE_CONST, {.i64 = PL_GAMUT_DARKEN}, 0, 0, STATIC, "gamut_mode" },
        { "desaturate", "Colorimetrically desaturate colors", 0, AV_OPT_TYPE_CONST, {.i64 = PL_GAMUT_DESATURATE}, 0, 0, STATIC, "gamut_mode" },
    { "tonemapping", "Tone-mapping algorithm", OFFSET(tonemapping), AV_OPT_TYPE_INT, {.i64 = TONE_MAP_AUTO}, 0, TONE_MAP_COUNT - 1, DYNAMIC, "tonemap" },
        { "auto", "Automatic selection", 0, AV_OPT_TYPE_CONST, {.i64 = TONE_MAP_AUTO}, 0, 0, STATIC, "tonemap" },
        { "clip", "No tone mapping (clip", 0, AV_OPT_TYPE_CONST, {.i64 = TONE_MAP_CLIP}, 0, 0, STATIC, "tonemap" },
#if PL_API_VER >= 246
        { "st2094-40", "SMPTE ST 2094-40", 0, AV_OPT_TYPE_CONST, {.i64 = TONE_MAP_ST2094_40}, 0, 0, STATIC, "tonemap" },
        { "st2094-10", "SMPTE ST 2094-10", 0, AV_OPT_TYPE_CONST, {.i64 = TONE_MAP_ST2094_10}, 0, 0, STATIC, "tonemap" },
#endif
        { "bt.2390", "ITU-R BT.2390 EETF", 0, AV_OPT_TYPE_CONST, {.i64 = TONE_MAP_BT2390}, 0, 0, STATIC, "tonemap" },
        { "bt.2446a", "ITU-R BT.2446 Method A", 0, AV_OPT_TYPE_CONST, {.i64 = TONE_MAP_BT2446A}, 0, 0, STATIC, "tonemap" },
        { "spline", "Single-pivot polynomial spline", 0, AV_OPT_TYPE_CONST, {.i64 = TONE_MAP_SPLINE}, 0, 0, STATIC, "tonemap" },
        { "reinhard", "Reinhard", 0, AV_OPT_TYPE_CONST, {.i64 = TONE_MAP_REINHARD}, 0, 0, STATIC, "tonemap" },
        { "mobius", "Mobius", 0, AV_OPT_TYPE_CONST, {.i64 = TONE_MAP_MOBIUS}, 0, 0, STATIC, "tonemap" },
        { "hable", "Filmic tone-mapping (Hable)", 0, AV_OPT_TYPE_CONST, {.i64 = TONE_MAP_HABLE}, 0, 0, STATIC, "tonemap" },
        { "gamma", "Gamma function with knee", 0, AV_OPT_TYPE_CONST, {.i64 = TONE_MAP_GAMMA}, 0, 0, STATIC, "tonemap" },
        { "linear", "Perceptually linear stretch", 0, AV_OPT_TYPE_CONST, {.i64 = TONE_MAP_LINEAR}, 0, 0, STATIC, "tonemap" },
    { "tonemapping_param", "Tunable parameter for some tone-mapping functions", OFFSET(tonemapping_param), AV_OPT_TYPE_FLOAT, {.dbl = 0.0}, 0.0, 100.0, .flags = DYNAMIC },
    { "tonemapping_mode", "Tone-mapping mode", OFFSET(tonemapping_mode), AV_OPT_TYPE_INT, {.i64 = PL_TONE_MAP_AUTO}, 0, PL_TONE_MAP_MODE_COUNT - 1, DYNAMIC, "tonemap_mode" },
        { "auto", "Automatic selection", 0, AV_OPT_TYPE_CONST, {.i64 = PL_TONE_MAP_AUTO}, 0, 0, STATIC, "tonemap_mode" },
        { "rgb", "Per-channel (RGB)", 0, AV_OPT_TYPE_CONST, {.i64 = PL_TONE_MAP_RGB}, 0, 0, STATIC, "tonemap_mode" },
        { "max", "Maximum component", 0, AV_OPT_TYPE_CONST, {.i64 = PL_TONE_MAP_MAX}, 0, 0, STATIC, "tonemap_mode" },
        { "hybrid", "Hybrid of Luma/RGB", 0, AV_OPT_TYPE_CONST, {.i64 = PL_TONE_MAP_HYBRID}, 0, 0, STATIC, "tonemap_mode" },
        { "luma", "Luminance", 0, AV_OPT_TYPE_CONST, {.i64 = PL_TONE_MAP_LUMA}, 0, 0, STATIC, "tonemap_mode" },
    { "inverse_tonemapping", "Inverse tone mapping (range expansion)", OFFSET(inverse_tonemapping), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DYNAMIC },
    { "tonemapping_crosstalk", "Crosstalk factor for tone-mapping", OFFSET(crosstalk), AV_OPT_TYPE_FLOAT, {.dbl = 0.04}, 0.0, 0.30, DYNAMIC },
    { "tonemapping_lut_size", "Tone-mapping LUT size", OFFSET(tonemapping_lut_size), AV_OPT_TYPE_INT, {.i64 = 256}, 2, 1024, DYNAMIC },

#if FF_API_LIBPLACEBO_OPTS
    /* deprecated options for backwards compatibility, defaulting to -1 to not override the new defaults */
    { "desaturation_strength", "Desaturation strength", OFFSET(desat_str), AV_OPT_TYPE_FLOAT, {.dbl = -1.0}, -1.0, 1.0, DYNAMIC | AV_OPT_FLAG_DEPRECATED },
    { "desaturation_exponent", "Desaturation exponent", OFFSET(desat_exp), AV_OPT_TYPE_FLOAT, {.dbl = -1.0}, -1.0, 10.0, DYNAMIC | AV_OPT_FLAG_DEPRECATED },
    { "gamut_warning", "Highlight out-of-gamut colors", OFFSET(gamut_warning), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DYNAMIC | AV_OPT_FLAG_DEPRECATED },
    { "gamut_clipping", "Enable colorimetric gamut clipping", OFFSET(gamut_clipping), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DYNAMIC | AV_OPT_FLAG_DEPRECATED },
#endif

    { "dithering", "Dither method to use", OFFSET(dithering), AV_OPT_TYPE_INT, {.i64 = PL_DITHER_BLUE_NOISE}, -1, PL_DITHER_METHOD_COUNT - 1, DYNAMIC, "dither" },
        { "none", "Disable dithering", 0, AV_OPT_TYPE_CONST, {.i64 = -1}, 0, 0, STATIC, "dither" },
        { "blue", "Blue noise", 0, AV_OPT_TYPE_CONST, {.i64 = PL_DITHER_BLUE_NOISE}, 0, 0, STATIC, "dither" },
        { "ordered", "Ordered LUT", 0, AV_OPT_TYPE_CONST, {.i64 = PL_DITHER_ORDERED_LUT}, 0, 0, STATIC, "dither" },
        { "ordered_fixed", "Fixed function ordered", 0, AV_OPT_TYPE_CONST, {.i64 = PL_DITHER_ORDERED_FIXED}, 0, 0, STATIC, "dither" },
        { "white", "White noise", 0, AV_OPT_TYPE_CONST, {.i64 = PL_DITHER_WHITE_NOISE}, 0, 0, STATIC, "dither" },
    { "dither_lut_size", "Dithering LUT size", OFFSET(dither_lut_size), AV_OPT_TYPE_INT, {.i64 = 6}, 1, 8, STATIC },
    { "dither_temporal", "Enable temporal dithering", OFFSET(dither_temporal), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DYNAMIC },

    { "cones", "Colorblindness adaptation model", OFFSET(cones), AV_OPT_TYPE_FLAGS, {.i64 = 0}, 0, PL_CONE_LMS, DYNAMIC, "cone" },
        { "l", "L cone", 0, AV_OPT_TYPE_CONST, {.i64 = PL_CONE_L}, 0, 0, STATIC, "cone" },
        { "m", "M cone", 0, AV_OPT_TYPE_CONST, {.i64 = PL_CONE_M}, 0, 0, STATIC, "cone" },
        { "s", "S cone", 0, AV_OPT_TYPE_CONST, {.i64 = PL_CONE_S}, 0, 0, STATIC, "cone" },
    { "cone-strength", "Colorblindness adaptation strength", OFFSET(cone_str), AV_OPT_TYPE_FLOAT, {.dbl = 0.0}, 0.0, 10.0, DYNAMIC },

    { "custom_shader_path", "Path to custom user shader (mpv .hook format)", OFFSET(shader_path), AV_OPT_TYPE_STRING, .flags = STATIC },
    { "custom_shader_bin", "Custom user shader as binary (mpv .hook format)", OFFSET(shader_bin), AV_OPT_TYPE_BINARY, .flags = STATIC },

    /* Performance/quality tradeoff options */
    { "skip_aa", "Skip anti-aliasing", OFFSET(skip_aa), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 0, DYNAMIC },
    { "polar_cutoff", "Polar LUT cutoff", OFFSET(polar_cutoff), AV_OPT_TYPE_FLOAT, {.dbl = 0}, 0.0, 1.0, DYNAMIC },
    { "disable_linear", "Disable linear scaling", OFFSET(disable_linear), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DYNAMIC },
    { "disable_builtin", "Disable built-in scalers", OFFSET(disable_builtin), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DYNAMIC },
#if FF_API_LIBPLACEBO_OPTS
    { "force_icc_lut", "Deprecated, does nothing", OFFSET(force_icc_lut), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DYNAMIC | AV_OPT_FLAG_DEPRECATED },
#endif
    { "force_dither", "Force dithering", OFFSET(force_dither), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DYNAMIC },
    { "disable_fbos", "Force-disable FBOs", OFFSET(disable_fbos), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DYNAMIC },
    { NULL },
};

AVFILTER_DEFINE_CLASS(libplacebo);

static const AVFilterPad libplacebo_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = &libplacebo_config_input,
    },
};

static const AVFilterPad libplacebo_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = &libplacebo_config_output,
    },
};

const AVFilter ff_vf_libplacebo = {
    .name           = "libplacebo",
    .description    = NULL_IF_CONFIG_SMALL("Apply various GPU filters from libplacebo"),
    .priv_size      = sizeof(LibplaceboContext),
    .init           = &libplacebo_init,
    .uninit         = &libplacebo_uninit,
    .activate       = &libplacebo_activate,
    .process_command = &libplacebo_process_command,
    FILTER_INPUTS(libplacebo_inputs),
    FILTER_OUTPUTS(libplacebo_outputs),
    FILTER_QUERY_FUNC(libplacebo_query_format),
    .priv_class     = &libplacebo_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .flags          = AVFILTER_FLAG_HWDEVICE,
};
