#ifndef CDSP_MSAN_COMPAT_H
#define CDSP_MSAN_COMPAT_H

#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#define CDSP_MSAN_UNPOISON(ptr, size) __msan_unpoison((ptr), (size))
#endif
#endif

#ifndef CDSP_MSAN_UNPOISON
#define CDSP_MSAN_UNPOISON(ptr, size) ((void)0)
#endif

#endif  // CDSP_MSAN_COMPAT_H
