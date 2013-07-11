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

void speex_update_rates(pa_resampler *r) {
    SpeexResamplerState *state;
    pa_assert(r);

    state = r->implementation.data;

    pa_assert_se(speex_resampler_set_rate(state, r->i_ss.rate, r->o_ss.rate) == 0);
}

void speex_reset(pa_resampler *r) {
    SpeexResamplerState *state;
    pa_assert(r);

    state = r->implementation.data;

    pa_assert_se(speex_resampler_reset_mem(state) == 0);
}

void speex_free(pa_resampler *r) {
    SpeexResamplerState *state;
    pa_assert(r);

    state = r->implementation.data;
    if (!state)
        return;

    speex_resampler_destroy(state);
}

int speex_init(pa_resampler *r) {
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

    if (!(state = speex_resampler_init(r->work_channels, r->i_ss.rate, r->o_ss.rate, q, &err)))
        return -1;

    r->implementation.data = state;

    return 0;
}
