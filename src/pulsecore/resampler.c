/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <math.h>

#ifdef HAVE_LIBSAMPLERATE
#include <samplerate.h>
#endif

#ifdef HAVE_SPEEX
#include <speex/speex_resampler.h>
#endif

#include <pulse/xmalloc.h>
#include <pulsecore/sconv.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/strbuf.h>
#include <pulsecore/remap.h>
#include <pulsecore/core-util.h>

#include "resampler.h"

/* Number of samples of extra space we allow the resamplers to return */
#define EXTRA_FRAMES 128

struct pa_resampler {
    pa_resample_method_t method;
    pa_resample_flags_t flags;

    pa_sample_spec i_ss, o_ss;
    pa_channel_map i_cm, o_cm;
    size_t i_fz, o_fz, w_sz;
    pa_mempool *mempool;

    pa_memchunk to_work_format_buf;
    pa_memchunk remap_buf;
    pa_memchunk resample_buf;
    pa_memchunk from_work_format_buf;
    unsigned to_work_format_buf_samples;
    size_t remap_buf_size;
    unsigned resample_buf_samples;
    unsigned from_work_format_buf_samples;
    bool remap_buf_contains_leftover_data;

    pa_sample_format_t work_format;
    uint8_t work_channels;

    pa_convert_func_t to_work_format_func;
    pa_convert_func_t from_work_format_func;

    pa_remap_t remap;
    bool map_required;

    pa_resampler_implementation implementation;
};

static int copy_init(pa_resampler *r);

static pa_resampler_implementation copy_impl = {
    .init = copy_init,
};

static int trivial_init(pa_resampler*r);
static void trivial_resample(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames);
static void trivial_update_rates_or_reset(pa_resampler *r);

struct trivial { /* data specific to the trivial resampler */
    unsigned o_counter;
    unsigned i_counter;
};

static pa_resampler_implementation trivial_impl = {
    .init = trivial_init,
    .resample = trivial_resample,
    .update_rates = trivial_update_rates_or_reset,
    .reset = trivial_update_rates_or_reset,
};

#ifdef HAVE_SPEEX
static int speex_init(pa_resampler*r);
static void speex_free(pa_resampler *r);
static void speex_update_rates(pa_resampler *r);
static void speex_reset(pa_resampler *r);

struct speex{ /* data specific to speex */
    SpeexResamplerState* state;
};

static pa_resampler_implementation speex_impl = {
    .init = speex_init,
    .free = speex_free,
    .update_rates = speex_update_rates,
    .reset = speex_reset,
};
#endif

static int peaks_init(pa_resampler*r);
static void peaks_resample(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames);
static void peaks_update_rates_or_reset(pa_resampler *r);

static pa_resampler_implementation peaks_impl = {
    .init = peaks_init,
    .resample = peaks_resample,
    .update_rates = peaks_update_rates_or_reset,
    .reset = peaks_update_rates_or_reset,
};

struct peaks{ /* data specific to the peak finder pseudo resampler */
    unsigned o_counter;
    unsigned i_counter;

    float max_f[PA_CHANNELS_MAX];
    int16_t max_i[PA_CHANNELS_MAX];
};

#ifdef HAVE_LIBSAMPLERATE
static int libsamplerate_init(pa_resampler*r);
static void libsamplerate_resample(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames);
static void libsamplerate_update_rates(pa_resampler *r);
static void libsamplerate_reset(pa_resampler *r);
static void libsamplerate_free(pa_resampler *r);

struct src{ /* data specific to libsamplerate */
    SRC_STATE *state;
};

static pa_resampler_implementation libsamplerate_impl = {
    .init = libsamplerate_init,
    .free = libsamplerate_free,
    .resample = libsamplerate_resample,
    .update_rates = libsamplerate_update_rates,
    .reset = libsamplerate_reset,
};
#endif

static void calc_map_table(pa_resampler *r);

static pa_resampler_implementation *impl_table[] = {
#ifdef HAVE_LIBSAMPLERATE
    [PA_RESAMPLER_SRC_LINEAR] = &libsamplerate_impl,
#else
    [PA_RESAMPLER_SRC_LINEAR] = NULL,
#endif
    [PA_RESAMPLER_TRIVIAL] = &trivial_impl,
#ifdef HAVE_SPEEX
    [PA_RESAMPLER_SPEEX_FIXED_BASE] = &speex_impl,
#else
    [PA_RESAMPLER_SPEEX_FIXED_BASE] = NULL,
#endif
    [PA_RESAMPLER_COPY] = &copy_impl,
    [PA_RESAMPLER_PEAKS] = &peaks_impl,
};

static pa_resample_method_t pa_resampler_fix_method(
                pa_resample_flags_t flags,
                pa_resample_method_t method,
                const uint32_t rate_a,
                const uint32_t rate_b) {

    pa_assert(rate_a > 0 && rate_a <= PA_RATE_MAX);
    pa_assert(rate_b > 0 && rate_b <= PA_RATE_MAX);
    pa_assert(method >= 0);
    pa_assert(method < PA_RESAMPLER_MAX);

    if (!(flags & PA_RESAMPLER_VARIABLE_RATE) && rate_a == rate_b) {
        pa_log_info("Forcing resampler 'copy', because of fixed, identical sample rates.");
        method = PA_RESAMPLER_COPY;
    }

    if (!pa_resample_method_supported(method)) {
        pa_log_warn("Support for resampler '%s' not compiled in, reverting to 'auto'.", pa_resample_method_to_string(method));
        method = PA_RESAMPLER_AUTO;
    }

    switch (method) {
        case PA_RESAMPLER_COPY:
            if (flags & PA_RESAMPLER_VARIABLE_RATE) {
                pa_log_info("Resampler '%s' cannot do variable rate, reverting to resampler 'auto'.", pa_resample_method_to_string(method));
                method = PA_RESAMPLER_AUTO;
            }
            break;

        /* The Peaks resampler only supports downsampling.
         * Revert to auto if we are upsampling */
        case PA_RESAMPLER_PEAKS:
            if (rate_a < rate_b)
                method = PA_RESAMPLER_AUTO;
            break;

        default:
            break;
    }

    if (method == PA_RESAMPLER_AUTO) {
#ifdef HAVE_SPEEX
        method = PA_RESAMPLER_SPEEX_FLOAT_BASE + 1;
#else
        method = PA_RESAMPLER_TRIVIAL;
#endif
    }

    return method;
}

