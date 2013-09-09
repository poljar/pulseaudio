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

#include <libswresample/swresample.h>
#include <libavresample/avresample.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>

#include "../resampler.h"

static int lavr_init(pa_resampler *r);
static void lavr_free(pa_resampler *r);
static void lavr_resample(pa_resampler *r, const pa_memchunk *input,
                          unsigned in_n_frames, pa_memchunk *output,
                          unsigned *out_n_frames);
static void lavr_udpate_rates(pa_resampler *r);
static void lavr_reset(pa_resampler *r);

static void lavr_resample(pa_resampler *r, const pa_memchunk *input,
                          unsigned in_n_frames, pa_memchunk *output,
                          unsigned *out_n_frames) {
    AVAudioResampleContext *state;
    uint8_t *out;
    uint8_t *in;
    unsigned out_samples;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    state = r->impl.data;

    out_samples = *out_n_frames;
    in = pa_memblock_acquire_chunk(input);
    out = pa_memblock_acquire_chunk(output);

    out_samples = avresample_convert(state, &out, 0, out_samples, &in, 0, in_n_frames);

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    *out_n_frames = out_samples;
}

static void lavr_udpate_rates(pa_resampler *r) {
    AVAudioResampleContext *state;
    pa_assert(r);

    state = r->impl.data;

    avresample_close(state);

    av_opt_set_int(state, "in_sample_rate", r->i_ss.rate, 0);
    av_opt_set_int(state, "out_sample_rate", r->o_ss.rate, 0);

    avresample_open(state);
}

static void lavr_reset(pa_resampler *r) {
    AVAudioResampleContext *state;
    pa_assert(r);

    state = r->impl.data;
    avresample_close(state);
    avresample_open(state);
}

static void lavr_free(pa_resampler *r) {
    AVAudioResampleContext *state;
    pa_assert(r);

    state = r->impl.data;
    avresample_free(&state);
}

int pa_resampler_lavr_init(pa_resampler *r) {
    AVAudioResampleContext *state;
    int channel_map;
    int format;
    pa_assert(r);

    if (!(state = avresample_alloc_context()))
        return -1;

    pa_log_debug("%d", r->o_ss.channels);
    switch (r->o_ss.channels) {
        case 1:
            channel_map = AV_CH_LAYOUT_MONO;
            break;
        case 2:
            channel_map = AV_CH_LAYOUT_STEREO;
            break;
        case 3:
            channel_map = AV_CH_LAYOUT_2POINT1;
            break;
        case 4:
            channel_map = AV_CH_LAYOUT_4POINT0;
            break;
        case 5:
            channel_map = AV_CH_LAYOUT_5POINT0;
            break;
        case 6:
            channel_map = AV_CH_LAYOUT_5POINT1;
            break;
        case 7:
            channel_map = AV_CH_LAYOUT_6POINT1;
            break;
        case 8:
            channel_map = AV_CH_LAYOUT_7POINT1;
            break;
        default:
            pa_assert_not_reached();
    }

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
    av_opt_set_int(state, "in_channel_layout", channel_map, 0);
    av_opt_set_int(state, "out_channel_layout", channel_map, 0);
    av_opt_set_int(state, "in_sample_rate", r->i_ss.rate, 0);
    av_opt_set_int(state, "out_sample_rate", r->o_ss.rate, 0);
    av_opt_set_int(state, "in_sample_fmt", format, 0);
    av_opt_set_int(state, "out_sample_fmt", format, 0);

    if (avresample_open(state) < 0)
        return -1;

    r->impl.free = lavr_free;
    r->impl.reset = lavr_reset;
    r->impl.update_rates = lavr_udpate_rates;
    r->impl.resample = lavr_resample;
    r->impl.data = state;

    return 0;
}
