// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include "actual_replay.h"

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include "replay.capnp.h"

ReplayDumper::ThreadState* ReplayDumper::find_thread(
    uint64_t thread_id, bool *live_ptr) {
  auto pair = per_thread_instructions_.emplace(
    thread_id, ThreadState(thread_id, live_ptr));
  return &(pair.first->second);
}

constexpr int kIterationSize = 4096;

void ReplayDumper::after_record() {
  iteration_size++;
  if (iteration_size < kIterationSize) {
    return;
  }

  flush_chunk();
}

void ReplayDumper::record_malloc(
  ThreadState* state, uint64_t tok, uint64_t size,
  uint64_t timestamp) {

  auto reg = ids_space_.allocate_id();
  allocated_[tok] = reg;
  state->instructions.push_back(Instruction::Malloc(reg, size));

  after_record();
}

void ReplayDumper::record_free(
  ThreadState* state, uint64_t tok, uint64_t timestamp) {

  assert(allocated_.count(tok) == 1);
  auto reg = allocated_[tok];
  allocated_.erase(tok);
  freed_this_iteration_.insert(reg);

  state->instructions.push_back(Instruction::Free(reg));

  after_record();
}

struct ChunkInfo {
  uint64_t thread_count;
};

struct ThreadInfo {
  uint64_t thread_id;
  bool live;
  uint32_t instructions_count;
};

class FunctionOutputStream : ::kj::OutputStream {
public:
  FunctionOutputStream(const ReplayDumper::writer_fn_t& writer) : writer_(writer) {}
  ~FunctionOutputStream() = default;

  virtual override void write(const void* buffer, size_t size) {
    writer_(buffer, size);
  }
private:
  const ReplayDumper::writer_fn_t &writer_;
};

void ReplayDumper::flush_chunk() {
  ::capnp::MallocMessageBuilder message;
  Batch::Builder batch = message.initRoot<Batch>();
  ::capnp::List<ThreadChunk>::Builder threads = batch.initThreads(per_thread_instructions_.size());

  int idx = 0;
  for (auto &pair : per_thread_instructions_) {
    auto thread_id = pair.first;
    auto &state = pair.second;
    assert(state.thread_id == thread_id);
    auto live = *(state.live_ptr);

    ThreadChunk::Builder tinfo = threads[idx++];

    tinfo.setThreadID(thread_id);
    tinfo.setLive(live);
    ::capnp::List<Instruction>::Builder instructions = tinfo.initInstructions(state.instructions.size());

    // ThreadInfo tinfo;
    // tinfo.thread_id = thread_id;
    // tinfo.live = live;
    // tinfo.instructions_count = state.instructions.size();

    // writer_fn_(&tinfo, sizeof(tinfo));

    int instruction_idx = 0;
    for (auto &instr : state.instructions) {
      auto builder = instructions[instruction_idx++];
      builder.setType(instr.type);
      builder.setReg(instr.reg);
      builder.setSize(instr.size);
    }
  }

  {
    FunctionOutputStream os(writer_fn_);
    ::capnp::writePackedMessage(os, message);
  }

  for (auto reg : freed_this_iteration_) {
    ids_space_.free_id(reg);
  }

  per_thread_instructions_.clear();
  iteration_size = 0;
}

// void ReplayDumper::flush_chunk() {
//   ChunkInfo info;
//   info.thread_count = per_thread_instructions_.size();
//   writer_fn_(&info, sizeof(info));

//   for (auto &pair : per_thread_instructions_) {
//     auto thread_id = pair.first;
//     auto &state = pair.second;
//     assert(state.thread_id == thread_id);
//     auto live = *(state.live_ptr);

//     ThreadInfo tinfo;
//     tinfo.thread_id = thread_id;
//     tinfo.live = live;
//     tinfo.instructions_count = state.instructions.size();

//     writer_fn_(&tinfo, sizeof(tinfo));

//     for (auto &instr : state.instructions) {
//       writer_fn_(&instr, sizeof(instr));
//     }
//   }

//   for (auto reg : freed_this_iteration_) {
//     ids_space_.free_id(reg);
//   }

//   per_thread_instructions_.clear();
//   iteration_size = 0;
// }
