// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2017, gperftools Contributors
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

#include <sys/socket.h>
#include <netdb.h>
#include <sys/un.h>

#include "malloc_tracer.h"
#include "malloc_tracer_buf.h"

#include "base/googleinit.h"
#include "base/atomicops.h"

#define FD_BUF_SIZE (32 << 20)

static char fd_buf[2][FD_BUF_SIZE] __attribute__((aligned(4096)));
static int write_buf;
static int to_save_pos[2];

static int fd;

static void must_write_to_fd(const char* buf, int bytes) {
  if (fd < 0) {
    return;
  }
  do {
    int rv = write(fd, buf, bytes);
    if (rv < 0) {
      if (errno != EINTR) {
        perror("write");
        abort();
      }
      rv = 0;
    }
    bytes -= rv;
    buf += rv;
  } while (bytes > 0);
}

static sem_t space_sem[2];
static sem_t ready_sem[2];
static uint64_t total_saved;

static AtomicWord fully_setup;

static sem_t saver_thread_sem;

static void *saver_thread(void *__dummy) {
  MallocTracer::GetInstance();
  MallocTracer::ExcludeCurrentThreadDumping();
  sem_post(&saver_thread_sem);

  int bufno = 0;
  while (true) {
    sem_wait(ready_sem + bufno);
    int to_save = to_save_pos[bufno];
    if (to_save == 0) {
      break;
    }

    char *save_buf = fd_buf[bufno];

    // last buffer can be not full 4k. So just round up
    to_save = (to_save + 4095) & -4096;

    must_write_to_fd(save_buf, to_save);
    total_saved += to_save;
    sem_post(space_sem + bufno);
    bufno = (bufno + 1) % 2;
  }

  {
    char buf[4096] __attribute__((aligned(4096)));
    memset(buf, 0, sizeof(buf));
    char *p = buf;
    p += snprintf(p, buf + sizeof(buf) - p,
                  "total_saved = %llu\n", (unsigned long long)total_saved);
    MallocTracer::SPrintStats(p, buf + sizeof(buf));
    printf("%s", buf);
    must_write_to_fd(buf, sizeof(buf));
  }

  close(fd);

  sem_post(space_sem + bufno);
  sem_post(space_sem + (bufno + 1) % 2);

  return 0;
}

static void open_trace_output() {
  fd = -1;

  char *filename = getenv("TCMALLOC_TRACE_OUTPUT");
  if (filename == NULL) {
    return;
  }

  unsetenv("TCMALLOC_TRACE_OUTPUT");

  // O_NONBLOCK is for named pipes
  fd = open(filename, O_WRONLY|O_CREAT|O_NONBLOCK, 0644);
  if (fd < 0) {
    perror("open");
    fprintf(stderr, "will not save trace anywhere\n");
    return;
  }

  int flags = fcntl(fd, F_GETFL);
  if (flags < 0) {
    perror("fcntl(F_GETFL)");
    abort();
  }
  flags &= ~O_NONBLOCK;
  fcntl(fd, F_SETFL, flags);
  // O_DIRECT is good idea for regular files on disk
  fcntl(fd, F_SETFL, flags | O_DIRECT);

  // if output is pipe, we need larger buffer
  fcntl(fd, F_SETPIPE_SZ, 1 << 20);
}

static void do_setup_tail() {
  (void)TracerBuffer::GetInstance();

  open_trace_output();

  sem_init(&saver_thread_sem, 0, 0);
  pthread_t saver;
  int rv = pthread_create(&saver, 0, saver_thread, 0);
  if (rv != 0) {
    errno = rv;
    perror("pthread_create");
    abort();
  }
  sem_wait(&saver_thread_sem);

  base::subtle::Release_Store(&fully_setup, 1);
}

REGISTER_MODULE_INITIALIZER(setup_tail, do_setup_tail());

class ActualTracerBuffer : public TracerBuffer {
public:
  ActualTracerBuffer();

  virtual void Refresh();
  virtual void Finalize();
  virtual bool IsFullySetup();

  void SetBuffer(char *buffer, size_t used, size_t size) {
    start = buffer;
    current = buffer + used;
    limit = buffer + size;
  }

  void RefreshInternal(int to_write);

  char *start;

private:
  ~ActualTracerBuffer() {}
};

ActualTracerBuffer::ActualTracerBuffer() {
  start = NULL;
  SetBuffer(fd_buf[0], 0, FD_BUF_SIZE);
}

void ActualTracerBuffer::RefreshInternal(int to_write) {
  int tail = current - start - to_write;

  int next_buf = (write_buf + 1) % 2;
  sem_wait(space_sem + next_buf);

  memcpy(fd_buf[next_buf], fd_buf[write_buf] + to_write, tail);

  to_save_pos[write_buf] = to_write;
  sem_post(ready_sem + write_buf);

  write_buf = next_buf;
  SetBuffer(fd_buf[write_buf], tail, FD_BUF_SIZE);
}

void ActualTracerBuffer::Refresh() {
  RefreshInternal((current - start) & -4096);
}

void ActualTracerBuffer::Finalize() {
  RefreshInternal(current - start);
  // signal saver thread that we're done
  RefreshInternal(0);

  // and wait until it is done
  sem_wait(space_sem + (write_buf + 1) % 2);
  sem_wait(space_sem + (write_buf + 2) % 2);
}

bool ActualTracerBuffer::IsFullySetup() {
  return base::subtle::Acquire_Load(&fully_setup);
}

TracerBuffer::~TracerBuffer() {}

const int TracerBuffer::kMinSizeAfterRefresh;

TracerBuffer* TracerBuffer::GetInstance() {
  static union {
    void *dummy;
    double dummy2;
    char bytes[sizeof(ActualTracerBuffer)];
  } space;
  static int initialized;

  if (!initialized) {
    sem_init(space_sem + 0, 0, 1);
    sem_init(space_sem + 1, 0, 1);
    sem_init(ready_sem + 0, 0, 0);
    sem_init(ready_sem + 1, 0, 0);

    new (&space) ActualTracerBuffer();
    initialized = true;
  }

  return reinterpret_cast<ActualTracerBuffer*>(&space);
}
