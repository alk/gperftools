// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
/* Copyright (c) 2007, Google Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ---
 * Author: Craig Silverstein
 */

#ifndef _WIN32
# error You should only be including windows/port.cc in a windows environment!
#endif

#define NOMINMAX       // so std::max, below, compiles correctly
#include <config.h>
#include <string.h>    // for strlen(), memset(), memcmp()
#include <assert.h>
#include <stdarg.h>    // for va_list, va_start, va_end
#include <windows.h>
#include "port.h"
#include "base/logging.h"
#include "base/spinlock.h"
#include "internal_logging.h"
#include "system-alloc.h"
#include "common.h"

// -----------------------------------------------------------------------
// Basic libraries

int getpagesize() {
  static int pagesize = 0;
  if (pagesize == 0) {
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    pagesize = std::max(system_info.dwPageSize,
                        system_info.dwAllocationGranularity);
  }
  return pagesize;
}

extern "C" PERFTOOLS_DLL_DECL void* __sbrk(ptrdiff_t increment) {
  LOG(FATAL, "Windows doesn't implement sbrk!\n");
  return NULL;
}

// We need to write to 'stderr' without having windows allocate memory.
// The safest way is via a low-level call like WriteConsoleA().  But
// even then we need to be sure to print in small bursts so as to not
// require memory allocation.
extern "C" PERFTOOLS_DLL_DECL void WriteToStderr(const char* buf, int len) {
  // Looks like windows allocates for writes of >80 bytes
  for (int i = 0; i < len; i += 80) {
    write(STDERR_FILENO, buf + i, std::min(80, len - i));
  }
}


// -----------------------------------------------------------------------
// Threads code

// Declared (not extern "C") in thread_cache.h
bool CheckIfKernelSupportsTLS() {
  // TODO(csilvers): return true (all win's since win95, at least, support this)
  return false;
}

// Windows doesn't support pthread_key_create's destr_function, and in
// fact it's a bit tricky to get code to run when a thread exits.  This
// is cargo-cult magic from http://www.codeproject.com/threads/tls.asp.
// This code is for VC++ 7.1 and later; VC++ 6.0 support is possible
// but more busy-work -- see the webpage for how to do it.  If all
// this fails, we could use DllMain instead.  The big problem with
// DllMain is it doesn't run if this code is statically linked into a
// binary (it also doesn't run if the thread is terminated via
// TerminateThread, which if we're lucky this routine does).

// Force a reference to _tls_used to make the linker create the TLS directory
// if it's not already there (that is, even if __declspec(thread) is not used).
// Force a reference to p_thread_callback_tcmalloc and p_process_term_tcmalloc
// to prevent whole program optimization from discarding the variables.
#ifdef _MSC_VER
#if defined(_M_IX86)
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_p_thread_callback_tcmalloc")
#pragma comment(linker, "/INCLUDE:_p_process_term_tcmalloc")
#elif defined(_M_X64)
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:p_thread_callback_tcmalloc")
#pragma comment(linker, "/INCLUDE:p_process_term_tcmalloc")
#endif
#endif

// When destr_fn eventually runs, it's supposed to take as its
// argument the tls-value associated with key that pthread_key_create
// creates.  (Yeah, it sounds confusing but it's really not.)  We
// store the destr_fn/key pair in this data structure.  Because we
// store this in a single var, this implies we can only have one
// destr_fn in a program!  That's enough in practice.  If asserts
// trigger because we end up needing more, we'll have to turn this
// into an array.
struct DestrFnClosure {
  void (*destr_fn)(void*);
  pthread_key_t key_for_destr_fn_arg;
};

static DestrFnClosure destr_fn_info;   // initted to all NULL/0.

static int on_process_term(void) {
  if (destr_fn_info.destr_fn) {
    void *ptr = TlsGetValue(destr_fn_info.key_for_destr_fn_arg);
    // This shouldn't be necessary, but in Release mode, Windows
    // sometimes trashes the pointer in the TLS slot, so we need to
    // remove the pointer from the TLS slot before the thread dies.
    TlsSetValue(destr_fn_info.key_for_destr_fn_arg, NULL);
    if (ptr)  // pthread semantics say not to call if ptr is NULL
      (*destr_fn_info.destr_fn)(ptr);
  }
  return 0;
}

