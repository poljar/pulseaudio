/***
  This file is part of PulseAudio.

  Copyright 2013 Peter Meerwald <pmeerw@pmeerw.net>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <check.h>
#include <stdio.h>
#include <math.h>

#include <pulse/pulseaudio.h>

#include <pulse/rtclock.h>
#include <pulse/sample.h>

#include <pulsecore/log.h>
#include <pulsecore/resampler.h>
#include <pulsecore/macro.h>
#include <pulsecore/memblock.h>
#include <pulsecore/core-util.h>

#define PA_CPU_TEST_RUN_START(l, t1, t2)                        \
{                                                               \
    int _j, _k;                                                 \
    int _times = (t1), _times2 = (t2);                          \
    pa_usec_t _start, _stop;                                    \
    pa_usec_t _min = INT_MAX, _max = 0;                         \
    double _s1 = 0, _s2 = 0;                                    \
    const char *_label = (l);                                   \
                                                                \
    for (_k = 0; _k < _times2; _k++) {                          \
        _start = pa_rtclock_now();                              \
        for (_j = 0; _j < _times; _j++)

#define PA_CPU_TEST_RUN_STOP                                    \
        _stop = pa_rtclock_now();                               \
                                                                \
        if (_min > (_stop - _start)) _min = _stop - _start;     \
        if (_max < (_stop - _start)) _max = _stop - _start;     \
        _s1 += _stop - _start;                                  \
        _s2 += (_stop - _start) * (_stop - _start);             \
    }                                                           \
    pa_log_debug("%s: %llu usec (avg: %g, min = %llu, max = %llu, stddev = %g).", _label, \
            (long long unsigned int)_s1,                        \
            ((double)_s1 / _times2),                            \
            (long long unsigned int)_min,                       \
            (long long unsigned int)_max,                       \
            sqrt(_times2 * _s2 - _s1 * _s1) / _times2);         \
}

#define TIMES 300
#define TIMES2 100
#define BLOCKSIZE_MSEC 10
static pa_mempool *pool = NULL;

static pa_resampler* create_resampler(pa_resample_method_t method, unsigned fromrate, unsigned torate, pa_sample_format_t format) {
    pa_resampler *r;
    pa_sample_spec a, b;

    a.channels = b.channels = 1;
    a.rate = fromrate;
    b.rate = torate;
    a.format = b.format = format;

    pa_assert_se(r = pa_resampler_new(pool, &a, NULL, &b, NULL, method, 0));

    return r;
}

static pa_memchunk create_memchunk(unsigned rate, pa_sample_format_t format) {
    pa_sample_spec a;
    pa_memchunk i;
    void *d;

    a.channels = 1;
    a.rate = rate;
    a.format = format;

    i.memblock = pa_memblock_new(pool, pa_usec_to_bytes(BLOCKSIZE_MSEC*PA_USEC_PER_MSEC, &a));
    i.length = pa_memblock_get_length(i.memblock);
    i.index = 0;

    d = pa_memblock_acquire(i.memblock);
    if (format == PA_SAMPLE_S16NE) {
        int16_t *u = d;
        memset(u, 0, i.length);
    }
    else if (format == PA_SAMPLE_FLOAT32NE) {
        float *u = d;
        memset(u, 0, i.length);
    } else
        pa_assert_not_reached();

    pa_memblock_release(i.memblock);

    return i;
}

typedef struct {
    pa_resample_method_t method;
    const char *name;
    enum {SINT16, FLOAT, BOTH} format;
} test_resamplers_t;

static test_resamplers_t test_resamplers[] = {
    {PA_RESAMPLER_SPEEX_FIXED_BASE, "speex-fixed", SINT16},
    {PA_RESAMPLER_SPEEX_FLOAT_BASE, "speex-float", FLOAT},
    {PA_RESAMPLER_TRIVIAL, "trivial", BOTH},
    {PA_RESAMPLER_SRC_SINC_FASTEST, "src-sinc-fastest", FLOAT},
    {PA_RESAMPLER_SRC_ZERO_ORDER_HOLD, "src-zoh", FLOAT},
    {PA_RESAMPLER_SRC_LINEAR, "src-linear", FLOAT},
    {PA_RESAMPLER_LSWR, "lswr", BOTH},
//    {PA_RESAMPLER_FFMPEG, "ffmpeg", SINT16},
//    {PA_RESAMPLER_SINC, "sinc", FLOAT},
//    {PA_RESAMPLER_ZITA, "zita", FLOAT},
    {PA_RESAMPLER_MAX, NULL, 0}
};

static void run(unsigned fmt, unsigned fromrate, unsigned torate) {
    test_resamplers_t *t = test_resamplers;
    pa_resampler *resampler;
    pa_memchunk i, j;
    pa_resample_method_t got_method;
    pa_sample_format_t got_format;
    pa_sample_format_t format;

    pa_assert(fmt == SINT16 || fmt == FLOAT);
    format = (fmt == FLOAT) ? PA_SAMPLE_FLOAT32NE : PA_SAMPLE_S16NE;

    pa_log_debug("Checking %s resampling (%d -> %d)",
        pa_sample_format_to_string(format), fromrate, torate);

    i = create_memchunk(fromrate, format);

    for ( ; t->method < PA_RESAMPLER_MAX; t++) {
        if (t->format != fmt && t->format != BOTH)
            continue;

        pa_log_set_level(PA_LOG_ERROR);
        resampler = create_resampler(t->method, fromrate, torate, format);
        pa_log_set_level(PA_LOG_DEBUG);

        got_method = pa_resampler_get_method(resampler);
        if (got_method != t->method) {
            pa_log_info("Requested %s, but got %s, skipping test",
                pa_resample_method_to_string(t->method), pa_resample_method_to_string(got_method));
            continue;
        }

        got_format = pa_resampler_get_work_format(resampler);
        if (got_format != format) {
            pa_log_info("Requested %s, but got %s, skipping test",
                pa_sample_format_to_string(format), pa_sample_format_to_string(got_format));
            continue;
        }

        PA_CPU_TEST_RUN_START(t->name, TIMES, TIMES2) {
            pa_resampler_run(resampler, &i, &j);
            pa_memblock_unref(j.memblock);
        } PA_CPU_TEST_RUN_STOP
        pa_resampler_free(resampler);
    }

    pa_memblock_unref(i.memblock);
}

START_TEST (s16_test) {
    run(SINT16, 44100, 48000);
    run(SINT16, 48000, 16000);
    run(SINT16, 16000, 32000);
    run(SINT16, 32000, 16000);
    run(SINT16, 16000, 48000);
    run(SINT16, 48000, 16000);
}
END_TEST

START_TEST (float32_test) {
    run(FLOAT, 44100, 48000);
    run(FLOAT, 48000, 16000);
    run(FLOAT, 16000, 32000);
    run(FLOAT, 32000, 16000);
    run(FLOAT, 16000, 48000);
    run(FLOAT, 48000, 16000);
}
END_TEST

int main(int argc, char *argv[]) {
    int failed = 0;
    Suite *s;
    TCase *tc;
    SRunner *sr;

    pa_log_set_level(PA_LOG_ERROR);
    if (!getenv("MAKE_CHECK"))
        pa_log_set_level(PA_LOG_DEBUG);

    pa_assert_se(pool = pa_mempool_new(false, 0));

    s = suite_create("resampler");

    tc = tcase_create("s16");
    tcase_add_test(tc, s16_test);
    tcase_set_timeout(tc, 120);
    suite_add_tcase(s, tc);

    tc = tcase_create("float32");
    tcase_add_test(tc, float32_test);
    tcase_set_timeout(tc, 120);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    pa_mempool_free(pool);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