/* Return true if a is a more precise sample format than b, else return false */
static bool sample_format_more_precise(pa_sample_format_t a, pa_sample_format_t b) {
    pa_assert(a >= 0 && a < PA_SAMPLE_MAX);
    pa_assert(b >= 0 && b < PA_SAMPLE_MAX);

    switch (a) {
        case PA_SAMPLE_U8:
        case PA_SAMPLE_ALAW:
        case PA_SAMPLE_ULAW:
            return false;
            break;

        case PA_SAMPLE_S16LE:
        case PA_SAMPLE_S16BE:
            if (b == PA_SAMPLE_ULAW || b == PA_SAMPLE_ALAW || b == PA_SAMPLE_U8 ||
                b == PA_SAMPLE_S16LE || b == PA_SAMPLE_S16BE)
                return false;
            else
                return true;
            break;

        case PA_SAMPLE_S24LE:
        case PA_SAMPLE_S24BE:
        case PA_SAMPLE_S24_32LE:
        case PA_SAMPLE_S24_32BE:
            if (b == PA_SAMPLE_ULAW || b == PA_SAMPLE_ALAW || b == PA_SAMPLE_U8 ||
                b == PA_SAMPLE_S16LE || b == PA_SAMPLE_S16BE)
                return true;
            else
                return false;
            break;

        case PA_SAMPLE_FLOAT32LE:
        case PA_SAMPLE_FLOAT32BE:
        case PA_SAMPLE_S32LE:
        case PA_SAMPLE_S32BE:
            if (b == PA_SAMPLE_FLOAT32LE || b == PA_SAMPLE_FLOAT32BE ||
                b == PA_SAMPLE_S32LE || b == PA_SAMPLE_FLOAT32BE)
                return false;
            else
                return true;
            break;

        default:
            return false;
    }
}

static pa_sample_format_t pa_resampler_choose_work_format(
                    pa_resample_method_t method,
                    pa_sample_format_t a,
                    pa_sample_format_t b,
                    bool map_required) {
    pa_sample_format_t work_format;

    pa_assert(a >= 0 && a < PA_SAMPLE_MAX);
    pa_assert(b >= 0 && b < PA_SAMPLE_MAX);
    pa_assert(method >= 0);
    pa_assert(method < PA_RESAMPLER_MAX);

    if (method >= PA_RESAMPLER_SPEEX_FIXED_BASE && method <= PA_RESAMPLER_SPEEX_FIXED_MAX)
        method = PA_RESAMPLER_SPEEX_FIXED_BASE;

    switch (method) {
        /* This block is for resampling functions that only
         * support the S16 sample format. */
        case PA_RESAMPLER_SPEEX_FIXED_BASE:
            work_format = PA_SAMPLE_S16NE;
            break;

        /* This block is for resampling functions that support
         * any sample format. */
        case PA_RESAMPLER_COPY:                 /* fall through */
        case PA_RESAMPLER_TRIVIAL:
            if (!map_required && a == b) {
                work_format = a;
                break;
            }
                                                /* else fall through */
        case PA_RESAMPLER_PEAKS:
            if (a == PA_SAMPLE_S16NE || b == PA_SAMPLE_S16NE)
                work_format = PA_SAMPLE_S16NE;
            else if (sample_format_more_precise(a, PA_SAMPLE_S16NE) ||
                     sample_format_more_precise(b, PA_SAMPLE_S16NE))
                work_format = PA_SAMPLE_FLOAT32NE;
            else
                work_format = PA_SAMPLE_S16NE;
            break;

        default:
            work_format = PA_SAMPLE_FLOAT32NE;
    }

    return work_format;
}

static void find_implementation(pa_resampler *resampler, pa_resample_method_t method) {
    pa_assert(resampler);
    pa_assert(method >= 0);
    pa_assert(method < PA_RESAMPLER_MAX);

    if (method >= PA_RESAMPLER_SPEEX_FIXED_BASE && method <= PA_RESAMPLER_SPEEX_FIXED_MAX)
        resampler->implementation = *impl_table[PA_RESAMPLER_SPEEX_FIXED_BASE];
    else if (method >= PA_RESAMPLER_SPEEX_FLOAT_BASE && method <= PA_RESAMPLER_SPEEX_FLOAT_MAX) {
        resampler->implementation = *impl_table[PA_RESAMPLER_SPEEX_FIXED_BASE];
    } else if (method <= PA_RESAMPLER_SRC_LINEAR)
        resampler->implementation = *impl_table[PA_RESAMPLER_SRC_LINEAR];
    else
        resampler->implementation = *impl_table[method];
}

pa_resampler* pa_resampler_new(
        pa_mempool *pool,
        const pa_sample_spec *a,
        const pa_channel_map *am,
        const pa_sample_spec *b,
        const pa_channel_map *bm,
        pa_resample_method_t method,
        pa_resample_flags_t flags) {

    pa_resampler *r = NULL;

    pa_assert(pool);
    pa_assert(a);
    pa_assert(b);
    pa_assert(pa_sample_spec_valid(a));
    pa_assert(pa_sample_spec_valid(b));
    pa_assert(method >= 0);
    pa_assert(method < PA_RESAMPLER_MAX);

    method = pa_resampler_fix_method(flags, method, a->rate, b->rate);

    r = pa_xnew0(pa_resampler, 1);
    r->mempool = pool;
    r->method = method;

    find_implementation(r, method);

    r->flags = flags;

    /* Fill sample specs */
    r->i_ss = *a;
    r->o_ss = *b;

    /* set up the remap structure */
    r->remap.i_ss = &r->i_ss;
    r->remap.o_ss = &r->o_ss;
    r->remap.format = &r->work_format;

    if (am)
        r->i_cm = *am;
    else if (!pa_channel_map_init_auto(&r->i_cm, r->i_ss.channels, PA_CHANNEL_MAP_DEFAULT))
        goto fail;

    if (bm)
        r->o_cm = *bm;
    else if (!pa_channel_map_init_auto(&r->o_cm, r->o_ss.channels, PA_CHANNEL_MAP_DEFAULT))
        goto fail;

    r->i_fz = pa_frame_size(a);
    r->o_fz = pa_frame_size(b);

    calc_map_table(r);

    pa_log_info("Using resampler '%s'", pa_resample_method_to_string(method));

    r->work_format = pa_resampler_choose_work_format(method, a->format, b->format, r->map_required);

    pa_log_info("Using %s as working format.", pa_sample_format_to_string(r->work_format));

    r->w_sz = pa_sample_size_of_format(r->work_format);

    if (r->i_ss.format != r->work_format) {
        if (r->work_format == PA_SAMPLE_FLOAT32NE) {
            if (!(r->to_work_format_func = pa_get_convert_to_float32ne_function(r->i_ss.format)))
                goto fail;
        } else {
            pa_assert(r->work_format == PA_SAMPLE_S16NE);
            if (!(r->to_work_format_func = pa_get_convert_to_s16ne_function(r->i_ss.format)))
                goto fail;
        }
    }

    if (r->o_ss.format != r->work_format) {
        if (r->work_format == PA_SAMPLE_FLOAT32NE) {
            if (!(r->from_work_format_func = pa_get_convert_from_float32ne_function(r->o_ss.format)))
                goto fail;
        } else {
            pa_assert(r->work_format == PA_SAMPLE_S16NE);
            if (!(r->from_work_format_func = pa_get_convert_from_s16ne_function(r->o_ss.format)))
                goto fail;
        }
    }

    if (r->o_ss.channels <= r->i_ss.channels)
        r->work_channels = r->o_ss.channels;
    else
        r->work_channels = r->i_ss.channels;

    pa_log_debug("Resampler:\n  rate %d -> %d (method %s),\n  format %s -> %s (intermediate %s),\n  channels %d -> %d (resampling %d)",
        a->rate, b->rate, pa_resample_method_to_string(r->method),
        pa_sample_format_to_string(a->format), pa_sample_format_to_string(b->format), pa_sample_format_to_string(r->work_format),
        a->channels, b->channels, r->work_channels);

    /* initialize implementation */
    if (r->implementation.init(r) < 0)
        goto fail;

    return r;

fail:
    pa_xfree(r);

    return NULL;
}

