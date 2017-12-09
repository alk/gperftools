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
#include <signal.h>
#include <x86intrin.h>

#include <sys/socket.h>
#include <netdb.h>
#include <sys/un.h>

// #undef USE_SNAPPY

#ifdef USE_SNAPPY
#include <snappy-c.h>
#endif

#include "page_heap_allocator.h"
#include "base/spinlock.h"
#include "base/googleinit.h"
#include "malloc_tracer.h"

static const int kDumperPeriodMicros = 3000;

static SpinLock lock(base::LINKER_INITIALIZED);
static SpinLock signal_lock(base::LINKER_INITIALIZED);

static const int dump_signal = std::max(std::min(0x35, SIGRTMAX), SIGRTMIN);

static int fd;

static const int kTokenSize = 4 << 10;
static const int kTSShift = 10;
static const uint64_t kTSMask = ~((1ULL << kTSShift) - 1);

static uint64_t token_counter;
static uint64_t thread_id_counter;

static uint64_t base_ts;

#define FD_BUF_SIZE (32 << 20)

static char fd_buf[2][FD_BUF_SIZE] __attribute__((aligned(4096)));
static int write_buf;
static int to_save_pos[2];

static int fd_buf_pos;

static sem_t space_sem[2];
static sem_t ready_sem[2];

__thread MallocTracer::Storage MallocTracer::instance ATTR_INITIAL_EXEC;
__thread bool had_tracer;

MallocTracer::Storage *MallocTracer::all_tracers;

static pthread_key_t instance_key;
static pthread_once_t setup_once = PTHREAD_ONCE_INIT;
static pthread_once_t first_tracer_setup_once = PTHREAD_ONCE_INIT;

static tcmalloc::PageHeapAllocator<MallocTracer> malloc_tracer_allocator;

static sem_t signal_completions;
static bool fully_setup;
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
  MallocTracer::Storage *instanceptr =
    reinterpret_cast<MallocTracer::Storage *>(arg);

  MallocTracer *tracer = instanceptr->ptr;

  // have pthread call us again on next destruction iteration and give
  // rest of tls destructors chance to get traced properly
  if (tracer->destroy_count++ < 1) {
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

static int unix_open(const char *path) {
  struct sockaddr_un us;
  us.sun_family = AF_UNIX;
  memset(us.sun_path, 0, sizeof(us.sun_path));
  strncpy(us.sun_path, path, sizeof(us.sun_path) - 1);

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    abort();
  }
  int rv = connect(fd, reinterpret_cast<struct sockaddr *>(&us), sizeof(us));
  if (rv < 0) {
    perror("connect");
    abort();
  }
  int optval = 1024*1024;
  rv = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &optval, sizeof(optval));
  if (rv < 0) {
    perror("setsockopt");
    abort();
  }
  return fd;
}

static int tcp_open(const char *path) {
  const char *colon = strchr(path, ':');
  if (!colon) {
    errno = EINVAL;
    return -1;
  }
  char *addr = (char *)calloc(colon - path + 1, 1);
  memcpy(addr, path, colon - path);

  struct addrinfo *p;
  struct addrinfo hints;

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC; /* Allow IPv4 or IPv6 */
  hints.ai_socktype = SOCK_STREAM;

  int rv = getaddrinfo(addr, colon + 1, NULL, &p);
  if (rv != 0) {
    printf("getaddrinfo:%s\n", gai_strerror(rv));
    abort();
  }

  int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
  if (fd < 0) {
    perror("socket");
    abort();
  }

  rv = connect(fd, p->ai_addr, p->ai_addrlen);
  if (rv < 0) {
    perror("connect");
    abort();
  }

  freeaddrinfo(p);
  free(p);

  int val = 4 << 20;
  rv = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val));
  if (rv < 0) {
    perror("setsockopt");
    abort();
  }
  return fd;
}

static uint64_t total_saved;
static uint64_t total_written;
static uint64_t thread_dump_written;

#ifdef USE_SNAPPY
static char snappy_buf[FD_BUF_SIZE*2] __attribute__((aligned(4096)));
static int snappy_tail;
static unsigned char snappy_header[] = {0xff, 0x06, 0x00, 0x00, 0x73, 0x4e, 0x61, 0x50, 0x70, 0x59};

extern "C" void tcmalloc_tracing_crc32c_init(void);
extern "C" uint32_t tcmalloc_tracing_crc32c(uint32_t crc, const void *buf, size_t len);

