#ifndef CLIB_FILTERS_LOOKAHEAD_LIMITER_H
#define CLIB_FILTERS_LOOKAHEAD_LIMITER_H

struct filter_vtable;

extern const struct filter_vtable g_lookahead_gain_vtable;
extern const struct filter_vtable g_lookahead_limiter_vtable;

/**
 * @brief Push `attack_samples` of silence into lookahead history.
 *
 * Used when processor parameters change to match upstream behavior.
 */
void lookahead_gain_pad_silence(void *gain_ptr);

#endif // CLIB_FILTERS_LOOKAHEAD_LIMITER_H
