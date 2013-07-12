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

#include <speex/speex_resampler.h>
#include "resampler.h"

static int speex_init(pa_resampler*r);
static void speex_free(pa_resampler *r);
static void speex_update_rates(pa_resampler *r);
static void speex_reset(pa_resampler *r);

pa_resampler_implementation speex_impl = {
    .init = speex_init,
    .free = speex_free,
    .update_rates = speex_update_rates,
    .reset = speex_reset,
    .names = { "speex-float-0",
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
    }
};

static void speex_resample_float(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    float *in, *out;
    uint32_t inf = in_n_frames, outf = *out_n_frames;
    SpeexResamplerState *state;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    state = r->implementation.data;

    in = pa_memblock_acquire_chunk(input);
    out = pa_memblock_acquire_chunk(output);

    pa_assert_se(speex_resampler_process_interleaved_float(state, in, &inf, out, &outf) == 0);

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    pa_assert(inf == in_n_frames);
    *out_n_frames = outf;
}

static void speex_resample_int(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    int16_t *in, *out;
    uint32_t inf = in_n_frames, outf = *out_n_frames;
    SpeexResamplerState *state;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    state = r->implementation.data;

    in = pa_memblock_acquire_chunk(input);
    out = pa_memblock_acquire_chunk(output);

    pa_assert_se(speex_resampler_process_interleaved_int(state, in, &inf, out, &outf) == 0);

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    pa_assert(inf == in_n_frames);
    *out_n_frames = outf;
}

static void speex_update_rates(pa_resampler *r) {
    SpeexResamplerState *state;
    pa_assert(r);

    state = r->implementation.data;

    pa_assert_se(speex_resampler_set_rate(state, r->i_ss.rate, r->o_ss.rate) == 0);
}

static void speex_reset(pa_resampler *r) {
    SpeexResamplerState *state;
    pa_assert(r);

    state = r->implementation.data;

    pa_assert_se(speex_resampler_reset_mem(state) == 0);
}

static void speex_free(pa_resampler *r) {
    SpeexResamplerState *state;
    pa_assert(r);

    state = r->implementation.data;
    if (!state)
        return;

    speex_resampler_destroy(state);
}

static int speex_init(pa_resampler *r) {
    int q, err;
    SpeexResamplerState *state;

    pa_assert(r);

    if (r->method >= PA_RESAMPLER_SPEEX_FIXED_BASE && r->method <= PA_RESAMPLER_SPEEX_FIXED_MAX) {

        q = r->method - PA_RESAMPLER_SPEEX_FIXED_BASE;
        r->implementation.resample = speex_resample_int;

    } else {
        pa_assert(r->method >= PA_RESAMPLER_SPEEX_FLOAT_BASE && r->method <= PA_RESAMPLER_SPEEX_FLOAT_MAX);

        q = r->method - PA_RESAMPLER_SPEEX_FLOAT_BASE;
        r->implementation.resample = speex_resample_float;
    }

    pa_log_info("Choosing speex quality setting %i.", q);

    if (!(state = speex_resampler_init(r->o_ss.channels, r->i_ss.rate, r->o_ss.rate, q, &err)))
        return -1;

    r->implementation.data = state;

    return 0;
}
