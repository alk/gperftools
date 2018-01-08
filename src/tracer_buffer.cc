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
#include "config.h"

#include "tracer_buffer.h"

#include <errno.h>
#include <fcntl.h>
#if USE_LZ4
#include <lz4frame.h>
#endif
#include <netdb.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "base/atomicops.h"
#include "base/googleinit.h"
#include "internal_logging.h"
#include "malloc_tracer.h"

namespace tcmalloc {

static const int kBufsCount = 8;
static const int kOneBufSize = (64 << 20)/kBufsCount;

// Buffer lifecycle is as follows:
//
//  a) space_sem = 0, ready_sem = 0: buffer is actively being written
//  to by TracerBuffer. write_buf has index of buffer
//
//  b) space_sem = 0, ready_sem = 1: buffer is ready to be saved
//  to_save_pos represents amount of data in buffer. Which is multiple
//  of page size (except for very last data).
//
//  c) space_sem = 0, ready_sem = 0: once saver sem_wait's ready_sem
//  it is saving that buffer.
//
//  d) space_sem = 1, ready_sem = 0: once saver is done, it posts to
//  space_sem, which represents that the buffer is ready to be filled
//  again.
//
static char fd_buf[kBufsCount][kOneBufSize] __attribute__((aligned(4096)));
static int write_buf;
static int to_save_pos[kBufsCount];
static sem_t space_sem[kBufsCount];
static sem_t ready_sem[kBufsCount];

static uint64_t total_saved;

static AtomicWord fully_setup;

class Writer {
public:
  virtual ~Writer() {}

  virtual void Write(const char* data, size_t amount) = 0;
  virtual void WriteLast(const char* data, size_t amount) = 0;
};

class DirectWriter : public Writer {
public:
  DirectWriter(int fd) : fd_(fd) {}
  ~DirectWriter() {}