#endif

static sem_t saver_thread_sem;

static void *saver_thread(void *__dummy) {
  MallocTracer::GetInstance();
  MallocTracer::ExcludeCurrentThreadDumping();
  sem_post(&saver_thread_sem);

#ifdef USE_SNAPPY
  memcpy(snappy_buf, snappy_header, sizeof(snappy_header));
  snappy_tail = sizeof(snappy_header);
  tcmalloc_tracing_crc32c_init();
#endif

  int bufno = 0;
  while (true) {
    sem_wait(ready_sem + bufno);
    int to_save = to_save_pos[bufno];
    if (to_save == 0) {
      break;
    }
    // printf("saving %d for %lld\n", bufno, (long long)to_save);

    char *save_buf = fd_buf[bufno];

#ifdef USE_SNAPPY
    uint32_t crc = tcmalloc_tracing_crc32c(0, save_buf, to_save);
    crc = ((crc >> 15) | (crc << 17)) + 0xa282ead8;

    char *len_ptr = snappy_buf + snappy_tail;
    snappy_tail += 4;

    memcpy(snappy_buf + snappy_tail, &crc, sizeof(crc));
    snappy_tail += 4;

    size_t compressed_size = sizeof(snappy_buf) - snappy_tail;
    snappy_status st = snappy_compress(save_buf, to_save,
                                       snappy_buf + snappy_tail,
                                       &compressed_size);
    if (st != SNAPPY_OK) {
      printf("st = %d\n", st);
      abort();
    }

    // least significant byte of 0 means it is compressed block
    uint32_t len = (compressed_size + 4) << 8;
    memcpy(len_ptr, &len, sizeof(len));

    to_save = snappy_tail + compressed_size;
    save_buf = snappy_buf;

    snappy_tail = to_save & 4095;
    to_save &= -4096;
#else
    // last buffer can be not full 4k. So just round up
    to_save = (to_save + 4095) & -4096;
#endif

    int rv = write(fd, save_buf, to_save);
    if (rv < 0) {
      perror("write");
      abort();
    }
    if (rv != to_save) {
      abort();
    }
#ifdef USE_SNAPPY
    if (to_save != 0) {
      memcpy(snappy_buf, snappy_buf + to_save, snappy_tail);
    }
#endif
    total_saved += to_save;
    sem_post(space_sem + bufno);
    bufno = (bufno + 1) % 2;
  }

#ifdef USE_SNAPPY
  if (snappy_tail) {
    int new_tail = (snappy_tail + 4 + 4095) & -4096;
    memset(snappy_buf + snappy_tail, 0, new_tail - snappy_tail);
    uint32_t padding_len = new_tail - snappy_tail;
    padding_len <<= 8;
    padding_len |= 0xfe;
    memcpy(snappy_buf + snappy_tail, &padding_len, sizeof(padding_len));

    int rv = write(fd, snappy_buf, new_tail);
    if (rv < 0) {
      perror("write");
      abort();
    }
    total_saved += new_tail;
  }
#endif

  {
    char buf[4096] __attribute__((aligned(4096)));
    memset(buf, 0, sizeof(buf));
    char *p = buf;
    p += snprintf(p, buf + sizeof(buf) - p,
                  "total_written = %llu\n", (unsigned long long)total_written);
    p += snprintf(p, buf + sizeof(buf) - p,
                  "total_saved = %llu (%g%%)\n", (unsigned long long)total_saved,
             100.0 * total_saved / total_written);
    p += snprintf(p, buf + sizeof(buf) - p,
                  "token_counter = %llu\n", (unsigned long long)token_counter);
    p += snprintf(p, buf + sizeof(buf) - p,
                  "thread_id_counter = %llu\n", (unsigned long long)thread_id_counter);
    p += snprintf(p, buf + sizeof(buf) - p,
                  "thread_dump_written = %llu\n", (unsigned long long)thread_dump_written);
    printf("%s", buf);
    int rv = write(fd, buf, sizeof(buf));
    if (rv < 0) {
      perror("write");
      abort();
    }
  }

  close(fd);

  sem_post(space_sem + bufno);
  sem_post(space_sem + (bufno + 1) % 2);

  return 0;
}

void MallocTracer::SetupFirstTracer() {
  base_ts = __rdtsc() & kTSMask;
  new (get_first_tracer()) MallocTracer(0);
}

