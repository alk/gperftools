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

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>

#include <sys/socket.h>
#include <netdb.h>
#include <sys/un.h>

#include <x86intrin.h>

#include "page_heap_allocator.h"
#include "base/spinlock.h"
#include "base/googleinit.h"
#include "internal_logging.h"
#include "malloc_tracer.h"
#include "malloc_tracer_buf.h"

namespace tcmalloc {

const unsigned EventsEncoder::kEventMalloc;
const unsigned EventsEncoder::kEventFree;
const unsigned EventsEncoder::kEventTok;
const unsigned EventsEncoder::kEventBuf;
const unsigned EventsEncoder::kEventFreeSized;
const unsigned EventsEncoder::kEventExtBase;
const unsigned EventsEncoder::kTypeShift;
const unsigned EventsEncoder::kTypeMask;
const unsigned EventsEncoder::kEventDeath;
const unsigned EventsEncoder::kEventEnd;
const unsigned EventsEncoder::kEventRealloc;
const unsigned EventsEncoder::kEventMemalign;
const unsigned EventsEncoder::kEventSyncBarrier;
const unsigned EventsEncoder::kExtTypeShift;
const unsigned EventsEncoder::kExtTypeMask;

static const int kDumperPeriodMicros = 3000;

static SpinLock lock(base::LINKER_INITIALIZED);

static const int kTokenSize = 4 << 10;
static const int kTSShift = 10;
static const uint64_t kTSMask = ~((1ULL << kTSShift) - 1);

static uint64_t token_counter;
static uint64_t thread_id_counter;
static uint64_t thread_dump_written;

static uint64_t base_ts;

__thread MallocTracer::Storage MallocTracer::instance ATTR_INITIAL_EXEC;
__thread bool had_tracer;

MallocTracer::Storage *MallocTracer::all_tracers;

static pthread_key_t instance_key;
static pthread_once_t setup_once = PTHREAD_ONCE_INIT;
static pthread_once_t first_tracer_setup_once = PTHREAD_ONCE_INIT;

static tcmalloc::PageHeapAllocator<MallocTracer> malloc_tracer_allocator;

static TracerBuffer* tracer_buffer;

static bool no_more_writes;

static union {
  struct {
    void *a, *b;
  } s;
  char space[sizeof(MallocTracer) + sizeof(void*)];
} first_tracer_space;

static MallocTracer *get_first_tracer() {
  return reinterpret_cast<MallocTracer *>(&first_tracer_space.s);
}

void MallocTracer::malloc_tracer_destructor(void *arg) {
  CHECK_CONDITION(!had_tracer);

  MallocTracer::Storage *instanceptr =
    reinterpret_cast<MallocTracer::Storage *>(arg);

  MallocTracer *tracer = instanceptr->ptr;

  // have pthread call us again on next destruction iteration and give
  // rest of tls destructors chance to get traced properly
  if (tracer->destroy_count++ < 3) {
    pthread_setspecific(instance_key, instanceptr);
    return;
  }

  if (instanceptr->pprev) {
    SpinLockHolder l(&lock);
    MallocTracer::Storage *s = *instanceptr->pprev = instanceptr->next;
    if (s) {
      s->pprev = instanceptr->pprev;
    }
    instanceptr->pprev = reinterpret_cast<MallocTracer::Storage **>(0xababababababababULL);
    instanceptr->next = reinterpret_cast<MallocTracer::Storage *>(0xcdcdcdcdcdcdcdcdULL);
  }

  had_tracer = true;
  instanceptr->ptr = NULL;
  tracer->~MallocTracer();

  if (tracer == get_first_tracer()) {
    return;
  }

  SpinLockHolder h(&lock);
  malloc_tracer_allocator.Delete(tracer);
}


void MallocTracer::SetupFirstTracer() {
  base_ts = __rdtsc() & kTSMask;
  new (get_first_tracer()) MallocTracer(0);
}

// guards cases of malloc calls during do_setup
static __thread bool in_setup ATTR_INITIAL_EXEC;

void MallocTracer::do_setup_tls() {
  in_setup = true;

  tracer_buffer = TracerBuffer::GetInstance();

  malloc_tracer_allocator.Init();
  int rv = pthread_key_create(&instance_key, &MallocTracer::malloc_tracer_destructor);
  CHECK_CONDITION(!rv);

  in_setup = false;
}

static void *dumper_thread(void *__dummy) {
  while (true) {
    usleep(kDumperPeriodMicros);
    MallocTracer::DumpEverything();
  }
  return NULL;
}

static void malloc_tracer_setup_tail() {
  (void)MallocTracer::GetInstance();

  pthread_t dumper;
  int rv = pthread_create(&dumper, 0, dumper_thread, 0);
  if (rv != 0) {
    errno = rv;
    perror("pthread_create");
    CHECK_CONDITION(rv == 0);
  }
}

REGISTER_MODULE_INITIALIZER(setup_tail, malloc_tracer_setup_tail());

MallocTracer *MallocTracer::GetInstanceSlow(void) {
  pthread_once(&first_tracer_setup_once, MallocTracer::SetupFirstTracer);
  if (in_setup) {
    return get_first_tracer();
  }

  pthread_once(&setup_once, &MallocTracer::do_setup_tls);

  MallocTracer *an_instance;
  {
    SpinLockHolder h(&lock);
    uint64_t thread_id = !had_tracer ? ++thread_id_counter : 0;

    if (thread_id == 1) {
      an_instance = get_first_tracer();
    } else {
      an_instance = malloc_tracer_allocator.New();
      new (an_instance) MallocTracer(thread_id);
    }

    instance.ptr = an_instance;
    instance.next = all_tracers;
    instance.pprev = &all_tracers;

    if (instance.next) {
      instance.next->pprev = &instance.next;
    }
    all_tracers = &instance;
  }

  if (!had_tracer) {
    pthread_setspecific(instance_key, &instance);
  }

  return an_instance;
}

MallocTracer::MallocTracer(uint64_t _thread_id) {
  thread_id = _thread_id;
  counter = 0;
  prev_size = 0;
  prev_token = 0;
  buf_ptr = buf_storage;
  buf_end = buf_storage + sizeof(buf_storage) - 10;
  signal_saved_buf_ptr = buf_storage;
  destroy_count = 0;

  RefreshToken();
}

static void append_buf_locked(const char *buf, size_t size) {
  if (no_more_writes) {
    return;
  }
  tracer_buffer->AppendData(buf, size);
}

static inline uint64_t ts_and_cpu(bool from_saver) {
  unsigned cpu;
  uint64_t ts = __rdtscp(&cpu) & kTSMask;
  ts -= base_ts;
  return ts | cpu | ((int)from_saver << (kTSShift - 1));
}

void MallocTracer::RefreshBufferInnerLocked(uint64_t size, bool from_saver) {
  char meta_buf[32];
  char *p = meta_buf;
  EventsEncoder::triple enc =
    EventsEncoder::encode_buffer(thread_id, ts_and_cpu(from_saver), size);
  p = VarintCodec::encode_unsigned(p, enc.first);
  p = VarintCodec::encode_unsigned(p, enc.second.first);
  p = VarintCodec::encode_unsigned(p, enc.second.second);

  append_buf_locked(meta_buf, p - meta_buf);
  append_buf_locked(signal_saved_buf_ptr, size);
}

void MallocTracer::RefreshBuffer(int number, uint64_t one, uint64_t two) {
  SpinLockHolder h(&lock);

repeat:
  if (buf_ptr != signal_saved_buf_ptr) {
    RefreshBufferInnerLocked(buf_ptr - signal_saved_buf_ptr, false);
  }

  SetBufPtr(buf_storage);
  signal_saved_buf_ptr = buf_storage;

  switch (number) {
  case 0:
    return;
  case 2:
    SetBufPtr(VarintCodec::encode_unsigned(
                VarintCodec::encode_unsigned(buf_ptr, one), two));
    break;
  case 1:
    SetBufPtr(VarintCodec::encode_unsigned(buf_ptr, one));
    break;
  default:
    abort();
  }

  if (destroy_count) {
    number = 0;
    goto repeat;
  }
}

void MallocTracer::DumpFromSaverThread() {
  uint64_t s = signal_snapshot_buf_ptr - signal_saved_buf_ptr;

  if (s == 0) {
    return;
  }

  RefreshBufferInnerLocked(s, true);

  signal_saved_buf_ptr = signal_snapshot_buf_ptr;

  thread_dump_written += s;
}

void MallocTracer::RefreshTokenAndDec() {
  uint64_t base = __sync_add_and_fetch(&token_counter, kTokenSize);

  token_base = base;
  counter = kTokenSize;

  if (!HasSpaceFor(2)) {
    RefreshBuffer(0, 0, 0);
  }

  char *p = buf_ptr;

  EventsEncoder::pair enc =
    EventsEncoder::encode_token(base - kTokenSize, ts_and_cpu(false));

  p = VarintCodec::encode_unsigned(p, enc.first);
  p = VarintCodec::encode_unsigned(p, enc.second);

  SetBufPtr(p);
}

void MallocTracer::RefreshToken() {
  RefreshTokenAndDec();
  counter++;
}

static void process_wide_barrier() {
  // TODO: use membarrier or google-only rseq barrier
  // syscall
  static volatile char a_page[4096] __attribute__((aligned(4096)));
  // first touch page
  a_page[0] &= 0xff;
  // and then tell it to go away. This will trigger IPI to all cores
  // running this process' mm for tlb flush and wait for
  // completion. Causing memory barriers everywhere.
  madvise(const_cast<char*>(a_page), 4096, MADV_DONTNEED);
}

void MallocTracer::DumpEverything() {
  if (!tracer_buffer->IsFullySetup()) {
    return;
  }

  SpinLockHolder h(&lock);

  for (MallocTracer::Storage *s = all_tracers; s != NULL; s = s->next) {
    // benign race here, reading buf_ptr
    s->ptr->signal_snapshot_buf_ptr = *const_cast<char * volatile *>(&s->ptr->buf_ptr);
  }

  // ensure that we're able to see all the data written up to
  // signal_snapshot_buf_ptr of all threads
  process_wide_barrier();

  for (MallocTracer::Storage *s = all_tracers; s != NULL; s = s->next) {
    if (s->ptr->signal_snapshot_buf_ptr == s->ptr->signal_saved_buf_ptr) {
      continue;
    }
    s->ptr->DumpFromSaverThread();
  }

  char sync_end_buf[24];
  char *p = sync_end_buf;
  EventsEncoder::pair enc = EventsEncoder::encode_sync_barrier(ts_and_cpu(false));
  p = VarintCodec::encode_unsigned(p, enc.first);
  p = VarintCodec::encode_unsigned(p, enc.second);
  append_buf_locked(sync_end_buf, p - sync_end_buf);
}

void MallocTracer::ExcludeCurrentThreadDumping() {
  if (instance.ptr == NULL || instance.pprev == NULL) {
    return;
  }
  SpinLockHolder h(&lock);
  *instance.pprev = instance.next;
  instance.pprev = NULL;
}

MallocTracer::~MallocTracer() {
  RefreshBuffer(0, 0, 0);

  char *p = buf_ptr;
  EventsEncoder::pair enc = EventsEncoder::encode_death(thread_id, ts_and_cpu(false));
  p = VarintCodec::encode_unsigned(p, enc.first);
  p = VarintCodec::encode_unsigned(p, enc.second);

  {
    SpinLockHolder h(&lock);
    append_buf_locked(buf_storage, p - buf_storage);
  }
  memset(this, 0xfe, sizeof(*this));
}

static void finalize_buf() {
  // saving rest of trace may still malloc, particularly if saver
  // thread uses snappy. So we need to drop lock soon. But we drop all
  // further buffer writes.
  {
    SpinLockHolder h(&lock);
    no_more_writes = true;
  }

  char encoded_end[16];
  char *p = encoded_end;
  p = VarintCodec::encode_unsigned(p, EventsEncoder::encode_end());
  ASSERT(p <= encoded_end + sizeof(encoded_end));

  tracer_buffer->AppendData(encoded_end, p - encoded_end);
  tracer_buffer->Finalize();
}

REGISTER_MODULE_DESTRUCTOR(tracer_deinit, do {
  finalize_buf();
} while (0));

void MallocTracer::SPrintStats(char* start, char* end) {
  snprintf(start, end - start,
           "token_counter = %llu\n"
           "thread_id_counter = %llu\n"
           "thread_dump_written = %llu\n",
           (unsigned long long)token_counter,
           (unsigned long long)thread_id_counter,
           (unsigned long long)thread_dump_written);
}

} // namespace tcmalloc