static void NTAPI on_tls_callback(HINSTANCE h, DWORD dwReason, PVOID pv) {
  if (dwReason == DLL_THREAD_DETACH) {   // thread is being destroyed!
    on_process_term();
  }
}

#ifdef _MSC_VER

// extern "C" suppresses C++ name mangling so we know the symbol names
// for the linker /INCLUDE:symbol pragmas above.
extern "C" {
// This tells the linker to run these functions.
#pragma data_seg(push, old_seg)
#pragma data_seg(".CRT$XLB")
void (NTAPI *p_thread_callback_tcmalloc)(
    HINSTANCE h, DWORD dwReason, PVOID pv) = on_tls_callback;
#pragma data_seg(".CRT$XTU")
int (*p_process_term_tcmalloc)(void) = on_process_term;
#pragma data_seg(pop, old_seg)
}  // extern "C"

#else  // #ifdef _MSC_VER  [probably msys/mingw]

// We have to try the DllMain solution here, because we can't use the
// msvc-specific pragmas.
BOOL WINAPI DllMain(HINSTANCE h, DWORD dwReason, PVOID pv) {
  if (dwReason == DLL_THREAD_DETACH)
    on_tls_callback(h, dwReason, pv);
  else if (dwReason == DLL_PROCESS_DETACH)
    on_process_term();
  return TRUE;
}

#endif  // #ifdef _MSC_VER

extern "C" pthread_key_t PthreadKeyCreate(void (*destr_fn)(void*)) {
  // Semantics are: we create a new key, and then promise to call
  // destr_fn with TlsGetValue(key) when the thread is destroyed
  // (as long as TlsGetValue(key) is not NULL).
  pthread_key_t key = TlsAlloc();
  if (destr_fn) {   // register it
    // If this assert fails, we'll need to support an array of destr_fn_infos
    assert(destr_fn_info.destr_fn == NULL);
    destr_fn_info.destr_fn = destr_fn;
    destr_fn_info.key_for_destr_fn_arg = key;
  }
  return key;
}

// NOTE: this is Win2K and later.  For Win98 we could use a CRITICAL_SECTION...
extern "C" int perftools_pthread_once(pthread_once_t *once_control,
                                      void (*init_routine)(void)) {
  // Try for a fast path first. Note: this should be an acquire semantics read.
  // It is on x86 and x64, where Windows runs.
  if (*once_control != 1) {
    while (true) {
      switch (InterlockedCompareExchange(once_control, 2, 0)) {
        case 0:
          init_routine();
          InterlockedExchange(once_control, 1);
          return 0;
        case 1:
          // The initializer has already been executed
          return 0;
        default:
          // The initializer is being processed by another thread
          SwitchToThread();
      }
    }
  }
  return 0;
}


// -----------------------------------------------------------------------
// These functions replace system-alloc.cc

// The current system allocator declaration (unused here)
SysAllocator* sys_alloc = NULL;
// Number of bytes taken from system.
size_t TCMalloc_SystemTaken = 0;

static void *AllocDirectly(size_t size, size_t *actual_size,
                           size_t alignment) {
  size_t pagesize = static_cast<size_t>(getpagesize());

  size_t reserve_size = size + pagesize;
  if (alignment > pagesize) {
    reserve_size += alignment;
  }
  reserve_size = tcmalloc::AlignUp(reserve_size, pagesize);
  size = tcmalloc::AlignUp(size, pagesize);
  void *alloc = VirtualAlloc(0, reserve_size, MEM_RESERVE, PAGE_READWRITE);
  if (alloc == NULL) {
    return alloc;
  }
  uintptr_t place = reinterpret_cast<uintptr_t>(alloc);
  uintptr_t aligned_place = tcmalloc::AlignUp(place, static_cast<uintptr_t>(alignment));
  assert(aligned_place + size <= place + reserve_size - pagesize);

  void *rv = VirtualAlloc(reinterpret_cast<void *>(aligned_place),
                          size, MEM_COMMIT, PAGE_READWRITE);
  if (rv == NULL) {
    VirtualFree(alloc, reserve_size, MEM_RELEASE);
    return NULL;
  }

  if (actual_size) {
    *actual_size = size;
  }
  return rv;
}

// we're MEM_RESERVE-ing memory from OS in chunks of this size.
// we assume it's multiple of getpagesize()
static const size_t kVMReserveChunkSize = 128*1024*1024;

