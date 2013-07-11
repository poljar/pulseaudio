#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "../resampler.h"
#include <pulse/xmalloc.h>
#include <math.h>

void peaks_resample(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
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

void peaks_update_rates_or_reset(pa_resampler *r) {
    struct peaks *peaks_data;
    pa_assert(r);

    peaks_data = r->implementation.data;

    peaks_data->i_counter = 0;
    peaks_data->o_counter = 0;
}

int peaks_init(pa_resampler*r) {
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
