@0xcb49ade5c790d703;

# using Cxx = import "/capnp/c++.capnp";
# $Cxx.namespace("replay");

struct Instruction {
  enum Type {
    malloc @0;
    free @1;
    realloc @2;
    memallign @3;
  }

  type @0 :Type;
  reg @1 :UInt32;
  ts @2 :UInt64;
  cpu @3 :UInt64;
  size @4 :UInt64;
  alignment @5 :UInt64;
}

struct ThreadChunk {
  threadID @0 :UInt64;
  live @1 :Bool;
  instructions @2 :List(Instruction);
}

struct Batch {
  threads @0 :List(ThreadChunk);
}
