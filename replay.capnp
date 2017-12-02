@0xcb49ade5c790d703;

$import "/capnp/c++.capnp".namespace("replay");

struct Instruction {
  enum Type {
    malloc @0;
    free @1;
    realloc @2;
    memallign @3;
  }

  type @0 :Type;
  reg @1 :UInt32;
  size @2 :UInt64;
  alignment @3 :UInt64;
  # ts @4 :UInt64;
  # cpu @5 :UInt64;
}

struct ThreadChunk {
  threadID @0 :UInt64;
  live @1 :Bool;
  instructions @2 :List(Instruction);
}

struct Batch {
  threads @0 :List(ThreadChunk);
}
