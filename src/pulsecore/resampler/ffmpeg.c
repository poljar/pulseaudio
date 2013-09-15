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

#include <pulse/xmalloc.h>
#include "../ffmpeg/avcodec.h"

#include "../resampler.h"

struct ffmpeg_data { /* data specific to ffmpeg */
    struct AVResampleContext *state;
    pa_memchunk buf[PA_CHANNELS_MAX];
};

static void ffmpeg_resample(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    unsigned used_frames = 0, c;
    int previous_consumed_frames = -1;
    struct ffmpeg_data *ffmpeg_data;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    ffmpeg_data = r->impl.data;

    for (c = 0; c < r->o_ss.channels; c++) {
        unsigned u;
        pa_memblock *b, *w;
        int16_t *p, *t, *k, *q, *s;
        int consumed_frames;

        /* Allocate a new block */
        b = pa_memblock_new(r->mempool, ffmpeg_data->buf[c].length + in_n_frames * sizeof(int16_t));
        p = pa_memblock_acquire(b);

        /* Now copy the input data, splitting up channels */
        t = (int16_t*) pa_memblock_acquire_chunk(input) + c;
        k = p;
        for (u = 0; u < in_n_frames; u++) {
            *k = *t;
            t += r->o_ss.channels;
            k ++;
        }
        pa_memblock_release(input->memblock);

        /* Allocate buffer for the result */
        w = pa_memblock_new(r->mempool, *out_n_frames * sizeof(int16_t));
        q = pa_memblock_acquire(w);

        /* Now, resample */
        used_frames = (unsigned) av_resample(ffmpeg_data->state,
                                             q, p,
                                             &consumed_frames,
                                             (int) in_n_frames, (int) *out_n_frames,
                                             c >= (unsigned) (r->o_ss.channels-1));

        pa_memblock_release(b);
        pa_memblock_unref(b);

        pa_assert(consumed_frames <= (int) in_n_frames);
        pa_assert(previous_consumed_frames == -1 || consumed_frames == previous_consumed_frames);
        previous_consumed_frames = consumed_frames;

        /* And place the results in the output buffer */
        s = (int16_t *) pa_memblock_acquire_chunk(output) + c;
        for (u = 0; u < used_frames; u++) {
            *s = *q;
            q++;
            s += r->o_ss.channels;
        }
        pa_memblock_release(output->memblock);
        pa_memblock_release(w);
        pa_memblock_unref(w);
    }

    if (previous_consumed_frames < (int) in_n_frames) {
        void *leftover_data = (int16_t *) pa_memblock_acquire_chunk(input) + previous_consumed_frames * r->o_ss.channels;
        size_t leftover_length = (in_n_frames - previous_consumed_frames) * r->o_ss.channels * sizeof(int16_t);

        pa_resampler_save_leftover(r, leftover_data, leftover_length);
        pa_memblock_release(input->memblock);
    }

    *out_n_frames = used_frames;
}

static void ffmpeg_free(pa_resampler *r) {
    unsigned c;
    struct ffmpeg_data *ffmpeg_data;

    pa_assert(r);

    ffmpeg_data = r->impl.data;
    if (ffmpeg_data->state)
        av_resample_close(ffmpeg_data->state);

    for (c = 0; c < PA_ELEMENTSOF(ffmpeg_data->buf); c++)
        if (ffmpeg_data->buf[c].memblock)
            pa_memblock_unref(ffmpeg_data->buf[c].memblock);
}

int pa_resampler_ffmpeg_init(pa_resampler *r) {
    unsigned c;
    struct ffmpeg_data *ffmpeg_data;

    pa_assert(r);

    ffmpeg_data = pa_xnew(struct ffmpeg_data, 1);

    /* We could probably implement different quality levels by
     * adjusting the filter parameters here. However, ffmpeg
     * internally only uses these hardcoded values, so let's use them
     * here for now as well until ffmpeg makes this configurable. */

    if (!(ffmpeg_data->state = av_resample_init((int) r->o_ss.rate, (int) r->i_ss.rate, 16, 10, 0, 0.8)))
        return -1;

    r->impl.free = ffmpeg_free;
    r->impl.resample = ffmpeg_resample;
    r->impl.data = (void *) ffmpeg_data;

    for (c = 0; c < PA_ELEMENTSOF(ffmpeg_data->buf); c++)
        pa_memchunk_reset(&ffmpeg_data->buf[c]);

    return 0;
}