void pa_resampler_free(pa_resampler *r) {
    pa_assert(r);

    if (r->implementation.free)
        r->implementation.free(r);

    if (r->to_work_format_buf.memblock)
        pa_memblock_unref(r->to_work_format_buf.memblock);
    if (r->remap_buf.memblock)
        pa_memblock_unref(r->remap_buf.memblock);
    if (r->resample_buf.memblock)
        pa_memblock_unref(r->resample_buf.memblock);
    if (r->from_work_format_buf.memblock)
        pa_memblock_unref(r->from_work_format_buf.memblock);

    pa_xfree(r->implementation.data);
    pa_xfree(r);
}

void pa_resampler_set_input_rate(pa_resampler *r, uint32_t rate) {
    pa_assert(r);
    pa_assert(rate > 0);

    if (r->i_ss.rate == rate)
        return;

    r->i_ss.rate = rate;

    r->implementation.update_rates(r);
}

void pa_resampler_set_output_rate(pa_resampler *r, uint32_t rate) {
    pa_assert(r);
    pa_assert(rate > 0);

    if (r->o_ss.rate == rate)
        return;

    r->o_ss.rate = rate;

    r->implementation.update_rates(r);
}

size_t pa_resampler_request(pa_resampler *r, size_t out_length) {
    pa_assert(r);

    /* Let's round up here to make it more likely that the caller will get at
     * least out_length amount of data from pa_resampler_run().
     *
     * We don't take the leftover into account here. If we did, then it might
     * be in theory possible that this function would return 0 and
     * pa_resampler_run() would also return 0. That could lead to infinite
     * loops. When the leftover is ignored here, such loops would eventually
     * terminate, because the leftover would grow each round, finally
     * surpassing the minimum input threshold of the resampler. */
    return ((((uint64_t) ((out_length + r->o_fz-1) / r->o_fz) * r->i_ss.rate) + r->o_ss.rate-1) / r->o_ss.rate) * r->i_fz;
}

size_t pa_resampler_result(pa_resampler *r, size_t in_length) {
    size_t frames;

    pa_assert(r);

    /* Let's round up here to ensure that the caller will always allocate big
     * enough output buffer. */

    frames = (in_length + r->i_fz - 1) / r->i_fz;

    if (r->remap_buf_contains_leftover_data)
        frames += r->remap_buf.length / (r->w_sz * r->o_ss.channels);

    return (((uint64_t) frames * r->o_ss.rate + r->i_ss.rate - 1) / r->i_ss.rate) * r->o_fz;
}

size_t pa_resampler_max_block_size(pa_resampler *r) {
    size_t block_size_max;
    pa_sample_spec max_ss;
    size_t max_fs;
    size_t frames;

    pa_assert(r);

    block_size_max = pa_mempool_block_size_max(r->mempool);

    /* We deduce the "largest" sample spec we're using during the
     * conversion */
    max_ss.channels = (uint8_t) (PA_MAX(r->i_ss.channels, r->o_ss.channels));

    /* We silently assume that the format enum is ordered by size */
    max_ss.format = PA_MAX(r->i_ss.format, r->o_ss.format);
    max_ss.format = PA_MAX(max_ss.format, r->work_format);

    max_ss.rate = PA_MAX(r->i_ss.rate, r->o_ss.rate);

    max_fs = pa_frame_size(&max_ss);
    frames = block_size_max / max_fs - EXTRA_FRAMES;

    if (r->remap_buf_contains_leftover_data)
        frames -= r->remap_buf.length / (r->w_sz * r->o_ss.channels);

    return ((uint64_t) frames * r->i_ss.rate / max_ss.rate) * r->i_fz;
}

void pa_resampler_reset(pa_resampler *r) {
    pa_assert(r);

    if (r->implementation.reset)
        r->implementation.reset(r);

    r->remap_buf_contains_leftover_data = false;
}

pa_resample_method_t pa_resampler_get_method(pa_resampler *r) {
    pa_assert(r);

    return r->method;
}

const pa_channel_map* pa_resampler_input_channel_map(pa_resampler *r) {
    pa_assert(r);

    return &r->i_cm;
}

const pa_sample_spec* pa_resampler_input_sample_spec(pa_resampler *r) {
    pa_assert(r);

    return &r->i_ss;
}

const pa_channel_map* pa_resampler_output_channel_map(pa_resampler *r) {
    pa_assert(r);

    return &r->o_cm;
}

const pa_sample_spec* pa_resampler_output_sample_spec(pa_resampler *r) {
    pa_assert(r);

    return &r->o_ss;
}

static const char * const resample_methods[] = {
    "src-sinc-best-quality",
    "src-sinc-medium-quality",
    "src-sinc-fastest",
    "src-zero-order-hold",
    "src-linear",
    "trivial",
    "speex-float-0",
    "speex-float-1",
    "speex-float-2",
    "speex-float-3",
    "speex-float-4",
    "speex-float-5",
    "speex-float-6",
    "speex-float-7",
    "speex-float-8",
    "speex-float-9",
    "speex-float-10",
    "speex-fixed-0",
    "speex-fixed-1",
    "speex-fixed-2",
    "speex-fixed-3",
    "speex-fixed-4",
    "speex-fixed-5",
    "speex-fixed-6",
    "speex-fixed-7",
    "speex-fixed-8",
    "speex-fixed-9",
    "speex-fixed-10",
    "auto",
    "copy",
    "peaks"
};

const char *pa_resample_method_to_string(pa_resample_method_t m) {

    if (m < 0 || m >= PA_RESAMPLER_MAX)
        return NULL;

    return resample_methods[m];
}

int pa_resample_method_supported(pa_resample_method_t m) {

    if (m < 0 || m >= PA_RESAMPLER_MAX)
        return 0;

#ifndef HAVE_LIBSAMPLERATE
    if (m <= PA_RESAMPLER_SRC_LINEAR)
        return 0;
#endif

#ifndef HAVE_SPEEX
    if (m >= PA_RESAMPLER_SPEEX_FLOAT_BASE && m <= PA_RESAMPLER_SPEEX_FLOAT_MAX)
        return 0;
    if (m >= PA_RESAMPLER_SPEEX_FIXED_BASE && m <= PA_RESAMPLER_SPEEX_FIXED_MAX)
        return 0;
#endif

    return 1;
}

pa_resample_method_t pa_parse_resample_method(const char *string) {
    pa_resample_method_t m;

    pa_assert(string);

    for (m = 0; m < PA_RESAMPLER_MAX; m++)
        if (pa_streq(string, resample_methods[m]))
            return m;

    if (pa_streq(string, "speex-fixed"))
        return PA_RESAMPLER_SPEEX_FIXED_BASE + 1;

    if (pa_streq(string, "speex-float"))
        return PA_RESAMPLER_SPEEX_FLOAT_BASE + 1;

    return PA_RESAMPLER_INVALID;
}

