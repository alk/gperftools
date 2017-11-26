// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// #include <utility>
#include <stdio.h>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <capnp/pretty-print.h>

#include "replay.capnp.h"

int main() {
  ::capnp::PackedFdMessageReader message(0);
  auto batch = message.getRoot<replay::Batch>();
  printf("%s\n", capnp::prettyPrint(batch).flatten().cStr());
  return 0;
}
