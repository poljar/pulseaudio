/***
  This file is part of PulseAudio.

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

#include <stdio.h>
#include <getopt.h>
#include <locale.h>
#include <math.h>
#include <fcntl.h>
#include <errno.h>

#include <pulse/pulseaudio.h>

#include <pulse/rtclock.h>
#include <pulse/sample.h>
#include <pulse/volume.h>

#include <pulsecore/i18n.h>
#include <pulsecore/log.h>
#include <pulsecore/resampler.h>
#include <pulsecore/macro.h>
#include <pulsecore/endianmacros.h>
#include <pulsecore/memblock.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/sconv-s16le.h>
#include <pulsecore/sconv-s16be.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sndfile-util.h>

static void help(const char *argv0) {
    printf(_("%s [options]\n\n"
             "-h, --help                            Show this help\n"
             "-v, --verbose                         Print debug messages\n"
             "      --from-rate=SAMPLERATE          From sample rate in Hz (defaults to 96000)\n"
             "      --format=SAMPLEFORMAT           From sample type (defaults to float32ne)\n"
             "      --to-rate=SAMPLERATE            To sample rate in Hz (defaults to 44100)\n"
             "      --resample-method=METHOD        Resample method (defaults to auto)\n"
             "      --signal-length=SECONDS         Length of the generated signal (defaults to 8)\n"
             "      --signal-type=SIGNALTYPE        Type of the generated signal (defaults to 440Hz sine)\n"
             "      --base-frequency=BASEFREQUENCY  Start frequency of the chirp or frequency of the sine (defaults to 440Hz)\n"
             "      --stop-frequency=STOPFREQUENCY  Stop frequency of the chirp signal (defaults to 48000Hz)\n"
             "\n"
             "Sample type must be one of s16le, s16be, float32ne (default float32ne)\n"
             "\n"
             "Signal type must be one of sine, linear-chirp, log-chirp (default sine)\n"
             "\n"
             "See --dump-resample-methods for possible values of resample methods.\n"),
             argv0);
}

enum {
    ARG_VERSION = 256,
    ARG_FROM_SAMPLERATE,
    ARG_FORMAT,
    ARG_TO_SAMPLERATE,
    ARG_SIGNAL_LENGTH,
    ARG_SIGNAL_TYPE,
    ARG_BASE_FREQ,
    ARG_STOP_FREQ,
    ARG_RESAMPLE_METHOD,
    ARG_DUMP_RESAMPLE_METHODS
};

typedef enum signal_type {
    SIGNAL_INVALID = -1,
    SIGNAL_SINE,
    SIGNAL_LIN_CHIRP,
    SIGNAL_LOG_CHIRP,
} signal_type_t;

static void dump_resample_methods(void) {
    int i;

    for (i = 0; i < PA_RESAMPLER_MAX; i++)
        if (pa_resample_method_supported(i))
            printf("%s\n", pa_resample_method_to_string(i));

}

static void chirp_chunk(pa_memchunk *chunk, pa_mempool *pool, pa_sample_spec *sample_spec,
                        unsigned int freq0, unsigned int freq1, unsigned int seconds, signal_type_t type) {
    float chirp_rate;
    unsigned int frame_size;

    union memblock_u {
        float *f32_memblock;
        int16_t *s16_memblock;
    } memblock;

    if (type == SIGNAL_LOG_CHIRP)
        pa_assert(freq0 * freq1 > 0);

    pa_memchunk_reset(chunk);

    chunk->memblock = pa_memblock_new(pool, pa_usec_to_bytes(seconds * PA_USEC_PER_SEC, sample_spec));
    chunk->length = pa_memblock_get_length(chunk->memblock);

    chirp_rate = (freq1 - freq0) / seconds;
    frame_size = pa_frame_size(sample_spec);

    if (sample_spec->format == PA_SAMPLE_FLOAT32)
        memblock.f32_memblock = pa_memblock_acquire(chunk->memblock);
    else if (sample_spec->format == PA_SAMPLE_S16NE)
        memblock.s16_memblock = pa_memblock_acquire(chunk->memblock);
    else
        pa_assert_not_reached();

    for (uint64_t k = 0; k < chunk->length / frame_size; k++) {
        float sample;
        double t = (double) k / (double) sample_spec->rate;

        if (type == SIGNAL_LIN_CHIRP)
            sample = (float) 0.5f * sin((double) 2 * M_PI * (freq0 * t + 0.5 * chirp_rate * t * t));
        else {
            if (freq0 == freq1)
                sample = (float) 0.5f * sin((double) 2 * M_PI * freq0 * t);
            else {
                chirp_rate = seconds / log((double) freq1 / (double) freq0);
                sample = (float) 0.5f * sin((double) 2 * M_PI * chirp_rate * freq0 *
                                            (pow((double) freq1 / (double) freq0, t / (double) seconds) - (double) 1.0f));
            }
        }

        switch (sample_spec->format) {
            case PA_SAMPLE_FLOAT32NE:
                *(memblock.f32_memblock++) = sample;
                break;

            case PA_SAMPLE_S16LE: {
                int16_t s16_sample;
                pa_sconv_s16le_from_float32ne(1, &sample, &s16_sample);
                *(memblock.s16_memblock++) = s16_sample;
                break;
                                  }

            case PA_SAMPLE_S16BE: {
                int16_t s16_sample;
                pa_sconv_s16be_from_float32ne(1, &sample, &s16_sample);
                *(memblock.s16_memblock++) = s16_sample;
                break;
                                  }

            default:
                pa_assert_not_reached();
        }
    }

    pa_memblock_release(chunk->memblock);
}

static void sine_chunk(pa_memchunk *chunk, pa_mempool *pool, pa_sample_spec *sample_spec, unsigned int freq, unsigned int seconds) {
    chirp_chunk(chunk, pool, sample_spec, freq, freq, seconds, SIGNAL_LIN_CHIRP);
}

static signal_type_t parse_signal_type(const char* type_string) {
    if (pa_streq(type_string, "sine"))
        return SIGNAL_SINE;
    else if (pa_streq(type_string, "linear-chirp"))
        return SIGNAL_LIN_CHIRP;
    else if (pa_streq(type_string, "log-chirp"))
        return SIGNAL_LOG_CHIRP;
    else
        return SIGNAL_INVALID;
}

static const char *signal_names[] = {
    "sine wave",
    "linear chirp",
    "logarithmic chirp",
};

static const char *signal_to_string(signal_type_t type) {
    return signal_names[type];
}

static int save_chunk(const char *filename, pa_memchunk *chunk, pa_sample_spec *sample_spec) {
    SF_INFO sfi;
    SNDFILE *sndfile = NULL;
    size_t frame_size;
    pa_channel_map channel_map;

    union memblock_u {
        float *f32_memblock;
        int16_t *s16_memblock;
    } memblock;

    static sf_count_t (*writef_function)(SNDFILE *_sndfile, const void *ptr,
                                         sf_count_t frames) = NULL;

    pa_assert(filename);
    pa_assert(chunk);
    pa_assert(sample_spec);

    pa_zero(sfi);

    if (pa_sndfile_write_sample_spec(&sfi, sample_spec) < 0) {
        pa_log(_("Failed to generate sample specification for file."));
        return -1;
    }

    sfi.format = SF_FORMAT_WAV;
    sfi.samplerate = sample_spec->rate;
    sfi.channels = sample_spec->channels;
    frame_size = pa_frame_size(sample_spec);

    switch (sample_spec->format) {
        case PA_SAMPLE_FLOAT32NE:
            sfi.format |= SF_FORMAT_FLOAT;
            memblock.f32_memblock = pa_memblock_acquire(chunk->memblock);
            break;
        case PA_SAMPLE_S16LE:                   /* fall trough */
        case PA_SAMPLE_S16BE:
            sfi.format |= SF_FORMAT_PCM_16;
            memblock.s16_memblock = pa_memblock_acquire(chunk->memblock);
            break;
        default:
            pa_assert_not_reached();
    }

    if (!(sndfile = sf_open(filename, SFM_WRITE, &sfi))) {
        pa_log(_("Failed to open audio file."));
        return -1;
    }

    pa_channel_map_init_extend(&channel_map, sample_spec->channels, PA_CHANNEL_MAP_DEFAULT);

    if (pa_sndfile_write_channel_map(sndfile, &channel_map) < 0)
        pa_log(_("Warning: failed to write channel map to file."));

    writef_function = pa_sndfile_writef_function(sample_spec);

    if (sample_spec->format == PA_SAMPLE_FLOAT32) {
        writef_function(sndfile, memblock.f32_memblock, (sf_count_t) chunk->length / frame_size);
    } else if (sample_spec->format == PA_SAMPLE_S16NE) {
        writef_function(sndfile, memblock.s16_memblock, (sf_count_t) chunk->length / frame_size);
    }
    else
        pa_assert_not_reached();

    pa_memblock_release(chunk->memblock);

    sf_close(sndfile);

    return 0;
}