static bool on_left(pa_channel_position_t p) {

    return
        p == PA_CHANNEL_POSITION_FRONT_LEFT ||
        p == PA_CHANNEL_POSITION_REAR_LEFT ||
        p == PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER ||
        p == PA_CHANNEL_POSITION_SIDE_LEFT ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_LEFT ||
        p == PA_CHANNEL_POSITION_TOP_REAR_LEFT;
}

static bool on_right(pa_channel_position_t p) {

    return
        p == PA_CHANNEL_POSITION_FRONT_RIGHT ||
        p == PA_CHANNEL_POSITION_REAR_RIGHT ||
        p == PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER ||
        p == PA_CHANNEL_POSITION_SIDE_RIGHT ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_RIGHT ||
        p == PA_CHANNEL_POSITION_TOP_REAR_RIGHT;
}

static bool on_center(pa_channel_position_t p) {

    return
        p == PA_CHANNEL_POSITION_FRONT_CENTER ||
        p == PA_CHANNEL_POSITION_REAR_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_REAR_CENTER;
}

static bool on_lfe(pa_channel_position_t p) {
    return
        p == PA_CHANNEL_POSITION_LFE;
}

static bool on_front(pa_channel_position_t p) {
    return
        p == PA_CHANNEL_POSITION_FRONT_LEFT ||
        p == PA_CHANNEL_POSITION_FRONT_RIGHT ||
        p == PA_CHANNEL_POSITION_FRONT_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_LEFT ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_RIGHT ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_CENTER ||
        p == PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER ||
        p == PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
}

static bool on_rear(pa_channel_position_t p) {
    return
        p == PA_CHANNEL_POSITION_REAR_LEFT ||
        p == PA_CHANNEL_POSITION_REAR_RIGHT ||
        p == PA_CHANNEL_POSITION_REAR_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_REAR_LEFT ||
        p == PA_CHANNEL_POSITION_TOP_REAR_RIGHT ||
        p == PA_CHANNEL_POSITION_TOP_REAR_CENTER;
}

static bool on_side(pa_channel_position_t p) {
    return
        p == PA_CHANNEL_POSITION_SIDE_LEFT ||
        p == PA_CHANNEL_POSITION_SIDE_RIGHT ||
        p == PA_CHANNEL_POSITION_TOP_CENTER;
}

enum {
    ON_FRONT,
    ON_REAR,
    ON_SIDE,
    ON_OTHER
};

static int front_rear_side(pa_channel_position_t p) {
    if (on_front(p))
        return ON_FRONT;
    if (on_rear(p))
        return ON_REAR;
    if (on_side(p))
        return ON_SIDE;
    return ON_OTHER;
}

