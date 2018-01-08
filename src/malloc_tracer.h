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

#ifndef TCMALLOC_MALLOC_TRACER_H_
#define TCMALLOC_MALLOC_TRACER_H_
#include "config.h"

#include <stddef.h>                     // for size_t, NULL
#ifdef HAVE_STDINT_H
#include <stdint.h>                     // for uint32_t, uint64_t
#endif

#include "altvarint_codec.h"
#include "base/basictypes.h"
#include "malloc_trace_encoder.h"

namespace tcmalloc {

class MallocTracer {
 public:
  static inline MallocTracer *GetInstance();

  inline uint64_t TraceMalloc(size_t size);
  inline void TraceFree(uint64_t token);
  inline void TraceFreeSized(uint64_t token);
  inline uint64_t TraceRealloc(uint64_t old_token, size_t new_size);
  inline uint64_t TraceMemalign(size_t size, size_t alignment);

  static void DumpEverything();
  static void ExcludeCurrentThreadFromDumping();
  static void SPrintStats(char* start, char* end);

 private:
  MallocTracer(uint64_t _thread_id);
  ~MallocTracer();

  inline void AppendWords(int count, uint64_t first, uint64_t second);

  void SetBufPtr(char *new_value) {
    // TODO: compiler barrier
    *const_cast<char * volatile *>(&buf_ptr_) = new_value;
  }

  bool HasSpaceFor(int varints) {
    // note, buf_end is set up such that varints = 1 case doesn't need any
    // extra computation
    return (buf_ptr_ + AltVarintCodec::kMaxSize * (varints - 1) < buf_end_);
  }

  void RefreshToken();
  void RefreshTokenAndDec();
  void RefreshBufferInnerLocked(uint64_t amount, uint64_t ts_and_cpu);
  void RefreshBuffer();

  static MallocTracer *GetInstanceSlow();
  static void SetupFirstTracer();

  static void DoSetupTLS();
  static void MallocTracerDestructor(void *arg);

  inline uint64_t UpdateTSAndCPU();

  void DumpFromSaverThread();

  char *buf_ptr_;
  char *buf_end_;

  uint64_t thread_id_;

  uint64_t token_base_;
  uint64_t counter_;

  ssize_t prev_size_;
  uint64_t prev_token_;

  int last_cpu_;

  char *signal_snapshot_buf_ptr_;
  char *signal_saved_buf_ptr_;
  int destroy_count_;

  char buf_storage_[4008];
  // sizeof(MallocTracer) == 4096

  struct Storage {
    MallocTracer *ptr;
    Storage **pprev;
    Storage *next;
  };

  static __thread Storage instance_ ATTR_INITIAL_EXEC;
  static Storage *all_tracers_;
};

inline ATTRIBUTE_ALWAYS_INLINE
MallocTracer *MallocTracer::GetInstance() {
  if (instance_.ptr) {
    return instance_.ptr;
  }
  return GetInstanceSlow();
}

inline ATTRIBUTE_ALWAYS_INLINE
void MallocTracer::AppendWords(int count, uint64_t first, uint64_t second) {
  if (PREDICT_FALSE(!HasSpaceFor(count))) {
    RefreshBuffer();
  }

  char *p = buf_ptr_;

  p = AltVarintCodec::encode_unsigned(p, first);
  if (count > 1) {
    p = AltVarintCodec::encode_unsigned(p, second);
  }

  SetBufPtr(p);
}

inline ATTRIBUTE_ALWAYS_INLINE
uint64_t MallocTracer::TraceMalloc(size_t size) {
  if (!--counter_) {
    RefreshTokenAndDec();
  }
  uint64_t token = token_base_ - counter_;

  uint64_t to_encode = MallocTraceEncoder::encode_malloc(size, &prev_size_);
  AppendWords(1, to_encode, to_encode);
  return token;
}

inline ATTRIBUTE_ALWAYS_INLINE
void MallocTracer::TraceFree(uint64_t token) {
  uint64_t to_encode = MallocTraceEncoder::encode_free(token, &prev_token_);
  AppendWords(1, to_encode, to_encode);
}

inline ATTRIBUTE_ALWAYS_INLINE
void MallocTracer::TraceFreeSized(uint64_t token) {
  uint64_t to_encode = MallocTraceEncoder::encode_free_sized(token, &prev_token_);
  AppendWords(1, to_encode, to_encode);
}

inline ATTRIBUTE_ALWAYS_INLINE
uint64_t MallocTracer::TraceRealloc(uint64_t old_token, size_t new_size) {
  if (!--counter_) {
    RefreshTokenAndDec();
  }
  uint64_t token = token_base_ - counter_;

  MallocTraceEncoder::pair p =
      MallocTraceEncoder::encode_realloc(old_token, new_size,
                                    &prev_size_, &prev_token_);
  AppendWords(2, p.first, p.second);
  return token;
}

inline ATTRIBUTE_ALWAYS_INLINE
uint64_t MallocTracer::TraceMemalign(size_t size, size_t alignment) {
  if (!--counter_) {
    RefreshTokenAndDec();
  }
  uint64_t token = token_base_ - counter_;

  MallocTraceEncoder::pair p =
      MallocTraceEncoder::encode_memalign(size, alignment, &prev_size_);
  AppendWords(2, p.first, p.second);
  return token;
}


} // namespace tcmalloc

#endif  // TCMALLOC_MALLOC_TRACER_H_