int main(int argc, char *argv[]) {
    int ret = 1, c;
    pa_mempool *pool = NULL;
    pa_sample_spec a, b;
    pa_resampler *resampler;
    pa_memchunk input_chunk;
    pa_resample_method_t method;

    int signal_type = SIGNAL_SINE;
    int signal_length = 8;
    uint32_t freq0 = 440;
    uint32_t freq1 = 48000;

    static const struct option long_options[] = {
        {"help",                  0, NULL, 'h'},
        {"verbose",               0, NULL, 'v'},
        {"version",               0, NULL, ARG_VERSION},
        {"from-rate",             1, NULL, ARG_FROM_SAMPLERATE},
        {"format",                1, NULL, ARG_FORMAT},
        {"to-rate",               1, NULL, ARG_TO_SAMPLERATE},
        {"signal-length",         1, NULL, ARG_SIGNAL_LENGTH},
        {"resample-method",       1, NULL, ARG_RESAMPLE_METHOD},
        {"signal-type",           1, NULL, ARG_SIGNAL_TYPE},
        {"dump-resample-methods", 0, NULL, ARG_DUMP_RESAMPLE_METHODS},
        {NULL,                    0, NULL, 0}
    };

    setlocale(LC_ALL, "");
#ifdef ENABLE_NLS
    bindtextdomain(GETTEXT_PACKAGE, PULSE_LOCALEDIR);
#endif

    pa_log_set_level(PA_LOG_WARN);
    if (!getenv("MAKE_CHECK"))
        pa_log_set_level(PA_LOG_INFO);

    pa_assert_se(pool = pa_mempool_new(false, 0));

    a.channels = b.channels = 1;
    a.rate = 96000;
    b.rate = 44100;
    a.format = b.format = PA_SAMPLE_FLOAT32;

    method = PA_RESAMPLER_AUTO;

    while ((c = getopt_long(argc, argv, "hv", long_options, NULL)) != -1) {

        switch (c) {
            case 'h' :
                help(argv[0]);
                ret = 0;
                goto quit;

            case 'v':
                pa_log_set_level(PA_LOG_DEBUG);
                break;

            case ARG_VERSION:
                printf(_("%s %s\n"), argv[0], PACKAGE_VERSION);
                ret = 0;
                goto quit;

            case ARG_DUMP_RESAMPLE_METHODS:
                dump_resample_methods();
                ret = 0;
                goto quit;

            case ARG_FROM_SAMPLERATE:
                a.rate = (uint32_t) atoi(optarg);
                break;

            case ARG_TO_SAMPLERATE:
                b.rate = (uint32_t) atoi(optarg);
                break;

            case ARG_FORMAT:
                a.format = pa_parse_sample_format(optarg);
                break;

            case ARG_BASE_FREQ:
                freq0 = (uint32_t) atoi(optarg);
                break;

            case ARG_SIGNAL_LENGTH:
                signal_length = atoi(optarg);
                break;

            case ARG_SIGNAL_TYPE:
                if ((signal_type = parse_signal_type(optarg)) < 0) {
                    ret = -1;
                    pa_log_info("Invalid signal type specified");
                    help(argv[0]);
                    goto quit;
                }
                break;

            case ARG_RESAMPLE_METHOD:
                if (*optarg == '\0' || pa_streq(optarg, "help")) {
                    dump_resample_methods();
                    ret = 0;
                    goto quit;
                }
                method = pa_parse_resample_method(optarg);
                break;

            default:
                goto quit;
        }
    }

    pa_log_info("Generating %s with start freq %dHz, stop freq %dHz and length %ds", signal_to_string(signal_type), freq0, freq1, signal_length);

    ret = 0;
    pa_assert_se(pool = pa_mempool_new(false, 0));

    pa_assert_se(resampler = pa_resampler_new(pool, &a, NULL, &b, NULL, method, 0));

    if (signal_type == SIGNAL_SINE)
        sine_chunk(&input_chunk, pool, &a, freq0, signal_length);
    else
        chirp_chunk(&input_chunk, pool, &a, freq0, freq1, signal_length, signal_type);

    /* TODO: run resampler here and save the resampled chunk */
    save_chunk("test.wav", &input_chunk, &a);

    pa_memblock_unref(input_chunk.memblock);

    pa_resampler_free(resampler);

 quit:
    if (pool)
        pa_mempool_free(pool);

    return ret;
}
