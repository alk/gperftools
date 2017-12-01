// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// #include <utility>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <assert.h>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <capnp/pretty-print.h>
#include <kj/io.h>

#include "replay.capnp.h"

static unsigned char buffer_space[128 << 10] __attribute__((aligned(4096)));

static void *registers[1024 << 10];

static void replay_instructions(const ::capnp::List<::replay::Instruction>::Reader& instructions) {
  for (auto instr : instructions) {
    printf("%s\n", capnp::prettyPrint(instr).flatten().cStr());
    switch (instr.getType()) {
    case replay::Instruction::Type::MALLOC: {
      assert(registers[instr.getReg()] == nullptr);
      auto ptr = registers[instr.getReg()] = malloc(instr.getSize());
      memset(ptr, 0, 8);
      break;
    }
    case replay::Instruction::Type::FREE:
      assert(registers[instr.getReg()] != nullptr);
      free(registers[instr.getReg()]);
      registers[instr.getReg()] = nullptr;
      break;
    case replay::Instruction::Type::REALLOC:
      registers[instr.getReg()] = realloc(registers[instr.getReg()],
                                          instr.getSize());
      break;
    case replay::Instruction::Type::MEMALLIGN:
      assert(registers[instr.getReg()] == nullptr);
      registers[instr.getReg()] = memalign(instr.getAlignment(),
                                           instr.getSize());
      break;
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
    ::capnp::PackedMessageReader message(input);
    auto batch = message.getRoot<replay::Batch>();
    for (auto threadInfo : batch.getThreads()) {
      printf("thread: %lld\n", (long long)threadInfo.getThreadID());
      replay_instructions(threadInfo.getInstructions());
    }
  }

  return 0;
}
