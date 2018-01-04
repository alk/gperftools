// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2016, gperftools Contributors
// All rights reserved.
//
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

#ifndef VARINT_CODEC_H
#define VARINT_CODEC_H
#include <stdint.h>
#include <string.h>

namespace tcmalloc {

class VarintCodec {
public:
  template <typename T>
  struct DecodeResult {
    uint64_t advance;
    T value;

    static DecodeResult make(uint64_t advance, const T &value) {
      DecodeResult rv;
      rv.advance = advance;
      rv.value = value;
      return rv;
    }
  };

  static char *encode_unsigned(char *place, uint64_t val);
  static char *encode_signed(char *place, int64_t val);

  static uint64_t zigzag(int64_t val) {
    val = (val << 1) ^ (val >> 63);
    return static_cast<uint64_t>(val);
  }

  static int64_t unzigzag(uint64_t val) {
    uint64_t sign = val & 1;
    return (val >> 1) ^ (0 - sign);
  }

  static DecodeResult<uint64_t> decode_unsigned(const char *place);
  static DecodeResult<int64_t> decode_signed(const char *place);

private:
  static __attribute__ ((visibility("internal")))  unsigned char encode_bits[64];
  static __attribute__ ((visibility("internal")))  uint64_t decode_masks[9];
};

inline char *VarintCodec::encode_signed(char *place, int64_t val) {
  return encode_unsigned(place, zigzag(val));
}

__attribute__((always_inline))
inline char *VarintCodec::encode_unsigned(char *place, uint64_t val) {
  // if (__builtin_expect(val < 128ULL, 1)) {
  //   val = (val << 1) | 1;
  //   *place = val;
  //   return place + 1;
  // }

  if (__builtin_expect((val >> 53) != 0, 0)) {
    uint16_t high = val >> 54;
    val = (val << 10) | (1 << 9);
    memcpy(place, &val, sizeof(val));
    memcpy(place + 8, &high, sizeof(high));
    return place + 10;
  }
  val = (val << 1) | 1;

#if !defined(__x86_64__)
  unsigned bit = (63 - __builtin_clzll(val));
  unsigned bits = encode_bits[bit];
  val = val << bits;
#else
  // asm for code above. Rare compiler is able to optimize bsr well. I
  // experimentally found seemingly unnecessary XOR instruction to
  // help a lot. Might be that "famous" false dependency thingy for
  // BSR.
  uint64_t bits;
  __asm__ (
    "xorl %%ecx, %%ecx\n\t"
    "bsrq %0, %%rcx\n\t"
    "movzbl (%3, %%rcx), %%ecx\n\t"
    "shlq %%cl, %0"
    : "=&r" (val),
      "=&c" (bits)
    : "0" (val), "r" (&encode_bits));
#endif

  memcpy(place, &val, sizeof(val));
  return place + bits + 1;
}

inline VarintCodec::DecodeResult<int64_t> VarintCodec::decode_signed(const char *place) {
  DecodeResult<uint64_t> t = decode_unsigned(place);
  int64_t val = static_cast<int64_t>(t.value >> 1);
  val ^= -static_cast<int64_t>(t.value & 1);
  return DecodeResult<int64_t>::make(t.advance, val);
}

inline VarintCodec::DecodeResult<uint64_t> VarintCodec::decode_unsigned(const char *place)
{
  uint64_t val = *reinterpret_cast<const uint64_t *>(place);

  // if (__builtin_expect(val & 1, 1)) {
  //   return DecodeResult<uint64_t>::make(1, (val & 0xff) >> 1);
  // }
  // if (val & 3) {
  //   return DecodeResult<uint64_t>::make(place + 2, (val & 0xffff) >> 2);
  // }

#if !defined(__x86_64__)
  unsigned p = __builtin_ffsll(val);
#else
  unsigned p;
  __asm__ ("xorl %0, %0; bsfl %1, %0" : "=&r"(p) : "r"((uint32_t)val));
#endif
  p += 1;
  val >>= p;
  if (__builtin_expect(p > 8, 0)) {
    uint16_t high;
    memcpy(&high, place + 8, sizeof(high));
    val |= ((uint64_t)high << (64-p));
    return DecodeResult<uint64_t>::make(p, val);
  }

  uint64_t mask = decode_masks[p];
  return DecodeResult<uint64_t>::make(p, val & mask);
}

} // namespace tcmalloc

#endif
