/***
  This file is part of PulseAudio.

  Copyright 2013 Damir Jelić

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

#include "pulsecore/resampler.h"

#include <libswresample/swresample.h>
#include <libavutil/opt.h>

static void lswr_resample(pa_resampler *r, const pa_memchunk *input,
                          unsigned in_n_frames, pa_memchunk *output,
                          unsigned *out_n_frames) {
    SwrContext *state;
    uint8_t *out;
    const uint8_t *in;
    unsigned out_samples;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    state = r->impl.data;

    out_samples = *out_n_frames;
    in = pa_memblock_acquire_chunk(input);
    out = pa_memblock_acquire_chunk(output);

    out_samples = swr_convert(state, &out, out_samples, &in, in_n_frames);

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    *out_n_frames = out_samples;
}

static void lswr_update_rates(pa_resampler *r) {
    SwrContext *state;
    pa_assert(r);

    state = r->impl.data;

    av_opt_set_int(state, "in_sample_rate", r->i_ss.rate, 0);
    av_opt_set_int(state, "out_sample_rate", r->o_ss.rate, 0);

    swr_init(state);
}

static void lswr_reset(pa_resampler *r) {
    SwrContext *state;
    pa_assert(r);

    state = r->impl.data;
    swr_convert(state, NULL, 0, NULL, 0);
}

static void lswr_free(pa_resampler *r) {
    SwrContext *state;
    pa_assert(r);

    state = r->impl.data;
    swr_free(&state);
}

int pa_resampler_lswr_init(pa_resampler *r) {
    SwrContext *state;
    int format;

    pa_assert(r);

    if (!(state = swr_alloc()))
        return -1;

    switch (r->work_format) {
        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_S16LE:
            format = AV_SAMPLE_FMT_S16;
            break;

        case PA_SAMPLE_FLOAT32BE:
        case PA_SAMPLE_FLOAT32LE:
            format = AV_SAMPLE_FMT_FLT;
            break;

        default:
            pa_assert_not_reached();
    }

    av_opt_set_int(state, "in_channel_count", r->o_ss.channels, 0);
    av_opt_set_int(state, "out_channel_count", r->o_ss.channels, 0);
    av_opt_set_int(state, "in_sample_rate", r->i_ss.rate, 0);
    av_opt_set_int(state, "out_sample_rate", r->o_ss.rate, 0);
    av_opt_set_sample_fmt(state, "in_sample_fmt", format, 0);
    av_opt_set_sample_fmt(state, "out_sample_fmt", format, 0);

    if (swr_init(state) < 0)
        return -1;

    r->impl.free = lswr_free;
    r->impl.reset = lswr_reset;
    r->impl.update_rates = lswr_update_rates;
    r->impl.resample = lswr_resample;
    r->impl.data = state;

    return 0;
}