static void calc_map_table(pa_resampler *r) {
    unsigned oc, ic;
    unsigned n_oc, n_ic;
    bool ic_connected[PA_CHANNELS_MAX];
    bool remix;
    pa_strbuf *s;
    char *t;
    pa_remap_t *m;

    pa_assert(r);

    if (!(r->map_required = (r->i_ss.channels != r->o_ss.channels || (!(r->flags & PA_RESAMPLER_NO_REMAP) && !pa_channel_map_equal(&r->i_cm, &r->o_cm)))))
        return;

    m = &r->remap;

    n_oc = r->o_ss.channels;
    n_ic = r->i_ss.channels;

    memset(m->map_table_f, 0, sizeof(m->map_table_f));
    memset(m->map_table_i, 0, sizeof(m->map_table_i));

    memset(ic_connected, 0, sizeof(ic_connected));
    remix = (r->flags & (PA_RESAMPLER_NO_REMAP | PA_RESAMPLER_NO_REMIX)) == 0;

    if (r->flags & PA_RESAMPLER_NO_REMAP) {
        pa_assert(!remix);

        for (oc = 0; oc < PA_MIN(n_ic, n_oc); oc++)
            m->map_table_f[oc][oc] = 1.0f;

    } else if (r->flags & PA_RESAMPLER_NO_REMIX) {
        pa_assert(!remix);
        for (oc = 0; oc < n_oc; oc++) {
            pa_channel_position_t b = r->o_cm.map[oc];

            for (ic = 0; ic < n_ic; ic++) {
                pa_channel_position_t a = r->i_cm.map[ic];

                /* We shall not do any remixing. Hence, just check by name */
                if (a == b)
                    m->map_table_f[oc][ic] = 1.0f;
            }
        }
    } else {

        /* OK, we shall do the full monty: upmixing and downmixing. Our
         * algorithm is relatively simple, does not do spacialization, delay
         * elements or apply lowpass filters for LFE. Patches are always
         * welcome, though. Oh, and it doesn't do any matrix decoding. (Which
         * probably wouldn't make any sense anyway.)
         *
         * This code is not idempotent: downmixing an upmixed stereo stream is
         * not identical to the original. The volume will not match, and the
         * two channels will be a linear combination of both.
         *
         * This is loosely based on random suggestions found on the Internet,
         * such as this:
         * http://www.halfgaar.net/surround-sound-in-linux and the alsa upmix
         * plugin.
         *
         * The algorithm works basically like this:
         *
         * 1) Connect all channels with matching names.
         *
         * 2) Mono Handling:
         *    S:Mono: Copy into all D:channels
         *    D:Mono: Avg all S:channels
         *
         * 3) Mix D:Left, D:Right:
         *    D:Left: If not connected, avg all S:Left
         *    D:Right: If not connected, avg all S:Right
         *
         * 4) Mix D:Center
         *    If not connected, avg all S:Center
         *    If still not connected, avg all S:Left, S:Right
         *
         * 5) Mix D:LFE
         *    If not connected, avg all S:*
         *
         * 6) Make sure S:Left/S:Right is used: S:Left/S:Right: If not
         *    connected, mix into all D:left and all D:right channels. Gain is
         *    1/9.
         *
         * 7) Make sure S:Center, S:LFE is used:
         *
         *    S:Center, S:LFE: If not connected, mix into all D:left, all
         *    D:right, all D:center channels. Gain is 0.5 for center and 0.375
         *    for LFE. C-front is only mixed into L-front/R-front if available,
         *    otherwise into all L/R channels. Similarly for C-rear.
         *
         * 8) Normalize each row in the matrix such that the sum for each row is
         *    not larger than 1.0 in order to avoid clipping.
         *
         * S: and D: shall relate to the source resp. destination channels.
         *
         * Rationale: 1, 2 are probably obvious. For 3: this copies front to
         * rear if needed. For 4: we try to find some suitable C source for C,
         * if we don't find any, we avg L and R. For 5: LFE is mixed from all
         * channels. For 6: the rear channels should not be dropped entirely,
         * however have only minimal impact. For 7: movies usually encode
         * speech on the center channel. Thus we have to make sure this channel
         * is distributed to L and R if not available in the output. Also, LFE
         * is used to achieve a greater dynamic range, and thus we should try
         * to do our best to pass it to L+R.
         */

        unsigned
            ic_left = 0,
            ic_right = 0,
            ic_center = 0,
            ic_unconnected_left = 0,
            ic_unconnected_right = 0,
            ic_unconnected_center = 0,
            ic_unconnected_lfe = 0;
        bool ic_unconnected_center_mixed_in = 0;

        pa_assert(remix);

        for (ic = 0; ic < n_ic; ic++) {
            if (on_left(r->i_cm.map[ic]))
                ic_left++;
            if (on_right(r->i_cm.map[ic]))
                ic_right++;
            if (on_center(r->i_cm.map[ic]))
                ic_center++;
        }

        for (oc = 0; oc < n_oc; oc++) {
            bool oc_connected = false;
            pa_channel_position_t b = r->o_cm.map[oc];

            for (ic = 0; ic < n_ic; ic++) {
                pa_channel_position_t a = r->i_cm.map[ic];

                if (a == b || a == PA_CHANNEL_POSITION_MONO) {
                    m->map_table_f[oc][ic] = 1.0f;

                    oc_connected = true;
                    ic_connected[ic] = true;
                }
                else if (b == PA_CHANNEL_POSITION_MONO) {
                    m->map_table_f[oc][ic] = 1.0f / (float) n_ic;

                    oc_connected = true;
                    ic_connected[ic] = true;
                }
            }

            if (!oc_connected) {
                /* Try to find matching input ports for this output port */

                if (on_left(b)) {

                    /* We are not connected and on the left side, let's
                     * average all left side input channels. */

                    if (ic_left > 0)
                        for (ic = 0; ic < n_ic; ic++)
                            if (on_left(r->i_cm.map[ic])) {
                                m->map_table_f[oc][ic] = 1.0f / (float) ic_left;
                                ic_connected[ic] = true;
                            }

                    /* We ignore the case where there is no left input channel.
                     * Something is really wrong in this case anyway. */

                } else if (on_right(b)) {

                    /* We are not connected and on the right side, let's
                     * average all right side input channels. */

                    if (ic_right > 0)
                        for (ic = 0; ic < n_ic; ic++)
                            if (on_right(r->i_cm.map[ic])) {
                                m->map_table_f[oc][ic] = 1.0f / (float) ic_right;
                                ic_connected[ic] = true;
                            }

                    /* We ignore the case where there is no right input
                     * channel. Something is really wrong in this case anyway.
                     * */

                } else if (on_center(b)) {

                    if (ic_center > 0) {

                        /* We are not connected and at the center. Let's average
                         * all center input channels. */

                        for (ic = 0; ic < n_ic; ic++)
                            if (on_center(r->i_cm.map[ic])) {
                                m->map_table_f[oc][ic] = 1.0f / (float) ic_center;
                                ic_connected[ic] = true;
                            }

                    } else if (ic_left + ic_right > 0) {

                        /* Hmm, no center channel around, let's synthesize it
                         * by mixing L and R.*/

                        for (ic = 0; ic < n_ic; ic++)
                            if (on_left(r->i_cm.map[ic]) || on_right(r->i_cm.map[ic])) {
                                m->map_table_f[oc][ic] = 1.0f / (float) (ic_left + ic_right);
                                ic_connected[ic] = true;
                            }
                    }

                    /* We ignore the case where there is not even a left or
                     * right input channel. Something is really wrong in this
                     * case anyway. */

                } else if (on_lfe(b) && !(r->flags & PA_RESAMPLER_NO_LFE)) {

                    /* We are not connected and an LFE. Let's average all
                     * channels for LFE. */

                    for (ic = 0; ic < n_ic; ic++)
                        m->map_table_f[oc][ic] = 1.0f / (float) n_ic;

                    /* Please note that a channel connected to LFE doesn't
                     * really count as connected. */
                }
            }
        }

        for (ic = 0; ic < n_ic; ic++) {
            pa_channel_position_t a = r->i_cm.map[ic];

            if (ic_connected[ic])
                continue;

            if (on_left(a))
                ic_unconnected_left++;
            else if (on_right(a))
                ic_unconnected_right++;
            else if (on_center(a))
                ic_unconnected_center++;
            else if (on_lfe(a))
                ic_unconnected_lfe++;
        }

        for (ic = 0; ic < n_ic; ic++) {
            pa_channel_position_t a = r->i_cm.map[ic];

            if (ic_connected[ic])
                continue;

            for (oc = 0; oc < n_oc; oc++) {
                pa_channel_position_t b = r->o_cm.map[oc];

                if (on_left(a) && on_left(b))
                    m->map_table_f[oc][ic] = (1.f/9.f) / (float) ic_unconnected_left;

                else if (on_right(a) && on_right(b))
                    m->map_table_f[oc][ic] = (1.f/9.f) / (float) ic_unconnected_right;

                else if (on_center(a) && on_center(b)) {
                    m->map_table_f[oc][ic] = (1.f/9.f) / (float) ic_unconnected_center;
                    ic_unconnected_center_mixed_in = true;

                } else if (on_lfe(a) && !(r->flags & PA_RESAMPLER_NO_LFE))
                    m->map_table_f[oc][ic] = .375f / (float) ic_unconnected_lfe;
            }
        }

        if (ic_unconnected_center > 0 && !ic_unconnected_center_mixed_in) {
            unsigned ncenter[PA_CHANNELS_MAX];
            bool found_frs[PA_CHANNELS_MAX];

            memset(ncenter, 0, sizeof(ncenter));
            memset(found_frs, 0, sizeof(found_frs));

            /* Hmm, as it appears there was no center channel we
               could mix our center channel in. In this case, mix it into
               left and right. Using .5 as the factor. */

            for (ic = 0; ic < n_ic; ic++) {

                if (ic_connected[ic])
                    continue;

                if (!on_center(r->i_cm.map[ic]))
                    continue;

                for (oc = 0; oc < n_oc; oc++) {

                    if (!on_left(r->o_cm.map[oc]) && !on_right(r->o_cm.map[oc]))
                        continue;

                    if (front_rear_side(r->i_cm.map[ic]) == front_rear_side(r->o_cm.map[oc])) {
                        found_frs[ic] = true;
                        break;
                    }
                }

                for (oc = 0; oc < n_oc; oc++) {

                    if (!on_left(r->o_cm.map[oc]) && !on_right(r->o_cm.map[oc]))
                        continue;

                    if (!found_frs[ic] || front_rear_side(r->i_cm.map[ic]) == front_rear_side(r->o_cm.map[oc]))
                        ncenter[oc]++;
                }
            }

            for (oc = 0; oc < n_oc; oc++) {

                if (!on_left(r->o_cm.map[oc]) && !on_right(r->o_cm.map[oc]))
                    continue;

                if (ncenter[oc] <= 0)
                    continue;

                for (ic = 0; ic < n_ic; ic++) {

                    if (!on_center(r->i_cm.map[ic]))
                        continue;

                    if (!found_frs[ic] || front_rear_side(r->i_cm.map[ic]) == front_rear_side(r->o_cm.map[oc]))
                        m->map_table_f[oc][ic] = .5f / (float) ncenter[oc];
                }
            }
        }
    }

    for (oc = 0; oc < n_oc; oc++) {
        float sum = 0.0f;
        for (ic = 0; ic < n_ic; ic++)
            sum += m->map_table_f[oc][ic];

        if (sum > 1.0f)
            for (ic = 0; ic < n_ic; ic++)
                m->map_table_f[oc][ic] /= sum;
    }

    /* make an 16:16 int version of the matrix */
    for (oc = 0; oc < n_oc; oc++)
        for (ic = 0; ic < n_ic; ic++)
            m->map_table_i[oc][ic] = (int32_t) (m->map_table_f[oc][ic] * 0x10000);

    s = pa_strbuf_new();

    pa_strbuf_printf(s, "     ");
    for (ic = 0; ic < n_ic; ic++)
        pa_strbuf_printf(s, "  I%02u ", ic);
    pa_strbuf_puts(s, "\n    +");

    for (ic = 0; ic < n_ic; ic++)
        pa_strbuf_printf(s, "------");
    pa_strbuf_puts(s, "\n");

    for (oc = 0; oc < n_oc; oc++) {
        pa_strbuf_printf(s, "O%02u |", oc);

        for (ic = 0; ic < n_ic; ic++)
            pa_strbuf_printf(s, " %1.3f", m->map_table_f[oc][ic]);

        pa_strbuf_puts(s, "\n");
    }

    pa_log_debug("Channel matrix:\n%s", t = pa_strbuf_tostring_free(s));
    pa_xfree(t);

    /* initialize the remapping function */
    pa_init_remap(m);
}

