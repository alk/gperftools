// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include "actual_replay.h"

ThreadState* find_thread(uint64_t thread_id,
                         bool *live_ptr) {
  auto pair = per_thread_instructions_.emplace(
    thread_id, {thread_id, live_ptr});
  return &(pair.first->second);
}

void ReplayDumper::record_malloc(
  ThreadState* state, uint64_t tok, uint64_t size,
  uint64_t timestamp) {

  auto reg = ids_space_.allocate_id();
  allocated_[tok] = reg;
  state->instructions.push_back(Instruction::Malloc(reg, size));
}

void ReplayDumper::record_free(
  ThreadState* state, uint64_t tok, uint64_t timestamp) {

  assert(allocated_.cound(tok) == 1);
  auto reg = allocated_[tok];
  allocated_.erase(tok);
  freed_this_iteration_.insert(reg);

  state->instructions.push_back(Instruction::Free(reg));
}

struct ChunkInfo {
  uint64_t thread_count;
};

struct ThreadInfo {
  uint64_t thread_id;
  bool live;
  uint32_t instructions_count;
};

void ReplayDumper::flush_chunk() {
  ChunkInfo info;
  info.thread_count = per_thread_instructions_.size();
  writer_fn_(&info, sizeof(info));

  for (auto &pair : per_thread_instructions_) {
    auto thread_id = pair.first;
    auto &state = pair.second;
    assert(state.thread_id == thread_id);
    auto live = *(state.live_ptr);

    ThreadInfo tinfo;
    tinfo.thread_id = thread_id;
    tinfo.live = live;
    tinfo.instructions_count = state.instructions.size();

    writer_fn_(&tinfo, sizeof(tinfo));

    for (auto &instr : state.instructions) {
      writer_fn_(&instr, sizeof(instr));
    }
  }

  for (auto reg : freed_this_iteration_) {
    ids_space_.free_id(reg);
  }

  per_thread_instructions_.clear();
}
