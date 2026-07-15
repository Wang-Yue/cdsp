#ifndef CLIB_RESAMPLER_APPLE_RESAMPLER_H
#define CLIB_RESAMPLER_APPLE_RESAMPLER_H

/**
 * @file apple_resampler.h
 * @brief Apple Core Audio AudioConverter resampler wrapper.
 */

struct resampler_vtable;
extern const struct resampler_vtable g_apple_resampler_vtable;

#endif  // CLIB_RESAMPLER_APPLE_RESAMPLER_H