// allocations larger than this size will be VirtualAlloc-ed directly
// rather than being MEM_COMMIT-ed from
// [alloc_reserved_at,alloc_reserved_at+alloc_reserved_size) region as
// usual
//
// NOTE: that this is smaller than kMetadataAllocChunkSize in
// common.cc. That causes additional goodness of separating metadata
// and data allocations. Otherwise chunks metadata allocations between
// data allocations could prevent us from coalescing some free page
// spans.
static const size_t kVMReserveTooBigWaste = kVMReserveChunkSize / 32;

// this is start of MEM_RESERVE-ed area that we're going to return in
// TCMalloc_SystemAlloc
static char *alloc_reserved_at;
// and that's size of this area
static size_t alloc_reserved_size;
// and that's spinlock that guards memory allocations from this region
// as well as any other changes of alloc_* variables
static SpinLock alloc_spinlock(SpinLock::LINKER_INITIALIZED);

// returns count of bytes you need to add to ptr to round up to
// closest address aligned on alignment. If ptr is already aligned
// returns 0. It should be easy to see that returned value must be
// strictly less then alignment
//
// Alignment is assumed to be power of 2
static intptr_t GetAlignmentAddup(char *ptr, size_t alignment) {
  // negation here returns how much we need to add to get 0. Rules of
  // negation are in fact independent of desired bitness
  // (bitwise-negate and increment). Our desired bitness is
  // log_2(alignment). And masking off everything at and above that
  // bit gives us precisely what we need. Number we need to add to get
  // log_2(alignment) lowest bits at 0. It can be easily verified that
  // already aligned ptr will produce result of 0 (similarly how
  // arithmetic negate of 0 is still 0).
  return (-reinterpret_cast<intptr_t>(ptr)) & (alignment - 1);
}

