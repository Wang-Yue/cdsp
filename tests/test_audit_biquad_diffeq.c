// Regression tests for audit report 01, section 3 (diffeq.c).
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "config/config_gen.h"
#include "filters/diffeq.h"
#include "filters/filter.h"
#include "test_support.h"

static void *audit_make_diffeq(double *a, size_t na, double *b, size_t nb) {
  diff_eq_config_t p = {.a = a, .a_count = na, .b = b, .b_count = nb};
  filter_config_t cfg = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p};
  return g_diffeq_vtable.create("audit", &cfg, 48000, 0, NULL, NULL);
}

// 3.1: a negative a[0] with a shorter b must not make the zero padding -0.0,
// and the state-transfer comparison treats +0.0 and -0.0 as equal. Here the
// old filter pads b with +0.0 and the new one has an explicit 0.0 that is
// scaled to -0.0 (as upstream); the coefficients are numerically identical,
// so the state must carry over.
TEST(AuditDiffeq_SignedZeroPaddingTransfersState) {
  double a[] = {-1.0, 0.5};
  double b_short[] = {1.0};
  double b_long[] = {1.0, 0.0};
  void *src = audit_make_diffeq(a, 2, b_short, 1);
  void *dst = audit_make_diffeq(a, 2, b_long, 2);
  void *ref = audit_make_diffeq(a, 2, b_short, 1);
  ASSERT_TRUE(src && dst && ref);

  double w1[8] = {1.0, 0.5, -0.25, 0.75, 0, 0, 0, 0};
  double w2[8];
  memcpy(w2, w1, sizeof(w1));
  g_diffeq_vtable.process(src, w1, 4);
  g_diffeq_vtable.process(ref, w2, 4);

  g_diffeq_vtable.transfer_state(dst, src);

  double z1[8] = {0}, z2[8] = {0};
  g_diffeq_vtable.process(dst, z1, 8);
  g_diffeq_vtable.process(ref, z2, 8);
  for (size_t i = 0; i < 8; i++) {
    ASSERT_NEAR(z2[i], z1[i], 1e-15);
  }
  ASSERT_TRUE(fabs(z1[0]) > 0.0);

  g_diffeq_vtable.free(src);
  g_diffeq_vtable.free(dst);
  g_diffeq_vtable.free(ref);
}

TEST_MAIN()
