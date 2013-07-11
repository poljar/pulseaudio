#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <samplerate.h>
#include "../resampler.h"

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

void libsamplerate_resample(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    SRC_DATA data;
    SRC_STATE *state;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    state = r->implementation.data;
    memset(&data, 0, sizeof(data));

    data.data_in = pa_memblock_acquire_chunk(input);
    data.input_frames = (long int) in_n_frames;

    data.data_out = pa_memblock_acquire_chunk(output);
    data.output_frames = (long int) *out_n_frames;

    data.src_ratio = (double) r->o_ss.rate / r->i_ss.rate;
    data.end_of_input = 0;

    pa_assert_se(src_process(state, &data) == 0);

    if (data.input_frames_used < in_n_frames) {
        void *leftover_data = data.data_in + data.input_frames_used * r->work_channels;
        size_t leftover_length = (in_n_frames - data.input_frames_used) * sizeof(float) * r->work_channels;

        save_leftover(r, leftover_data, leftover_length);
    }

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    *out_n_frames = (unsigned) data.output_frames_gen;
}

void libsamplerate_update_rates(pa_resampler *r) {
    SRC_STATE *state;
    pa_assert(r);

    state = r->implementation.data;
    pa_assert_se(src_set_ratio(state, (double) r->o_ss.rate / r->i_ss.rate) == 0);
}

void libsamplerate_reset(pa_resampler *r) {
    SRC_STATE *state;
    pa_assert(r);

    state = r->implementation.data;
    pa_assert_se(src_reset(state) == 0);
}

void libsamplerate_free(pa_resampler *r) {
    SRC_STATE *state;
    pa_assert(r);

    state = r->implementation.data;
    if (state)
        src_delete(state);
}

int libsamplerate_init(pa_resampler *r) {
    int err;
    SRC_STATE *state = NULL;

    pa_assert(r);


    if (!(state = src_new(r->method, r->o_ss.channels, &err)))
        return -1;

    r->implementation.data = state;

    return 0;
}
