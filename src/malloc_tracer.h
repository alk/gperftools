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

#ifndef MALLOC_TRACER_H
#define MALLOC_TRACER_H

#include <config.h>
#include <stddef.h>                     // for size_t, NULL

#ifdef HAVE_STDINT_H
#include <stdint.h>                     // for uint32_t, uint64_t
#endif

#include "base/basictypes.h"
#include "malloc_trace_encoder.h"
#include "varint_codec.h"

namespace tcmalloc {

class MallocTracer {
public:
  static MallocTracer *GetInstance() {
    if (instance.ptr) {
      return instance.ptr;
    }
    return GetInstanceSlow();
  }

  ATTRIBUTE_ALWAYS_INLINE
  uint64_t TraceMalloc(size_t size) {
    uint64_t to_encode = EventsEncoder::encode_malloc(size, &prev_size);

    if (!--counter) {
      RefreshTokenAndDec();
    }

    uint64_t token = token_base - counter;

    if (!HasSpaceFor(1)) {
      RefreshBuffer(1, to_encode, to_encode);
      return token;
    }

    SetBufPtr(VarintCodec::encode_unsigned(buf_ptr, to_encode));

    return token;
  }

  ATTRIBUTE_ALWAYS_INLINE
  void TraceFree(uint64_t token) {
    uint64_t to_encode = EventsEncoder::encode_free(token, &prev_token);

    if (!HasSpaceFor(1)) {
      RefreshBuffer(1, to_encode, to_encode);
      return;
    }

    SetBufPtr(VarintCodec::encode_unsigned(buf_ptr, to_encode));
  }

  ATTRIBUTE_ALWAYS_INLINE
  void TraceFreeSized(uint64_t token, size_t size) {
    EventsEncoder::pair p =
      EventsEncoder::encode_free_sized(token, size,
                                       &prev_token, &prev_size);

    if (!HasSpaceFor(2)) {
      RefreshBuffer(2, p.first, p.second);
      return;
    }

    char *wp = buf_ptr;

    wp = VarintCodec::encode_unsigned(wp, p.first);
    wp = VarintCodec::encode_unsigned(wp, p.second);

    SetBufPtr(wp);
  }

  uint64_t TraceRealloc(uint64_t old_token, size_t new_size) {
    EventsEncoder::pair p =
      EventsEncoder::encode_realloc(old_token, new_size,
                                    &prev_size, &prev_token);

    if (!--counter) {
      RefreshTokenAndDec();
    }

    uint64_t token = token_base - counter;

    if (!HasSpaceFor(2)) {
      RefreshBuffer(2, p.first, p.second);
      return token;
    }

    char *wp = buf_ptr;

    wp = VarintCodec::encode_unsigned(wp, p.first);
    wp = VarintCodec::encode_unsigned(wp, p.second);

    SetBufPtr(wp);

    return token;
  }

  uint64_t TraceMemalign(size_t size, size_t alignment) {
    EventsEncoder::pair p =
      EventsEncoder::encode_memalign(size, alignment, &prev_size);

    if (!--counter) {
      RefreshTokenAndDec();
    }

    uint64_t token = token_base - counter;

    if (!HasSpaceFor(2)) {
      RefreshBuffer(2, p.first, p.second);
      return token;
    }

    char *wp = buf_ptr;

    wp = VarintCodec::encode_unsigned(wp, p.first);
    wp = VarintCodec::encode_unsigned(wp, p.second);

    SetBufPtr(wp);

    return token;
  }

  ~MallocTracer();

  static void DumpEverything();

  static void ExcludeCurrentThreadDumping();

  struct Storage {
    MallocTracer *ptr;
    Storage **pprev;
    Storage *next;
  };

  static void SPrintStats(char* start, char* end);

private:
  MallocTracer(uint64_t _thread_id);

  void SetBufPtr(char *new_value) {
    *const_cast<char * volatile *>(&buf_ptr) = new_value;
  }

  bool HasSpaceFor(int varints) {
    return (buf_ptr + 10 * (varints - 1) < buf_end);
  }

  void RefreshToken();
  void RefreshTokenAndDec();
  void RefreshBufferInnerLocked(uint64_t, bool from_saver);
  void RefreshBuffer(int count, uint64_t first, uint64_t second);

  static MallocTracer *GetInstanceSlow();
  static void SetupFirstTracer();

  static void do_setup_tls();
  static void malloc_tracer_destructor(void *arg);

  void DumpFromSaverThread();

  uint64_t thread_id;

  uint64_t token_base;
  uint64_t counter;

  ssize_t prev_size;
  uint64_t prev_token;

  char *buf_ptr;
  char *buf_end;
  char *signal_snapshot_buf_ptr;
  char *signal_saved_buf_ptr;
  char buf_storage[3072+768+128+8+64];
  // sizeof(MallocTracer) == 4024

  int destroy_count;

  static __thread Storage instance ATTR_INITIAL_EXEC;
  static Storage *all_tracers;
};

} // namespace tcmalloc

#endif // MALLOC_TRACER_H
