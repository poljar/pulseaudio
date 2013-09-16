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

#include <soxr.h>

#include "pulsecore/resampler.h"

static void soxr_free(pa_resampler *r);

static void soxr_resample(pa_resampler *r, const pa_memchunk *input,
                          unsigned in_n_frames, pa_memchunk *output,
                          unsigned *out_n_frames) {
    soxr_t state;
    soxr_error_t error;
    uint8_t *out;
    uint8_t *in;
    size_t odone;
    size_t consumed;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    state = r->impl.data;

    in = pa_memblock_acquire_chunk(input);
    out = pa_memblock_acquire_chunk(output);

    error = soxr_process(state, in, in_n_frames, &consumed, out, *out_n_frames, &odone);
    pa_assert(error == 0);

    if (consumed < in_n_frames) {
        void *leftover_data = in + consumed * r->o_ss.channels;
        size_t leftover_length = (in_n_frames - consumed) * sizeof(float) * r->o_ss.channels;

        pa_resampler_save_leftover(r, leftover_data, leftover_length);
    }

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    *out_n_frames = odone;
}

static void soxr_udpate_rates(pa_resampler *r) {
    pa_assert(r);

    soxr_free(r);
    pa_resampler_soxr_init(r);
}

static void soxr_reset(pa_resampler *r) {
    pa_assert(r);

    /* TODO use soxr_clear()
     * for some reason soxr_process crashes after we use soxr_clear() here */
    soxr_free(r);
    pa_resampler_soxr_init(r);
}

static void soxr_free(pa_resampler *r) {
    soxr_t state;
    pa_assert(r);

    state = r->impl.data;
    soxr_delete(state);
}

int pa_resampler_soxr_init(pa_resampler *r) {
    soxr_t state;
    soxr_error_t error;
    soxr_io_spec_t io_spec;
    soxr_runtime_spec_t runtime_spec;
    soxr_quality_spec_t quality_spec;
    unsigned int format;

    pa_assert(r);

    /* sample formats supported by soxr -> 0:float32 1:float64 2:int32 3:int16 */
    switch (r->work_format) {
        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_S16LE:
            format = 3;
            break;
        case PA_SAMPLE_FLOAT32BE:
        case PA_SAMPLE_FLOAT32LE:
            format = 0;
            break;
        default:
            pa_assert_not_reached();
    }

    io_spec = soxr_io_spec(format, format);
    runtime_spec = soxr_runtime_spec(0);
    quality_spec = soxr_quality_spec(SOXR_QQ, 0);

    state = soxr_create(r->i_ss.rate, r->o_ss.rate, r->o_ss.channels, &error, &io_spec, &quality_spec, &runtime_spec);

    if (error)
        return -1;

    r->impl.resample = soxr_resample;
    r->impl.update_rates = soxr_udpate_rates;
    r->impl.reset = soxr_reset;
    r->impl.free = soxr_free;
    r->impl.data = state;

    return 0;
}
