#ifndef CLIB_PROCESSORS_BIQUAD_PROCESSOR_H
#define CLIB_PROCESSORS_BIQUAD_PROCESSOR_H

/**
 * @file biquad_processor.h
 * @brief Internal multi-channel biquad processor.
 *
 * Provides optimized multi-channel biquad processing (interleaved and systolic
 * cascade) within a dsp_processor_t wrapper without exposing biquad internals
 * to the pipeline loop.
 */

struct processor_vtable;

extern const struct processor_vtable g_biquad_processor_vtable;

#endif  // CLIB_PROCESSORS_BIQUAD_PROCESSOR_H
