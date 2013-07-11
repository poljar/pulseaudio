#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "../resampler.h"
#include <pulse/xmalloc.h>

void trivial_resample(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
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

void trivial_update_rates_or_reset(pa_resampler *r) {
    struct trivial *trivial_data;
    pa_assert(r);

    trivial_data = r->implementation.data;

    trivial_data->i_counter = 0;
    trivial_data->o_counter = 0;
}

int trivial_init(pa_resampler*r) {
    struct trivial *trivial_data;
    pa_assert(r);

    trivial_data = pa_xnew0(struct trivial, 1);

    r->implementation.data = trivial_data;

    return 0;
}