extern PERFTOOLS_DLL_DECL
void* TCMalloc_SystemAlloc(size_t size, size_t *actual_size,
                           size_t alignment) {
  assert(tcmalloc::IsPowerOf2(alignment));

  // Too big allocations may force us to drop reserved chunk that
  // still has plenty of reserved address space left. So we just do
  // those big allocations directly, i.e. outside of our reserved
  // chunk.
  //
  // Things will fail miserably if size + alignment overflows
  // size_t. We don't ask for huge alignment so it should be fine to
  // just check for big size
  if (size + alignment > kVMReserveTooBigWaste || size > kVMReserveTooBigWaste) {
    return AllocDirectly(size, actual_size, alignment);
  }

  int pagesize = getpagesize();

  SpinLockHolder h(&alloc_spinlock);

  intptr_t alignment_addup;

  alignment_addup = GetAlignmentAddup(alloc_reserved_at, alignment);

  if (alignment_addup + size >= alloc_reserved_size) {
    // if we don't have enough space reserved for current allocation,
    // we just drop current address space chunk and reserve new.
    void *rv = VirtualAlloc(0, kVMReserveChunkSize, MEM_RESERVE, PAGE_READWRITE);
    if (rv == NULL) {
      return NULL;
    }
    alloc_reserved_at = static_cast<char *>(rv);
    // Windows doesn't allow MEM_RESET that covers two or more regions
    // returned from MEM_RESERVE.
    //
    // So we're leaving one page unused at the end of reserved
    // chunk. That's to prevent us from coalescing page spans
    // belonging to different MEM_RESERVE chunks, which could
    // eventually lead us to try to MEM_RESET that larger span
    alloc_reserved_size = kVMReserveChunkSize - pagesize;
    // requested alignment can be bigger than natural alignment of
    // stuff we've got from VirtualAlloc. So alignment_addup needs to
    // be properly recomputed
    alignment_addup = GetAlignmentAddup(alloc_reserved_at, alignment);

    // because of AllocDirectly check above and because
    // alignment_addup must be smaller than alignment we're sure we
    // cannot fail this check. I.e. if this check fails then
    // alignment_addup + size is almost as big as kVMReserveChunkSize
    // which is certainly larger than kVMReserveTooBigWaste. And given
    // alignment_addup is smaller than alignment, we know alignment +
    // size is even bigger. So this cannot happen.
    assert(alignment_addup + size < alloc_reserved_size);

    // we know alloc_reserved_at is page aligned and thus
    // alignment_addup should be page aligned too.
    //
    // we'll use that assertion below to ensure commit_begin == result
    // if we're hitting this path
    assert((alignment_addup & (pagesize - 1)) == 0);
  }

  char *result = alloc_reserved_at + alignment_addup;

  // we need to MEM_COMMIT memory segment we're returning. Committing
  // happens on page boundaries and pages cannot be committed twice so
  // we have to be careful.

  uintptr_t commit_begin = reinterpret_cast<uintptr_t>(result);
  // if previous allocation size was not aligned to pagesize and
  // alignment_addup is smaller than page size, then we've already
  // committed last page of previous allocation and first page of this
  // allocation. So commit address is rounded up to next page size.
  //
  // Note that there's no previous allocation in case we've just
  // reserved new chunk of address space, but in this case result is
  // aligned on pagesize. See last assertion of MEM_RESERVE path
  // above. So we'll start our commit request at result.
  commit_begin = tcmalloc::AlignUp(commit_begin, static_cast<uintptr_t>(pagesize));

  char *new_alloc_reserved_at = result + size;

  uintptr_t commit_end = reinterpret_cast<uintptr_t>(new_alloc_reserved_at);
  commit_end = tcmalloc::AlignUp(commit_end, static_cast<uintptr_t>(pagesize));
  // commit_end is easily past memory segment we're allocating
  // (because we're aligning up). So [commit_begin, commit_end) covers
  // it. And both commit_begin and commit_end are page aligned as
  // required by VirtualAlloc.
  //
  // In order for MEM_COMMIT to work we also need commit_end to be
  // less or equal than end of reserved area. This holds too. Because
  // if new_alloc_reserved_at is page aligned then commit_end equals
  // it (align up did not change it) and that means our condition
  // holds. If it's not page aligned then it can only be greater if
  // entire last page of our allocation is past reserved area which we
  // know cannot be true. I.e. it would make byte
  // new_alloc_reserved_at - 1 be past alloc_reserved_at +
  // alloc_reserved_size, which is violation of our other assertions
  // above. I.e. because alloc_reserved_at + alloc_reserved_size
  // (address just past end of our address space reservation) is page
  // aligned
  assert(commit_end <= reinterpret_cast<intptr_t>(alloc_reserved_at) + alloc_reserved_size);

  if (commit_end != commit_begin) {
    void *rv = VirtualAlloc(reinterpret_cast<void *>(commit_begin),
                            commit_end - commit_begin, MEM_COMMIT, PAGE_READWRITE);
    if (rv == NULL) {
      return NULL;
    }
  }

  alloc_reserved_size -= new_alloc_reserved_at - alloc_reserved_at;
  alloc_reserved_at = new_alloc_reserved_at;

  if (actual_size) {
    *actual_size = size;
  }

  TCMalloc_SystemTaken += size;

  return static_cast<void *>(result);
}

extern PERFTOOLS_DLL_DECL
bool TCMalloc_SystemRelease(void* start, size_t length) {
  void* result = VirtualAlloc(start, length,
                              MEM_RESET, PAGE_READWRITE);
  return result != NULL;
}

bool RegisterSystemAllocator(SysAllocator *allocator, int priority) {
  return false;   // we don't allow registration on windows, right now
}

void DumpSystemAllocatorStats(TCMalloc_Printer* printer) {
  // We don't dump stats on windows, right now
}


// -----------------------------------------------------------------------
// These functions rework existing functions of the same name in the
// Google codebase.

// A replacement for HeapProfiler::CleanupOldProfiles.
void DeleteMatchingFiles(const char* prefix, const char* full_glob) {
  WIN32_FIND_DATAA found;  // that final A is for Ansi (as opposed to Unicode)
  HANDLE hFind = FindFirstFileA(full_glob, &found);   // A is for Ansi
  if (hFind != INVALID_HANDLE_VALUE) {
    const int prefix_length = strlen(prefix);
    do {
      const char *fname = found.cFileName;
      if ((strlen(fname) >= prefix_length) &&
          (memcmp(fname, prefix, prefix_length) == 0)) {
        RAW_VLOG(0, "Removing old heap profile %s\n", fname);
        // TODO(csilvers): we really need to unlink dirname + fname
        _unlink(fname);
      }
    } while (FindNextFileA(hFind, &found) != FALSE);  // A is for Ansi
    FindClose(hFind);
  }
}