  void Write(const char* buf, size_t bytes) {
    if (fd_ < 0) {
      return;
    }
    do {
      int rv = write(fd_, buf, bytes);
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
  void WriteLast(const char* data, size_t amount) {
    Write(data, (amount+4095) & ~size_t(4095));
    close(fd_);
  }

private:
  int fd_;
};

#if USE_LZ4

class LZ4Compressor : public Writer {
public:
  LZ4Compressor(Writer* slave);
  ~LZ4Compressor();

  void Write(const char* data, size_t amount);
  void WriteLast(const char* data, size_t amount);

private:
  char buffer_[4<<20] __attribute__((aligned(4096)));
  static const int kMinAmountToSave = 3 << 20;
  static const int kBlockSize = 16 << 10;
  int buf_tail_;
  LZ4F_cctx* lzctx_;
  Writer* const slave_;
};

const int LZ4Compressor::kBlockSize;
const int LZ4Compressor::kMinAmountToSave;

LZ4Compressor::LZ4Compressor(Writer* slave) : buf_tail_(0), slave_(slave) {
  LZ4F_errorCode_t err = LZ4F_createCompressionContext(&lzctx_, LZ4F_VERSION);
  if (err != 0) {
    abort();
  }

  LZ4F_preferences_t prefs;
  memset(&prefs, 0, sizeof(prefs));
  prefs.frameInfo.blockMode = LZ4F_blockIndependent;

  buf_tail_ = LZ4F_compressBegin(lzctx_, buffer_, sizeof(buffer_), &prefs);
}

LZ4Compressor::~LZ4Compressor() {
  LZ4F_freeCompressionContext(lzctx_);
}

void LZ4Compressor::Write(const char* data, size_t amount) {
  while (amount > 0) {
    size_t this_write = kBlockSize;
    if (this_write > amount) {
      this_write = amount;
    }
    size_t written = LZ4F_compressUpdate(lzctx_,
                                         buffer_ + buf_tail_,
                                         sizeof(buffer_) - buf_tail_,
                                         data, this_write,
                                         NULL);
    if (LZ4F_isError(written)) {
      abort();
    }

    buf_tail_ += written;
    data += this_write;
    amount -= this_write;

    if (buf_tail_ >= kMinAmountToSave) {
      int to_save = buf_tail_ & ~4095;
      slave_->Write(buffer_, to_save);
      buf_tail_ -= to_save;
      memcpy(buffer_, buffer_ + to_save, buf_tail_);
    }
  }
}

void LZ4Compressor::WriteLast(const char* data, size_t amount) {
  Write(data, amount);

  size_t written = LZ4F_compressEnd(lzctx_, buffer_ + buf_tail_,
                                    sizeof(buffer_) - buf_tail_, NULL);
  if (LZ4F_isError(written)) {
    abort();
  }
  buf_tail_ += written;

  slave_->WriteLast(buffer_, buf_tail_);
}

#endif // USE_LZ4

static sem_t saver_thread_sem;

static void *saver_thread(void *_arg) {
  MallocTracer::ExcludeCurrentThreadFromDumping();
  sem_post(&saver_thread_sem);

  Writer* writer = reinterpret_cast<Writer*>(_arg);

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

    if (writer) {
      writer->Write(save_buf, to_save);
    }
    total_saved += to_save;
    sem_post(space_sem + bufno);
    bufno = (bufno + 1) % kBufsCount;
  }

  if (writer) {
    writer->WriteLast(NULL, 0);
  }

  {
    char buf[4096];
    char *p = buf;
    p += snprintf(p, buf + sizeof(buf) - p,
                  "total_saved = %llu\n", (unsigned long long)total_saved);
    MallocTracer::SPrintStats(p, buf + sizeof(buf));
    fprintf(stderr, "%s", buf);
  }

  // we've hit to_save = 0 case above and exited loop without
  // signalling buffer emptying. We do it here, signalling that saver
  // is fully done.
  sem_post(space_sem + bufno);

  return 0;
}

static Writer* open_trace_output() {
  int fd = -1;

  char *filename = getenv("TCMALLOC_TRACE_OUTPUT");
  if (filename == NULL) {
    return NULL;
  }

  unsetenv("TCMALLOC_TRACE_OUTPUT");

  // O_NONBLOCK is for named pipes
  fd = open(filename, O_WRONLY|O_CREAT|O_NONBLOCK|O_TRUNC, 0644);
  if (fd < 0) {
    perror("open");
    fprintf(stderr, "will not save trace anywhere\n");
    return NULL;
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

  Writer* direct_writer = new DirectWriter(fd);

#if USE_LZ4
  if (getenv("TCMALLOC_TRACE_UNCOMPRESSED")) {
    return direct_writer;
  }
  return new LZ4Compressor(direct_writer);

#else
  return direct_writer;
#endif
}

static void do_setup_tail() {
  (void)TracerBuffer::GetInstance();

  Writer* writer = open_trace_output();

  sem_init(&saver_thread_sem, 0, 0);
  pthread_t saver;
  int rv = pthread_create(&saver, 0, saver_thread, writer);
  if (rv != 0) {
    errno = rv;
    perror("pthread_create");
    abort();
  }
  sem_wait(&saver_thread_sem);

  base::subtle::Release_Store(&fully_setup, 1);
}

REGISTER_MODULE_INITIALIZER(setup_buf_tail, do_setup_tail());

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
  sem_wait(space_sem + 0);
  SetBuffer(fd_buf[0], 0, kOneBufSize);
}

void ActualTracerBuffer::RefreshInternal(int to_write) {
  CHECK_CONDITION(fd_buf[write_buf] == start);

  int tail = current - start - to_write;

  int next_buf = (write_buf + 1) % kBufsCount;
  sem_wait(space_sem + next_buf);

  memcpy(fd_buf[next_buf], fd_buf[write_buf] + to_write, tail);

  to_save_pos[write_buf] = to_write;
  sem_post(ready_sem + write_buf);

  write_buf = next_buf;
  SetBuffer(fd_buf[write_buf], tail, kOneBufSize);
}

void ActualTracerBuffer::Refresh() {
  RefreshInternal((current - start) & -4096);
}

void ActualTracerBuffer::Finalize() {
  RefreshInternal(current - start);
  // signal saver thread that we're done
  RefreshInternal(0);

  // and wait until it is done. We're just taking "ownership" of all
  // buffers. Once we're done, we can be sure that saver cannot be
  // running and that our last 2 buffers we passed to saver got
  // through.
  for (int i = 1; i < kBufsCount; i++) {
    sem_wait(space_sem + (write_buf + i) % kBufsCount);
  }
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
    for (int i = 0; i < kBufsCount; i++) {
      sem_init(space_sem + i, 0, 1);
      sem_init(ready_sem + i, 0, 0);
    }

    new (&space) ActualTracerBuffer();
    initialized = true;
  }

  return reinterpret_cast<ActualTracerBuffer*>(&space);
}

} // namespace tcmalloc
