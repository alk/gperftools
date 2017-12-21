// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "varint_codec.h"

__attribute__((aligned(64)))
unsigned char VarintCodec::encode_bits[64] = {
  0, 0, 0, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 1, 1, 1, 2,
  2, 2, 2, 2, 2, 2, 3, 3,
  3, 3, 3, 3, 3, 4, 4, 4,
  4, 4, 4, 4, 5, 5, 5, 5,
  5, 5, 5, 6, 6, 6, 6, 6,
  6, 6, 7, 7, 7, 7, 7, 7,
  7, 8, 8, 8, 8, 8, 8, 8
};

__attribute__((aligned(64)))
uint64_t VarintCodec::decode_masks[9] = {
  0x00000000LLU, 0x0000007FLLU,
  0x00003FFFLLU, 0x001FFFFFLLU,
  0x0FFFFFFFLLU, 0x7FFFFFFFFLLU,
  0x3FFFFFFFFFFLLU, 0x1FFFFFFFFFFFFLLU,
  0xFFFFFFFFFFFFFFFFLLU};