// guards cases of malloc calls during do_setup
static __thread bool in_setup ATTR_INITIAL_EXEC;

void MallocTracer::do_setup_tls() {
  in_setup = true;

  malloc_tracer_allocator.Init();
  int rv = pthread_key_create(&instance_key, &MallocTracer::malloc_tracer_destructor);
  if (rv) {
    // TODO
    abort();
  }

  sem_init(space_sem + 0, 0, 1);
  sem_init(space_sem + 1, 0, 1);
  sem_init(ready_sem + 0, 0, 0);
  sem_init(ready_sem + 1, 0, 0);

  in_setup = false;
}

void dump_signal_handler(int sig) {
  int saved_errno = errno;

  {
    SpinLockHolder h(&signal_lock);
    MallocTracer::instance.ptr->DumpFromSignalLocked();
  }
  sem_post(&signal_completions);

  errno = saved_errno;
}

static void *dumper_thread(void *__dummy) {
  while (true) {
    usleep(kDumperPeriodMicros);
    MallocTracer::DumpEverything();
  }
}

static void do_setup_tail() {
  int rv;

  {
    char namebuf[1024];
    snprintf(namebuf, sizeof(namebuf)-1, "/tmp/mtrace-%d-%llu",
             (int)getpid(), (unsigned long long)time(0));
    fd = open(namebuf, O_WRONLY|O_DIRECT|O_CREAT, 0644);
  }

  // fd = open("outpipe", O_WRONLY, 0644);
  // fd = open("output", O_WRONLY|O_DIRECT|O_CREAT, 0644);
  // fd = open("/dev/null", O_WRONLY, 0777);
  // fd = unix_open("/tmp/tmpsock");
  // fd = tcp_open("192.168.1.164:1024");
  if (fd < 0) {
    perror("open");
    abort();
  }

  sem_init(&saver_thread_sem, 0, 0);
  pthread_t saver;
  rv = pthread_create(&saver, 0, saver_thread, 0);
  if (rv != 0) {
    errno = rv;
    perror("pthread_create");
    abort();
  }
  sem_wait(&saver_thread_sem);

  sem_init(&signal_completions, 0, 0);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = dump_signal_handler;
  sa.sa_flags = SA_RESTART;
  rv = sigaction(dump_signal, &sa, NULL);
  if (rv != 0) {
    perror("sigaction");
    printf("min = %x, max = %x\n", SIGRTMIN, SIGRTMAX);
    abort();
  }

  pthread_t dumper;
  rv = pthread_create(&dumper, 0, dumper_thread, 0);
  if (rv != 0) {
    errno = rv;
    perror("pthread_create");
    abort();
  }

  SpinLockHolder h(&lock);
  fully_setup = true;
}

REGISTER_MODULE_INITIALIZER(setup_tail, do_setup_tail());

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

    instance.t = pthread_self();
    instance.ptr = an_instance;
    instance.next = NULL;
    instance.pprev = NULL;

    if (!had_tracer) {
      instance.pprev = &all_tracers;
      instance.next = all_tracers;

      if (instance.next) {
        instance.next->pprev = &instance.next;
      }
      all_tracers = &instance;
    }
  }

  pthread_setspecific(instance_key, &instance);

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

static void save_buf_internal(int);

static void save_buf() {
  int to_write = fd_buf_pos & -4096;

  if (to_write == 0) {
    abort();
  }
  save_buf_internal(to_write);
}

static void save_buf_internal(int to_write) {
  int next_buf = (write_buf + 1) % 2;
  // printf("waiting space in %d\n", next_buf);
  sem_wait(space_sem + next_buf);
  memcpy(fd_buf[next_buf], fd_buf[write_buf] + to_write, fd_buf_pos - to_write);

  fd_buf_pos -= to_write;
  total_written += to_write;

  to_save_pos[write_buf] = to_write;
  sem_post(ready_sem + write_buf);
  // printf("posted readiness in %d\n", write_buf);
  write_buf = next_buf;
}

static void append_buf_locked(const char *buf, size_t size) {
  if (no_more_writes) {
    return;
  }
  if (fd_buf_pos + size > sizeof(fd_buf[0])) {
    save_buf();
  }
  memcpy(fd_buf[write_buf] + fd_buf_pos, buf, size);
  fd_buf_pos += size;
}

static void append_buf(const char *buf, size_t size) {
  SpinLockHolder h(&lock);
  append_buf_locked(buf, size);
}

