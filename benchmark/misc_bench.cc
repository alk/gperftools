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

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

#include "run_benchmark.h"

static void bench_syscall(long iterations, uintptr_t param) {
  while (--iterations > 0) {
    syscall(SYS_getpid);
  }
}

static void bench_page_stack_presence_msync(long iterations, uintptr_t param) {
  void *ptr = __builtin_frame_address(0);
  uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  addr &= ~uintptr_t{4095};
  addr -= 4096;
  while (--iterations > 0) {
    int rv = msync(reinterpret_cast<char *>(addr), 1, MS_ASYNC);
    if (rv != 0) {
      perror("msync");
      abort();
    }
  }
}

static void bench_page_stack_presence_pipe(long iterations, uintptr_t param) {
  static char pipe_buf[4096];
  static std::pair<int, int> fds = (+[]() {
    int fd[2];
    int rv = pipe2(fd, O_NONBLOCK);
    if (rv < 0) {
      perror("pipe2");
      abort();
    }
    return std::make_pair(fd[0], fd[1]);
  })();
  void *ptr = __builtin_frame_address(0);
  uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  addr &= ~uintptr_t{4095};
  addr -= 4096;
  while (--iterations > 0) {
    int rv;
    do {
      rv = write(fds.second, reinterpret_cast<char *>(addr), 1);
      if (rv > 0) {
        break;
      }
      if (errno == EAGAIN) {
        read(fds.first, pipe_buf, sizeof(pipe_buf));
      } else if (errno != EINTR) {
        perror("write");
        abort();
      }
    } while (true);
  }
}

static void bench_page_stack_presence_rr(long iterations, uintptr_t param) {
  static __attribute__((aligned(4096))) char remote_read_buf[4096];

  pid_t pid = getpid();

  void *ptr = __builtin_frame_address(0);
  uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  addr &= ~uintptr_t{4095};
  addr -= 4096;
  while (--iterations > 0) {
    iovec local[1];
    iovec remote[1];
    remote[0].iov_base = reinterpret_cast<char *>(addr);
    remote[0].iov_len = 4096;
    local[0].iov_base = remote_read_buf;
    local[0].iov_len = 4096;
    int rv = process_vm_readv(pid, local, 1, remote, 1, 0);
    if (rv < 0) {
      perror("process_vm_readv");
      abort();
    }
    asm volatile("" : : "r"(remote_read_buf[0]));
  }
}

static void bench_page_stack_presence_pageflags(long iterations,
                                                uintptr_t param) {
  static int fd = (+[]() {
    int fd = open("/proc/self/pagemap", O_CLOEXEC | O_RDONLY);
    if (fd < 0) {
      perror("open(\"/proc/self/pagemap\")");
      abort();
    }
    return fd;
  })();
  void *ptr = __builtin_frame_address(0);
  uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  addr &= ~uintptr_t{4095};
  addr -= 4096;
  while (--iterations > 0) {
    uint64_t buf[1];
    int rv = pread(fd, buf, 8, addr / 4096 * 8);
    if (rv != 8) {
      abort();
    }
    asm volatile("" : : "r"(buf[0]));
  }
}

int main(void) {
  report_benchmark("bench_syscall", bench_syscall, 0);
  report_benchmark("bench_page_stack_presence_msync",
                   bench_page_stack_presence_msync, 0);
  report_benchmark("bench_page_stack_presence_pipe",
                   bench_page_stack_presence_pipe, 0);
  report_benchmark("bench_page_stack_presence_rr", bench_page_stack_presence_rr,
                   0);
  report_benchmark("bench_page_stack_presence_pageflags",
                   bench_page_stack_presence_pageflags, 0);
}
