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

#include "../resampler.h"

static void speex_resample_float(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    float *in, *out;
    uint32_t inf = in_n_frames, outf = *out_n_frames;
    SpeexResamplerState *state;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    state = r->impl.data;

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

    state = r->impl.data;

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

    state = r->impl.data;

    pa_assert_se(speex_resampler_set_rate(state, r->i_ss.rate, r->o_ss.rate) == 0);
}

static void speex_reset(pa_resampler *r) {
    SpeexResamplerState *state;
    pa_assert(r);

    state = r->impl.data;

    pa_assert_se(speex_resampler_reset_mem(state) == 0);
}

static void speex_free(pa_resampler *r) {
    SpeexResamplerState *state;
    pa_assert(r);

    state = r->impl.data;
    if (!state)
        return;

    speex_resampler_destroy(state);
}

int pa_resampler_speex_init(pa_resampler *r) {
    int q, err;
    SpeexResamplerState *state;

    pa_assert(r);

    r->impl.free = speex_free;
    r->impl.update_rates = speex_update_rates;
    r->impl.reset = speex_reset;

    if (r->method >= PA_RESAMPLER_SPEEX_FIXED_BASE && r->method <= PA_RESAMPLER_SPEEX_FIXED_MAX) {

        q = r->method - PA_RESAMPLER_SPEEX_FIXED_BASE;
        r->impl.resample = speex_resample_int;

    } else {
        pa_assert(r->method >= PA_RESAMPLER_SPEEX_FLOAT_BASE && r->method <= PA_RESAMPLER_SPEEX_FLOAT_MAX);

        q = r->method - PA_RESAMPLER_SPEEX_FLOAT_BASE;
        r->impl.resample = speex_resample_float;
    }

    pa_log_info("Choosing speex quality setting %i.", q);

    if (!(state = speex_resampler_init(r->o_ss.channels, r->i_ss.rate, r->o_ss.rate, q, &err)))
        return -1;

    r->impl.data = state;

    return 0;
}