static inline uint64_t ts_and_cpu() {
  unsigned cpu;
  uint64_t ts = __rdtscp(&cpu) & kTSMask;
  ts -= base_ts;
  return ts | cpu;
}

void MallocTracer::RefreshBufferInnerLocked(uint64_t size) {
  char meta_buf[32];
  char *p = meta_buf;
  EventsEncoder::triple enc =
    EventsEncoder::encode_buffer(thread_id, ts_and_cpu(), size);
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
    RefreshBufferInnerLocked(buf_ptr - signal_saved_buf_ptr);
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

void MallocTracer::DumpFromSignalLocked() {
  uint64_t s = buf_ptr - signal_saved_buf_ptr;

  if (s == 0) {
    return;
  }

  RefreshBufferInnerLocked(s);

  signal_saved_buf_ptr = buf_ptr;

  thread_dump_written += s;
}

void MallocTracer::RefreshTokenAndDec() {
  uint64_t base = __sync_add_and_fetch(&token_counter, kTokenSize);

  token_base = base;
  counter = kTokenSize;

  // TODO: replace with has_size_for helper (note that buf_end has
  // margin for 10 bytes already)
  if (buf_ptr + 20 >= buf_end) {
    RefreshBuffer(0, 0, 0);
  }

  char *p = buf_ptr;

  EventsEncoder::triple enc =
    EventsEncoder::encode_token(thread_id, ts_and_cpu(), base - kTokenSize);

  p = VarintCodec::encode_unsigned(p, enc.first);
  p = VarintCodec::encode_unsigned(p, enc.second.first);
  p = VarintCodec::encode_unsigned(p, enc.second.second);

  SetBufPtr(p);
}

void MallocTracer::RefreshToken() {
  RefreshTokenAndDec();
  counter++;
}

void MallocTracer::DumpEverything() {
  SpinLockHolder h(&lock);
  if (!fully_setup) {
    return;
  }

  if (instance.ptr) {
    assert(!signal_lock.IsHeld());

    assert(instance.t == pthread_self());
    instance.ptr->DumpFromSignalLocked();
  }

  int signalled = 0;
  for (MallocTracer::Storage *s = all_tracers; s != NULL; s = s->next) {
    if (s == &instance) {
      continue;
    }
    // benign race here, reading buf_ptr
    if (s->ptr->buf_ptr == s->ptr->signal_saved_buf_ptr) {
      continue;
    }
    signalled++;
    int rv = pthread_kill(s->t, dump_signal);
    if (rv != 0) {
      errno = rv;
      perror("pthread_kill");
      abort();
    }
  }

  // yes we need to hold the lock across all signal handler
  // executions. Because signal handlers assume they have the lock
  for (; signalled > 0; signalled--) {
    sem_wait(&signal_completions);
  }

#ifndef NDEBUG
  int val = 0;
  sem_getvalue(&signal_completions, &val);
  assert(val == 0);
#endif
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
  EventsEncoder::pair enc = EventsEncoder::encode_death(thread_id, ts_and_cpu());
  p = VarintCodec::encode_unsigned(p, enc.first);
  SetBufPtr(VarintCodec::encode_unsigned(p, enc.second));

  append_buf(buf_storage, buf_ptr - buf_storage);
}

static void finalize_buf() {
  // saving rest of trace may still malloc, particularly if saver
  // thread uses snappy. So we need to drop look soon. But we drop all
  // further buffer writes.
  {
    SpinLockHolder h(&lock);
    no_more_writes = true;
  }

  if (fd_buf_pos >= 4096) {
    save_buf();
  }

  char *p = fd_buf[write_buf] + fd_buf_pos;
  p = VarintCodec::encode_unsigned(p, EventsEncoder::encode_end());
  fd_buf_pos = (p - fd_buf[write_buf]);
  // fd_buf_pos = (p - fd_buf[write_buf] + 4095) & -4096;

  save_buf_internal(fd_buf_pos);

  if (fd_buf_pos != 0) {
    abort();
  }

  // signal saver thread that we're done
  save_buf_internal(0);

  // and wait until it is done
  sem_wait(space_sem + (write_buf + 1) % 2);
  sem_wait(space_sem + (write_buf + 2) % 2);
}

REGISTER_MODULE_DESTRUCTOR(tracer_deinit, do {
  finalize_buf();
} while (0));