static pa_memchunk* convert_to_work_format(pa_resampler *r, pa_memchunk *input) {
    unsigned n_samples;
    void *src, *dst;

    pa_assert(r);
    pa_assert(input);
    pa_assert(input->memblock);

    /* Convert the incoming sample into the work sample format and place them
     * in to_work_format_buf. */

    if (!r->to_work_format_func || !input->length)
        return input;

    n_samples = (unsigned) ((input->length / r->i_fz) * r->i_ss.channels);

    r->to_work_format_buf.index = 0;
    r->to_work_format_buf.length = r->w_sz * n_samples;

    if (!r->to_work_format_buf.memblock || r->to_work_format_buf_samples < n_samples) {
        if (r->to_work_format_buf.memblock)
            pa_memblock_unref(r->to_work_format_buf.memblock);

        r->to_work_format_buf_samples = n_samples;
        r->to_work_format_buf.memblock = pa_memblock_new(r->mempool, r->to_work_format_buf.length);
    }

    src = pa_memblock_acquire_chunk(input);
    dst = pa_memblock_acquire(r->to_work_format_buf.memblock);

    r->to_work_format_func(n_samples, src, dst);

    pa_memblock_release(input->memblock);
    pa_memblock_release(r->to_work_format_buf.memblock);

    return &r->to_work_format_buf;
}

static pa_memchunk *remap_channels(pa_resampler *r, pa_memchunk *input) {
    unsigned in_n_samples, out_n_samples, in_n_frames, out_n_frames;
    void *src, *dst;
    size_t leftover_length = 0;
    bool have_leftover;

    pa_assert(r);
    pa_assert(input);
    pa_assert(input->memblock);

    /* Remap channels and place the result in remap_buf. There may be leftover
     * data in the beginning of remap_buf. The leftover data is already
     * remapped, so it's not part of the input, it's part of the output. */

    have_leftover = r->remap_buf_contains_leftover_data;
    r->remap_buf_contains_leftover_data = false;

    if (!have_leftover && (!r->map_required || input->length <= 0))
        return input;
    else if (input->length <= 0)
        return &r->remap_buf;

    in_n_samples = (unsigned) (input->length / r->w_sz);
    in_n_frames = out_n_frames = in_n_samples / r->i_ss.channels;

    if (have_leftover) {
        leftover_length = r->remap_buf.length;
        out_n_frames += leftover_length / (r->w_sz * r->o_ss.channels);
    }

    out_n_samples = out_n_frames * r->o_ss.channels;
    r->remap_buf.length = out_n_samples * r->w_sz;

    if (have_leftover) {
        if (r->remap_buf_size < r->remap_buf.length) {
            pa_memblock *new_block = pa_memblock_new(r->mempool, r->remap_buf.length);

            src = pa_memblock_acquire(r->remap_buf.memblock);
            dst = pa_memblock_acquire(new_block);
            memcpy(dst, src, leftover_length);
            pa_memblock_release(r->remap_buf.memblock);
            pa_memblock_release(new_block);

            pa_memblock_unref(r->remap_buf.memblock);
            r->remap_buf.memblock = new_block;
            r->remap_buf_size = r->remap_buf.length;
        }

    } else {
        if (!r->remap_buf.memblock || r->remap_buf_size < r->remap_buf.length) {
            if (r->remap_buf.memblock)
                pa_memblock_unref(r->remap_buf.memblock);

            r->remap_buf_size = r->remap_buf.length;
            r->remap_buf.memblock = pa_memblock_new(r->mempool, r->remap_buf.length);
        }
    }

    src = pa_memblock_acquire_chunk(input);
    dst = (uint8_t *) pa_memblock_acquire(r->remap_buf.memblock) + leftover_length;

    if (r->map_required) {
        pa_remap_t *remap = &r->remap;

        pa_assert(remap->do_remap);
        remap->do_remap(remap, dst, src, in_n_frames);

    } else
        memcpy(dst, src, input->length);

    pa_memblock_release(input->memblock);
    pa_memblock_release(r->remap_buf.memblock);

    return &r->remap_buf;
}

static pa_memchunk *resample(pa_resampler *r, pa_memchunk *input) {
    unsigned in_n_frames, in_n_samples;
    unsigned out_n_frames, out_n_samples;

    pa_assert(r);
    pa_assert(input);

    /* Resample the data and place the result in resample_buf. */

    if (!r->implementation.resample || !input->length)
        return input;

    in_n_samples = (unsigned) (input->length / r->w_sz);
    in_n_frames = (unsigned) (in_n_samples / r->work_channels);

    out_n_frames = ((in_n_frames*r->o_ss.rate)/r->i_ss.rate)+EXTRA_FRAMES;
    out_n_samples = out_n_frames * r->work_channels;

    r->resample_buf.index = 0;
    r->resample_buf.length = r->w_sz * out_n_samples;

    if (!r->resample_buf.memblock || r->resample_buf_samples < out_n_samples) {
        if (r->resample_buf.memblock)
            pa_memblock_unref(r->resample_buf.memblock);

        r->resample_buf_samples = out_n_samples;
        r->resample_buf.memblock = pa_memblock_new(r->mempool, r->resample_buf.length);
    }

    r->implementation.resample(r, input, in_n_frames, &r->resample_buf, &out_n_frames);
    r->resample_buf.length = out_n_frames * r->w_sz * r->work_channels;

    return &r->resample_buf;
}

