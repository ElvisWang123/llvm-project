// RUN: %check_clang_tidy %s misc-const-correctness %t \
// RUN: -config='{CheckOptions: \
// RUN:  {misc-const-correctness.AnalyzeValues: true,\
// RUN:   misc-const-correctness.WarnPointersAsValues: true, \
// RUN:   misc-const-correctness.WarnPointersAsPointers: false, \
// RUN:   misc-const-correctness.TransformPointersAsValues: true}\
// RUN:  }' -- -fno-delayed-template-parsing

void potential_const_pointer() {
  double np_local0[10] = {0., 1., 2., 3., 4., 5., 6., 7., 8., 9.};
  double *p_local0 = &np_local0[1];
  // CHECK-MESSAGES: [[@LINE-1]]:3: warning: variable 'p_local0' of type 'double *' can be declared 'const'
  // CHECK-FIXES: double *const p_local0 = &np_local0[1];
}
