// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// #include <utility>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <assert.h>

#include <sched.h>

#include <atomic>
#include <vector>
#include <thread>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <capnp/pretty-print.h>
#include <kj/io.h>

#include "replay.capnp.h"

#ifdef HAVE_BUILTIN_EXPECT
#define PREDICT_TRUE(x) __builtin_expect(!!(x), 1)
#define PREDICT_FALSE(x) __builtin_expect(!!(x), 0)
#else
#define PREDICT_TRUE(x) (x)
#define PREDICT_FALSE(x) (x)
#endif

static unsigned char buffer_space[128 << 10] __attribute__((aligned(4096)));

static uintptr_t registers[1024 << 10];

static void *read_register(int reg) {
  uintptr_t rv = registers[reg];

  if (PREDICT_FALSE(rv == 0)) {
    std::atomic<uintptr_t>* place = reinterpret_cast<std::atomic<uintptr_t>*>(registers + reg);
    do {
      sched_yield();
    } while (place->load(std::memory_order_seq_cst) == 0);
  }

  return reinterpret_cast<void *>(rv);
}

static void write_register(int reg, void *val) {
  registers[reg] = reinterpret_cast<uintptr_t>(val);
}

static void replay_instructions(const ::capnp::List<::replay::Instruction>::Reader& instructions) {
  for (auto instr : instructions) {
    // printf("%s\n", capnp::prettyPrint(instr).flatten().cStr());
    auto reg = instr.getReg();
    switch (instr.getType()) {
    case replay::Instruction::Type::MALLOC: {
      assert(registers[reg] == 0);
      auto ptr = malloc(instr.getSize());
      if (ptr == nullptr) {
        abort();
      }
      write_register(reg, ptr);
      memset(ptr, 0, 8);
      break;
    }
    case replay::Instruction::Type::FREE:
      free(read_register(reg));
      write_register(reg, nullptr);
      break;
    case replay::Instruction::Type::REALLOC: {
      auto ptr = read_register(reg);
      ptr = realloc(ptr, instr.getSize());
      if (ptr == nullptr) {
        abort();
      }
      write_register(reg, ptr);
      break;
    }
    case replay::Instruction::Type::MEMALLIGN: {
      assert(registers[reg] == 0);
      auto ptr = memalign(instr.getAlignment(), instr.getSize());
      if (ptr == nullptr) {
        abort();
      }
      write_register(reg, ptr);
      break;
    }
    default:
      abort();
    }
  }
}

int main() {
  ::kj::FdInputStream fd0(0);
  ::kj::BufferedInputStreamWrapper input(fd0,
                                         kj::arrayPtr(buffer_space, sizeof(buffer_space)));

  while (input.tryGetReadBuffer() != nullptr) {
    ::capnp::InputStreamMessageReader message(input);
    auto batch = message.getRoot<replay::Batch>();

    std::vector<std::thread> threads;

    auto threadsList = batch.getThreads();

    assert(threadsList.size() > 0);

    auto threadsListEnd = threadsList.end();

    for (auto iter = threadsList.begin() + 1; iter != threadsListEnd; ++iter) {
      auto threadInfo = *iter;
      // printf("thread: %lld\n", (long long)threadInfo.getThreadID());
      threads.emplace_back(std::thread([threadInfo] () {
          replay_instructions(threadInfo.getInstructions());
          }));
    }

    {
      auto threadInfo = *(threadsList.begin());
      // printf("1thread: %lld\n", (long long)threadInfo.getThreadID());
      replay_instructions(threadInfo.getInstructions());
    }

    for (auto &t : threads) {
      t.join();
    }

    // printf("end of batch!\n\n\n");
  }

  return 0;
}
