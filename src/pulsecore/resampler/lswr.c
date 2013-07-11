#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include "../resampler.h"

void lswr_resample(pa_resampler *r, const pa_memchunk *input,
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

    state = r->implementation.data;

    out_samples = *out_n_frames;
    in = pa_memblock_acquire_chunk(input);
    out = pa_memblock_acquire_chunk(output);

    out_samples = swr_convert(state, &out, out_samples, &in, in_n_frames);

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    *out_n_frames = out_samples;
}

void lswr_udpate_rates(pa_resampler *r) {
    SwrContext *state;
    pa_assert(r);

    state = r->implementation.data;

    av_opt_set_int(state, "in_sample_rate", r->i_ss.rate, 0);
    av_opt_set_int(state, "out_sample_rate", r->o_ss.rate, 0);

    swr_init(state);
}

void lswr_reset(pa_resampler *r) {
    SwrContext *state;
    pa_assert(r);

    state = r->implementation.data;
    swr_convert(state, NULL, 0, NULL, 0);
}

void lswr_free(pa_resampler *r) {
    SwrContext *state;
    pa_assert(r);

    state = r->implementation.data;
    swr_free(&state);
}

int lswr_init(pa_resampler *r) {
    SwrContext *state;
    pa_assert(r);

    if (!(state = swr_alloc()))
        return -1;

    av_opt_set_int(state, "in_channel_count", r->work_channels, 0);
    av_opt_set_int(state, "out_channel_count", r->work_channels, 0);
    av_opt_set_int(state, "in_sample_rate", r->i_ss.rate, 0);
    av_opt_set_int(state, "out_sample_rate", r->o_ss.rate, 0);
    av_opt_set_sample_fmt(state, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    av_opt_set_sample_fmt(state, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    if (swr_init(state) < 0)
        return -1;

    r->implementation.data = state;

    return 0;
}