static pa_memchunk *convert_from_work_format(pa_resampler *r, pa_memchunk *input) {
    unsigned n_samples, n_frames;
    void *src, *dst;

    pa_assert(r);
    pa_assert(input);

    /* Convert the data into the correct sample type and place the result in
     * from_work_format_buf. */

    if (!r->from_work_format_func || !input->length)
        return input;

    n_samples = (unsigned) (input->length / r->w_sz);
    n_frames = n_samples / r->o_ss.channels;

    r->from_work_format_buf.index = 0;
    r->from_work_format_buf.length = r->o_fz * n_frames;

    if (!r->from_work_format_buf.memblock || r->from_work_format_buf_samples < n_samples) {
        if (r->from_work_format_buf.memblock)
            pa_memblock_unref(r->from_work_format_buf.memblock);

        r->from_work_format_buf_samples = n_samples;
        r->from_work_format_buf.memblock = pa_memblock_new(r->mempool, r->from_work_format_buf.length);
    }

    src = pa_memblock_acquire_chunk(input);
    dst = pa_memblock_acquire(r->from_work_format_buf.memblock);
    r->from_work_format_func(n_samples, src, dst);
    pa_memblock_release(input->memblock);
    pa_memblock_release(r->from_work_format_buf.memblock);

    return &r->from_work_format_buf;
}

void pa_resampler_run(pa_resampler *r, const pa_memchunk *in, pa_memchunk *out) {
    pa_memchunk *buf;

    pa_assert(r);
    pa_assert(in);
    pa_assert(out);
    pa_assert(in->length);
    pa_assert(in->memblock);
    pa_assert(in->length % r->i_fz == 0);

    buf = (pa_memchunk*) in;
    buf = convert_to_work_format(r, buf);
    /* Try to save resampling effort: if we have more output channels than
     * input channels, do resampling first, then remapping. */
    if (r->o_ss.channels <= r->i_ss.channels) {
        buf = remap_channels(r, buf);
        buf = resample(r, buf);
    } else {
        buf = resample(r, buf);
        buf = remap_channels(r, buf);
    }

    if (buf->length) {
        buf = convert_from_work_format(r, buf);
        *out = *buf;

        if (buf == in)
            pa_memblock_ref(buf->memblock);
        else
            pa_memchunk_reset(buf);
    } else
        pa_memchunk_reset(out);
}

static void save_leftover(pa_resampler *r, void *buf, size_t len) {
    void *dst;

    pa_assert(r);
    pa_assert(buf);
    pa_assert(len > 0);

    /* Store the leftover to remap_buf. */

    r->remap_buf.length = len;

    if (!r->remap_buf.memblock || r->remap_buf_size < r->remap_buf.length) {
        if (r->remap_buf.memblock)
            pa_memblock_unref(r->remap_buf.memblock);

        r->remap_buf_size = r->remap_buf.length;
        r->remap_buf.memblock = pa_memblock_new(r->mempool, r->remap_buf.length);
    }

    dst = pa_memblock_acquire(r->remap_buf.memblock);
    memcpy(dst, buf, r->remap_buf.length);
    pa_memblock_release(r->remap_buf.memblock);

    r->remap_buf_contains_leftover_data = true;
}

/*** libsamplerate based implementation ***/

#ifdef HAVE_LIBSAMPLERATE
static void libsamplerate_resample(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    SRC_DATA data;
    struct src *libsamplerate_data;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    libsamplerate_data = r->implementation.data;
    memset(&data, 0, sizeof(data));

    data.data_in = pa_memblock_acquire_chunk(input);
    data.input_frames = (long int) in_n_frames;

    data.data_out = pa_memblock_acquire_chunk(output);
    data.output_frames = (long int) *out_n_frames;

    data.src_ratio = (double) r->o_ss.rate / r->i_ss.rate;
    data.end_of_input = 0;

    pa_assert_se(src_process(libsamplerate_data->state, &data) == 0);

    if (data.input_frames_used < in_n_frames) {
        void *leftover_data = data.data_in + data.input_frames_used * r->work_channels;
        size_t leftover_length = (in_n_frames - data.input_frames_used) * sizeof(float) * r->work_channels;

        save_leftover(r, leftover_data, leftover_length);
    }

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    *out_n_frames = (unsigned) data.output_frames_gen;
}

static void libsamplerate_update_rates(pa_resampler *r) {
    struct src *libsamplerate_data;
    pa_assert(r);

    libsamplerate_data = r->implementation.data;
    pa_assert_se(src_set_ratio(libsamplerate_data->state, (double) r->o_ss.rate / r->i_ss.rate) == 0);
}

static void libsamplerate_reset(pa_resampler *r) {
    struct src *libsamplerate_data;
    pa_assert(r);

    libsamplerate_data = r->implementation.data;
    pa_assert_se(src_reset(libsamplerate_data->state) == 0);
}

static void libsamplerate_free(pa_resampler *r) {
    struct src *libsamplerate_data;
    pa_assert(r);

    libsamplerate_data = r->implementation.data;
    if (libsamplerate_data->state)
        src_delete(libsamplerate_data->state);
}

static int libsamplerate_init(pa_resampler *r) {
    int err;
    struct src *libsamplerate_data;

    pa_assert(r);

    libsamplerate_data = pa_xnew(struct src, 1);
    r->implementation.data = libsamplerate_data;

    if (!(libsamplerate_data->state = src_new(r->method, r->o_ss.channels, &err)))
        return -1;


    return 0;
}
#endif

#ifdef HAVE_SPEEX
/*** speex based implementation ***/

static void speex_resample_float(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    float *in, *out;
    uint32_t inf = in_n_frames, outf = *out_n_frames;
    struct speex *speex_data;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    speex_data = r->implementation.data;

    in = pa_memblock_acquire_chunk(input);
    out = pa_memblock_acquire_chunk(output);

    pa_assert_se(speex_resampler_process_interleaved_float(speex_data->state, in, &inf, out, &outf) == 0);

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    pa_assert(inf == in_n_frames);
    *out_n_frames = outf;
}

static void speex_resample_int(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    int16_t *in, *out;
    uint32_t inf = in_n_frames, outf = *out_n_frames;
    struct speex *speex_data;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    speex_data = r->implementation.data;

    in = pa_memblock_acquire_chunk(input);
    out = pa_memblock_acquire_chunk(output);

    pa_assert_se(speex_resampler_process_interleaved_int(speex_data->state, in, &inf, out, &outf) == 0);

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    pa_assert(inf == in_n_frames);
    *out_n_frames = outf;
}

static void speex_update_rates(pa_resampler *r) {
    struct speex *speex_data;
    pa_assert(r);

    speex_data = r->implementation.data;

    pa_assert_se(speex_resampler_set_rate(speex_data->state, r->i_ss.rate, r->o_ss.rate) == 0);
}

static void speex_reset(pa_resampler *r) {
    struct speex *speex_data;
    pa_assert(r);

    speex_data = r->implementation.data;

    pa_assert_se(speex_resampler_reset_mem(speex_data->state) == 0);
}

static void speex_free(pa_resampler *r) {
    struct speex *speex_data;
    pa_assert(r);

    speex_data = r->implementation.data;
    if (!speex_data->state)
        return;

    speex_resampler_destroy(speex_data->state);
}

