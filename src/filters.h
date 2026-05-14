/*
 * Digital Signal Processing Filters for Heart Rate and SpO2 Detection
 * Ported from Arduino MAX3010x library to Zephyr RTOS
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FILTERS_H
#define FILTERS_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Float version of PI to avoid double promotion warnings */
#define M_PI_F 3.14159265f

/* ========================================================================== */
/* Min/Max/Average Statistics                                                 */
/* ========================================================================== */

struct min_max_avg_stat {
    float min;
    float max;
    float sum;
    int count;
};

static inline void stat_init(struct min_max_avg_stat *s)
{
    s->min = NAN;
    s->max = NAN;
    s->sum = 0.0f;
    s->count = 0;
}

static inline void stat_process(struct min_max_avg_stat *s, float value)
{
    if (isnan(s->min)) {
        s->min = value;
    } else if (value < s->min) {
        s->min = value;
    }
    
    if (isnan(s->max)) {
        s->max = value;
    } else if (value > s->max) {
        s->max = value;
    }
    
    s->sum += value;
    s->count++;
}

static inline void stat_reset(struct min_max_avg_stat *s)
{
    stat_init(s);
}

static inline float stat_minimum(const struct min_max_avg_stat *s)
{
    return s->min;
}

static inline float stat_maximum(const struct min_max_avg_stat *s)
{
    return s->max;
}

static inline float stat_average(const struct min_max_avg_stat *s)
{
    if (s->count == 0) {
        return NAN;
    }
    return s->sum / (float)s->count;
}

/* ========================================================================== */
/* High Pass Filter                                                           */
/* ========================================================================== */

struct high_pass_filter {
    float kx;
    float ka0;
    float ka1;
    float kb1;
    float last_filter_value;
    float last_raw_value;
};

/**
 * @brief Initialize high pass filter with time constant
 * @param f Filter structure
 * @param samples Number of samples until decay to 36.8% (RC time constant equivalent)
 */
static inline void hpf_init_samples(struct high_pass_filter *f, float samples)
{
    f->kx = expf(-1.0f / samples);
    f->ka0 = (1.0f + f->kx) / 2.0f;
    f->ka1 = -f->ka0;
    f->kb1 = f->kx;
    f->last_filter_value = NAN;
    f->last_raw_value = NAN;
}

/**
 * @brief Initialize high pass filter with cutoff frequency
 * @param f Filter structure
 * @param cutoff Cutoff frequency in Hz
 * @param sampling_freq Sampling frequency in Hz
 */
static inline void hpf_init(struct high_pass_filter *f, float cutoff, float sampling_freq)
{
    hpf_init_samples(f, sampling_freq / (cutoff * 2.0f * M_PI_F));
}

/**
 * @brief Process a sample through the high pass filter
 * @param f Filter structure
 * @param value Input sample
 * @return Filtered output
 */
static inline float hpf_process(struct high_pass_filter *f, float value)
{
    if (isnan(f->last_filter_value) || isnan(f->last_raw_value)) {
        f->last_filter_value = 0.0f;
    } else {
        f->last_filter_value = f->ka0 * value 
                             + f->ka1 * f->last_raw_value 
                             + f->kb1 * f->last_filter_value;
    }
    
    f->last_raw_value = value;
    return f->last_filter_value;
}

static inline void hpf_reset(struct high_pass_filter *f)
{
    f->last_raw_value = NAN;
    f->last_filter_value = NAN;
}

/* ========================================================================== */
/* Low Pass Filter                                                            */
/* ========================================================================== */

struct low_pass_filter {
    float kx;
    float ka0;
    float kb1;
    float last_value;
};

/**
 * @brief Initialize low pass filter with time constant
 * @param f Filter structure
 * @param samples Number of samples until decay to 36.8% (RC time constant equivalent)
 */
static inline void lpf_init_samples(struct low_pass_filter *f, float samples)
{
    f->kx = expf(-1.0f / samples);
    f->ka0 = 1.0f - f->kx;
    f->kb1 = f->kx;
    f->last_value = NAN;
}

/**
 * @brief Initialize low pass filter with cutoff frequency
 * @param f Filter structure
 * @param cutoff Cutoff frequency in Hz
 * @param sampling_freq Sampling frequency in Hz
 */
static inline void lpf_init(struct low_pass_filter *f, float cutoff, float sampling_freq)
{
    lpf_init_samples(f, sampling_freq / (cutoff * 2.0f * M_PI_F));
}

/**
 * @brief Process a sample through the low pass filter
 * @param f Filter structure
 * @param value Input sample
 * @return Filtered output
 */
static inline float lpf_process(struct low_pass_filter *f, float value)
{
    if (isnan(f->last_value)) {
        f->last_value = value;
    } else {
        f->last_value = f->ka0 * value + f->kb1 * f->last_value;
    }
    return f->last_value;
}

static inline void lpf_reset(struct low_pass_filter *f)
{
    f->last_value = NAN;
}

/* ========================================================================== */
/* Differentiator                                                             */
/* ========================================================================== */

struct differentiator {
    float sampling_freq;
    float last_value;
};

static inline void diff_init(struct differentiator *d, float sampling_freq)
{
    d->sampling_freq = sampling_freq;
    d->last_value = NAN;
}

/**
 * @brief Process a sample through the differentiator
 * @param d Differentiator structure
 * @param value Input sample
 * @return Differentiated output (derivative * sampling_freq)
 */
static inline float diff_process(struct differentiator *d, float value)
{
    float diff = (value - d->last_value) * d->sampling_freq;
    d->last_value = value;
    return diff;
}

static inline void diff_reset(struct differentiator *d)
{
    d->last_value = NAN;
}

/* ========================================================================== */
/* Moving Average Filter                                                      */
/* ========================================================================== */

#define MOVING_AVG_MAX_SIZE 16

struct moving_avg_filter {
    int buffer_size;
    int index;
    int count;
    float values[MOVING_AVG_MAX_SIZE];
};

static inline void mavg_init(struct moving_avg_filter *f, int buffer_size)
{
    f->buffer_size = (buffer_size > MOVING_AVG_MAX_SIZE) ? MOVING_AVG_MAX_SIZE : buffer_size;
    f->index = 0;
    f->count = 0;
}

/**
 * @brief Process a sample through the moving average filter
 * @param f Filter structure
 * @param value Input sample
 * @return Moving average output
 */
static inline float mavg_process(struct moving_avg_filter *f, float value)
{
    f->values[f->index] = value;
    f->index = (f->index + 1) % f->buffer_size;
    
    if (f->count < f->buffer_size) {
        f->count++;
    }
    
    float sum = 0.0f;
    for (int i = 0; i < f->count; i++) {
        sum += f->values[i];
    }
    
    return sum / (float)f->count;
}

static inline void mavg_reset(struct moving_avg_filter *f)
{
    f->index = 0;
    f->count = 0;
}

static inline int mavg_count(const struct moving_avg_filter *f)
{
    return f->count;
}

#endif /* FILTERS_H */
