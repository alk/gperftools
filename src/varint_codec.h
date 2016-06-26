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

#ifndef VARINT_CODEC_H
#define VARINT_CODEC_H
#include "config.h"

#include <stdint.h>

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
public:
  // const int kMaxBytes = 10;

  static char *encode_unsigned(char *place, uint64_t val);
  static char *encode_signed(char *place, int64_t val);

  static uint64_t zigzag(int64_t val) {
    // uval = (uval << 1) | (uval >> 63);
    // uval ^= static_cast<uint64_t>(val >> 63);

    val = (val << 1) ^ (val >> 63);
    return static_cast<uint64_t>(val);
  }

  static int64_t unzigzag(uint64_t val) {
    uint64_t sign = val & 1;
    return (val >> 1) ^ (0 - sign);
  }

  static DecodeResult<uint64_t> decode_unsigned(char *place);
  static DecodeResult<int64_t> decode_signed(char *place);

  static __attribute__ ((visibility("internal")))  unsigned char encode_bits[64];
  static __attribute__ ((visibility("internal")))  uint64_t decode_masks[9];

private:
  static char *encode_varint_huge(char *place, uint64_t val, uint64_t high);
  static DecodeResult<uint64_t> decode_huge_varint_slow(char *place, uint64_t val, unsigned p);
};

inline char *VarintCodec::encode_signed(char *place, int64_t val) {
  return encode_unsigned(place, zigzag(val));
}

__attribute__((always_inline))
inline char *VarintCodec::encode_unsigned(char *place, uint64_t val) {
  // TODO: switch to classic varint encoding maybe which is maybe
  // slightly faster (i.e. because single-byte encoding it slightly
  // cheaper)
  //
  // while (__builtin_expect(val >= 128ULL, 0)) {
  //   *place++ = val | 0x80;
  //   val >>= 7;
  // }
  // *place++ = val;
  // return place;

  // *(uint64_t *)place = val;
  // return place+8;

  if (__builtin_expect(val < 128ULL, 1)) {
    val = (val << 1) | 1;
    *place = val;
    return place + 1;
  }

  // if (__builtin_expect(val < (0x10000ULL >> 2), 1)) {
  //   val = (val << 1) | 1;
  //   *(uint16_t *)place = (val << 1);
  //   return place + 2;
  // }

  // if (val >= (1ULL << 53)) {
  if ((val >> 53) != 0) {
    uint64_t high = val >> 54;
    return encode_varint_huge(place, val, high);
  }
#if 0
  *(uint64_t *)place = val;
  return place + 8;
#elif 1
  uint64_t bits;
  val = (val<<1) | 1;
  __asm__ (
    // "leaq 1(%0, %0, 1), %0\n\t"
    "xorl %%ecx, %%ecx\n\t"
    "bsrq %0, %%rcx\n\t"
    "movzbl (%3, %%rcx), %%ecx\n\t"
    // "stc\n\t"
    "shlq %%cl, %0"
    : "=&r" (val),
      "=&c" (bits)
    : "0" (val), "r" (&encode_bits));
  *(uint64_t *)place = val;
  return place + bits + 1;
#else
  if (__builtin_expect(val < 128ULL, 1)) {
    val = (val << 1) | 1;
    *place = val;
    return place + 1;
  }

  uint64_t high = val >> 54;
  if (high) {
    return encode_varint_huge(place, val, high);
  }

  val = (val << 1) | 1;

  // if (__builtin_expect(val < (0x10000 >> 1), 1)) {
  //   *(uint16_t *)place = (val << 1);
  //   return place + 2;
  // }
  // if (__builtin_expect(val < (0x1000000 >> 2), 1)) {
  //   *(uint32_t *)place = (val << 2);
  //   return place + 3;
  // }
  // if (__builtin_expect(val < (0x100000000 >> 3), 1)) {
  //   *(uint32_t *)place = (val << 3);
  //   return place + 4;
  // }

  unsigned bit = (63 - __builtin_clzll(val));
  unsigned bits = encode_bits[bit];
  val = val << bits;
  *(uint64_t *)place = val;

  return place + bits + 1;
#endif
}

inline VarintCodec::DecodeResult<int64_t> VarintCodec::decode_signed(char *place) {
  DecodeResult<uint64_t> t = decode_unsigned(place);
  int64_t val = static_cast<int64_t>(t.value >> 1);
  val ^= -static_cast<int64_t>(t.value & 1);
  return DecodeResult<int64_t>::make(t.advance, val);
}

inline VarintCodec::DecodeResult<uint64_t> VarintCodec::decode_unsigned(char *place)
{
  uint64_t val = *(uint64_t *)place;

  if (__builtin_expect(val & 1, 1)) {
    return DecodeResult<uint64_t>::make(1, (val & 0xff) >> 1);
  }
  // if (val & 3) {
  //   return DecodeResult<uint64_t>::make(place + 2, (val & 0xffff) >> 2);
  // }

  // unsigned p = __builtin_ffsll(val);
  unsigned p;
  __asm__ ("xorl %0, %0; bsfl %1, %0" : "=&r"(p) : "r"((uint32_t)val));
  p += 1;
  val >>= p;
  if (__builtin_expect(p > 8, 0)) {
    return decode_huge_varint_slow(place, val, p);
  }

  // uint64_t mask = (1ULL << (7 * p)) - 1;
  uint64_t mask = decode_masks[p];
  return DecodeResult<uint64_t>::make(p, val & mask);
}

#endif
