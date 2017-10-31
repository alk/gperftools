// -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include "../actual_replay.h"
#include <assert.h>

#include "run_benchmark.h"

static void id_tree_simple(long iterations, uintptr_t param) {
  constexpr int kPerIter = 4096;
  IdTree tree;

  for (int i = 0; i < kPerIter*2; i++) {
    uint64_t tok = tree.allocate_id();
    assert(tok == i);
  }

  for (int i = 0; i < kPerIter; i++) {
    tree.free_id(i * 2);
  }

  for (; iterations > 0; iterations -= kPerIter) {
    uint64_t regs[kPerIter];
    for (int i = 0; i < kPerIter; i++) {
      regs[i] = tree.allocate_id();
    }
    asm volatile("" : : : "memory");
    for (int i = 0; i < kPerIter; i++) {
      tree.free_id(regs[i]);
    }
  }
}

int main(void) {
  report_benchmark("id_tree_simple", id_tree_simple, 0);
  return 0;
}
