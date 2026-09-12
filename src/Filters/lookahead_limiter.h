#ifndef CLIB_FILTERS_LOOKAHEAD_LIMITER_H
#define CLIB_FILTERS_LOOKAHEAD_LIMITER_H

struct filter_vtable;

extern const struct filter_vtable g_lookahead_gain_vtable;
extern const struct filter_vtable g_lookahead_limiter_vtable;

#endif  // CLIB_FILTERS_LOOKAHEAD_LIMITER_H
