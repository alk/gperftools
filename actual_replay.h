// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#ifndef ACTUAL_REPLAY_H
#define ACTUAL_REPLAY_H
#include <assert.h>
#include <functional>
#include <stdint.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class IdTree {
public:
  uint64_t allocate_id();
  void free_id(uint64_t id);

private:

  typedef std::vector<uint64_t> bvector;

  // bit value of 1 means available
  bvector level0;
   // Level 1 has bit per every word in level 0. Bit value of 1 means
   // at least one bit in matching level0 _word_ is available (set to
   // 1).
  bvector level1;
  bvector level2;
  bvector level3;

  static unsigned bsf(uint64_t p) {
    assert(p != 0);
    return __builtin_ffsll(p) - 1;
  }

  static bool find_set_bit(const bvector &v, uint64_t *pos) {
    for (auto i = v.begin(); i != v.end(); i++) {
      uint64_t val = *i;
      if (val != 0) {
        *pos = (i - v.begin())*64 + bsf(val);
        return true;
      }
    }
    return false;
  }

  // returns true iff higher level needs to flip
  static bool set_bit_vector(uint64_t pos, bool bval, bvector &v) {
    auto divpos = pos / 64;
    if (v.size() <= divpos) {
      assert(v.size() == divpos);
      v.push_back(~0ULL);
    }
    uint64_t word = v[divpos];
    if (bval) {
      auto new_val = word | (1ULL << (pos % 64));
      v[divpos] = new_val;
      // true if we flipped from all 0s to single 1
      return (word == 0);
    } else {
      auto new_val = word & ~(1ULL << (pos % 64));
      v[divpos] = new_val;
      // true if we flipped to all 0s
      return (new_val == 0);
    }
  }

  void set_bit(uint64_t pos, bool bval) {
    if (!set_bit_vector(pos, bval, level0)) {
      return;
    }
    pos /= 64;
    if (!set_bit_vector(pos, bval, level1)) {
      return;
    }
    pos /= 64;
    if (!set_bit_vector(pos, bval, level2)) {
      return;
    }
    pos /= 64;
    set_bit_vector(pos, bval, level3);
  }
};

inline uint64_t IdTree::allocate_id() {
  uint64_t pos;
  bool ok = find_set_bit(level3, &pos);
  if (!ok) {
    pos = level0.size() * 64;
    set_bit(pos, false);
    return pos;
  }

  unsigned p2 = bsf(level2[pos]);
  uint64_t pos2 = pos * 64 + p2;
  unsigned p1 = bsf(level1[pos2]);
  uint64_t pos1 = pos2 * 64 + p1;
  unsigned p0 = bsf(level0[pos1]);
  uint64_t pos0 = pos1 * 64 + p0;
  set_bit(pos0, false);
  return pos0;
}

inline void IdTree::free_id(uint64_t id) {
  set_bit(id, true);
}

struct Instruction {
  static constexpr uint64_t kMalloc = 0;
  static constexpr uint64_t kFree   = 1;

  uint64_t type:8;
  uint64_t reg:58;
  uint64_t size;
  // alignment...

  static Instruction Malloc(int reg, uint64_t size) {
    Instruction rv;
    rv.type = kMalloc;
    rv.reg = reg;
    rv.size = size;
    return rv;
  }
  static Instruction Free(int reg) {
    Instruction rv;
    rv.type = kFree;
    rv.reg = reg;
    return rv;
  }
};

class ReplayDumper {
public:
  struct ThreadState {
    const uint64_t thread_id;
    bool * const live_ptr;
    std::vector<Instruction> instructions;

    ThreadState(uint64_t thread_id, bool* live_ptr)
      : thread_id(thread_id), live_ptr(live_ptr) {}
  };
  typedef std::function<int (void *, size_t)> writer_fn_t;
  ReplayDumper(const writer_fn_t& writer_fn) : writer_fn_(writer_fn) {}

  ThreadState* find_thread(uint64_t thread_id, bool *live_ptr);

  void record_malloc(ThreadState* state, uint64_t tok, uint64_t size,
		     uint64_t timestamp);

  void record_free(ThreadState* state, uint64_t tok, uint64_t timestamp);

  void flush_chunk();

private:
  void after_record();

  writer_fn_t writer_fn_;
  IdTree ids_space_;
  std::unordered_map<uint64_t, ThreadState> per_thread_instructions_;
  std::unordered_set<uint64_t> freed_this_iteration_;
  // maps tok -> register number
  std::unordered_map<uint64_t, uint64_t> allocated_;
  int iteration_size{};
};

#endif