static int speex_init(pa_resampler *r) {
    int q, err;
    struct speex *speex_data;

    pa_assert(r);

    speex_data = pa_xnew(struct speex, 1);
    r->implementation.data = speex_data;

    if (r->method >= PA_RESAMPLER_SPEEX_FIXED_BASE && r->method <= PA_RESAMPLER_SPEEX_FIXED_MAX) {

        q = r->method - PA_RESAMPLER_SPEEX_FIXED_BASE;
        r->implementation.resample = speex_resample_int;

    } else {
        pa_assert(r->method >= PA_RESAMPLER_SPEEX_FLOAT_BASE && r->method <= PA_RESAMPLER_SPEEX_FLOAT_MAX);

        q = r->method - PA_RESAMPLER_SPEEX_FLOAT_BASE;
        r->implementation.resample = speex_resample_float;
    }

    pa_log_info("Choosing speex quality setting %i.", q);

    if (!(speex_data->state = speex_resampler_init(r->work_channels, r->i_ss.rate, r->o_ss.rate, q, &err)))
        return -1;

    return 0;
}
#endif

/* Trivial implementation */

static void trivial_resample(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    size_t fz;
    unsigned i_index, o_index;
    void *src, *dst;
    struct trivial *trivial_data;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    trivial_data = r->implementation.data;
    fz = r->w_sz * r->work_channels;

    src = pa_memblock_acquire_chunk(input);
    dst = pa_memblock_acquire_chunk(output);

    for (o_index = 0;; o_index++, trivial_data->o_counter++) {
        i_index = ((uint64_t) trivial_data->o_counter * r->i_ss.rate) / r->o_ss.rate;
        i_index = i_index > trivial_data->i_counter ? i_index - trivial_data->i_counter : 0;

        if (i_index >= in_n_frames)
            break;

        pa_assert_fp(o_index * fz < pa_memblock_get_length(output->memblock));

        memcpy((uint8_t*) dst + fz * o_index, (uint8_t*) src + fz * i_index, (int) fz);
    }

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    *out_n_frames = o_index;

    trivial_data->i_counter += in_n_frames;

    /* Normalize counters */
    while (trivial_data->i_counter >= r->i_ss.rate) {
        pa_assert(trivial_data->o_counter >= r->o_ss.rate);

        trivial_data->i_counter -= r->i_ss.rate;
        trivial_data->o_counter -= r->o_ss.rate;
    }
}

static void trivial_update_rates_or_reset(pa_resampler *r) {
    struct trivial *trivial_data;
    pa_assert(r);

    trivial_data = r->implementation.data;

    trivial_data->i_counter = 0;
    trivial_data->o_counter = 0;
}

static int trivial_init(pa_resampler*r) {
    struct trivial *trivial_data;
    pa_assert(r);

    trivial_data = pa_xnew0(struct trivial, 1);

    r->implementation.data = trivial_data;

    return 0;
}

/* Peak finder implementation */

static void peaks_resample(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    unsigned c, o_index = 0;
    unsigned i, i_end = 0;
    void *src, *dst;
    struct peaks *peaks_data;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    peaks_data = r->implementation.data;
    src = pa_memblock_acquire_chunk(input);
    dst = pa_memblock_acquire_chunk(output);

    i = ((uint64_t) peaks_data->o_counter * r->i_ss.rate) / r->o_ss.rate;
    i = i > peaks_data->i_counter ? i - peaks_data->i_counter : 0;

    while (i_end < in_n_frames) {
        i_end = ((uint64_t) (peaks_data->o_counter + 1) * r->i_ss.rate) / r->o_ss.rate;
        i_end = i_end > peaks_data->i_counter ? i_end - peaks_data->i_counter : 0;

        pa_assert_fp(o_index * r->w_sz * r->o_ss.channels < pa_memblock_get_length(output->memblock));

        /* 1ch float is treated separately, because that is the common case */
        if (r->o_ss.channels == 1 && r->work_format == PA_SAMPLE_FLOAT32NE) {
            float *s = (float*) src + i;
            float *d = (float*) dst + o_index;

            for (; i < i_end && i < in_n_frames; i++) {
                float n = fabsf(*s++);

                if (n > peaks_data->max_f[0])
                    peaks_data->max_f[0] = n;
            }

            if (i == i_end) {
                *d = peaks_data->max_f[0];
                peaks_data->max_f[0] = 0;
                o_index++, peaks_data->o_counter++;
            }
        } else if (r->work_format == PA_SAMPLE_S16NE) {
            int16_t *s = (int16_t*) src + r->i_ss.channels * i;
            int16_t *d = (int16_t*) dst + r->o_ss.channels * o_index;

            for (; i < i_end && i < in_n_frames; i++)
                for (c = 0; c < r->o_ss.channels; c++) {
                    int16_t n = abs(*s++);

                    if (n > peaks_data->max_i[c])
                        peaks_data->max_i[c] = n;
                }

            if (i == i_end) {
                for (c = 0; c < r->o_ss.channels; c++, d++) {
                    *d = peaks_data->max_i[c];
                    peaks_data->max_i[c] = 0;
                }
                o_index++, peaks_data->o_counter++;
            }
        } else {
            float *s = (float*) src + r->i_ss.channels * i;
            float *d = (float*) dst + r->o_ss.channels * o_index;

            for (; i < i_end && i < in_n_frames; i++)
                for (c = 0; c < r->o_ss.channels; c++) {
                    float n = fabsf(*s++);

                    if (n > peaks_data->max_f[c])
                        peaks_data->max_f[c] = n;
                }

            if (i == i_end) {
                for (c = 0; c < r->o_ss.channels; c++, d++) {
                    *d = peaks_data->max_f[c];
                    peaks_data->max_f[c] = 0;
                }
                o_index++, peaks_data->o_counter++;
            }
        }
    }

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    *out_n_frames = o_index;

    peaks_data->i_counter += in_n_frames;

    /* Normalize counters */
    while (peaks_data->i_counter >= r->i_ss.rate) {
        pa_assert(peaks_data->o_counter >= r->o_ss.rate);

        peaks_data->i_counter -= r->i_ss.rate;
        peaks_data->o_counter -= r->o_ss.rate;
    }
}

static void peaks_update_rates_or_reset(pa_resampler *r) {
    struct peaks *peaks_data;
    pa_assert(r);

    peaks_data = r->implementation.data;

    peaks_data->i_counter = 0;
    peaks_data->o_counter = 0;
}

static int peaks_init(pa_resampler*r) {
    struct peaks *peaks_data;
    pa_assert(r);
    pa_assert(r->i_ss.rate >= r->o_ss.rate);
    pa_assert(r->work_format == PA_SAMPLE_S16NE || r->work_format == PA_SAMPLE_FLOAT32NE);

    peaks_data = pa_xnew(struct peaks, 1);
    r->implementation.data = peaks_data;

    peaks_data->o_counter = peaks_data->i_counter = 0;
    memset(peaks_data->max_i, 0, sizeof(peaks_data->max_i));
    memset(peaks_data->max_f, 0, sizeof(peaks_data->max_f));

    return 0;
}

/*** copy (noop) implementation ***/

static int copy_init(pa_resampler *r) {
    pa_assert(r);

    pa_assert(r->o_ss.rate == r->i_ss.rate);

    return 0;
}
