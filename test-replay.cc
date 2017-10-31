#include "replay.capnp.h"
#include <utility>
#include <capnp/serialize.h>

struct SimpleInstruction {
  int type;
  int size;
  int reg;
};

extern "C" {
  void foobar(SimpleInstruction *out, Instruction::Reader* rd);
  void bar(SimpleInstruction *out, char *data, size_t data_size);
  int maria(const Batch::Reader* batch);
};

// void foobar(SimpleInstruction *out, Instruction::Reader* rd) {
//   SimpleInstruction &rv = *out;
//   rv.type = int(rd->getType());
//   rv.reg = rd->getReg();
//   rv.size = rd->getSize();
// }

void bar(SimpleInstruction *out, char *data, size_t data_size) {
  auto arr = kj::Array<const capnp::word>(reinterpret_cast<capnp::word*>(data), data_size, kj::NullArrayDisposer{});
  capnp::FlatArrayMessageReader r(arr.asPtr());
  auto msg = r.getRoot<Instruction>();

  SimpleInstruction &rv = *out;
  rv.type = int(msg.getType());
  rv.reg = msg.getReg();
  rv.size = msg.getSize();
}

int maria(const Batch::Reader* batch) {
  int rv = 0;
  for (const auto &thread_chunk : batch->getThreads()) {
    rv += thread_chunk.getInstructions().size();
  }
  return rv;
}
