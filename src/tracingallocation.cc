// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// heavily based on debugallocation.cc

#include "config.h"
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif
// We only need malloc.h for struct mallinfo.
#ifdef HAVE_STRUCT_MALLINFO
// Malloc can be in several places on older versions of OS X.
# if defined(HAVE_MALLOC_H)
# include <malloc.h>
# elif defined(HAVE_MALLOC_MALLOC_H)
# include <malloc/malloc.h>
# elif defined(HAVE_SYS_MALLOC_H)
# include <sys/malloc.h>
# endif
#endif
#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <gperftools/malloc_extension.h>
#include <gperftools/malloc_hook.h>
#include <gperftools/stacktrace.h>
#include "addressmap-inl.h"
#include "base/commandlineflags.h"
#include "base/googleinit.h"
#include "base/logging.h"
#include "base/spinlock.h"
#include "malloc_hook-inl.h"
#include "symbolize.h"

#include "malloc_tracer.h"

// NOTE: due to #define below, tcmalloc.cc will omit tc_XXX
// definitions. So that debug implementations can be defined
// instead. We're going to use do_malloc, do_free and other do_XXX
// functions that are defined in tcmalloc.cc for actual memory
// management
#define TCMALLOC_USING_DEBUGALLOCATION
#include "tcmalloc.cc"

// ========================================================================= //

// Round "value" up to next "alignment" boundary.
// Requires that "alignment" be a power of two.
static intptr_t RoundUp(intptr_t value, intptr_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static inline ATTRIBUTE_ALWAYS_INLINE size_t tracing_adjust_size(size_t size) {
  size_t rv = size + 16;
  if (rv < size) {
    return size;
  }
  return rv;
}

// The following functions may be called via MallocExtension::instance()
// for memory verification and statistics.
class TracingMallocImplementation : public TCMallocImplementation {
 public:
  // TODO: nallocx and all bits below

  virtual size_t GetEstimatedAllocatedSize(size_t size) {
    return TCMallocImplementation::GetEstimatedAllocatedSize(tracing_adjust_size(size));
  }

  virtual size_t GetAllocatedSize(const void* p) {
    if (p == NULL) {
      return 0;
    }
    uint64_t *meta = reinterpret_cast<uint64_t *>(const_cast<void *>(p)) - 2;
    return TCMallocImplementation::GetAllocatedSize(meta);
  }

 };

static union {
  char chars[sizeof(TracingMallocImplementation)];
  void *ptr;
} tracing_malloc_implementation_space;

REGISTER_MODULE_INITIALIZER(tracingallocation, {
#if (__cplusplus >= 201103L)
    COMPILE_ASSERT(alignof(tracing_malloc_implementation_space) >= alignof(TracingMallocImplementation),
                   tracing_malloc_implementation_space_is_not_properly_aligned);
#endif
  // Either we or valgrind will control memory management.  We
  // register our extension if we're the winner. Otherwise let
  // Valgrind use its own malloc (so don't register our extension).
  if (!RunningOnValgrind()) {
    TracingMallocImplementation *impl = new (tracing_malloc_implementation_space.chars) TracingMallocImplementation();
    MallocExtension::Register(impl);
  }
});

static inline ATTRIBUTE_ALWAYS_INLINE void *tracing_pad(void *chunk, size_t size) {
  if (PREDICT_FALSE(chunk == NULL)) {
    return chunk;
  }
  uint64_t tok = MallocTracer::GetInstance()->TraceMalloc(size);
  uint64_t *meta = static_cast<uint64_t *>(chunk);
  meta[0] = 0;
  meta[1] = tok;
  return meta + 2;
}

static inline ATTRIBUTE_ALWAYS_INLINE void *add_malloc_tracing(size_t size, void *(*malloc_fn)(size_t)) {
  return tracing_pad(malloc_fn(tracing_adjust_size(size)), size);
}

static inline ATTRIBUTE_ALWAYS_INLINE void trace_free(void *ptr) {
  if (!ptr) {
    return;
  }
  uint64_t *meta = reinterpret_cast<uint64_t *>(ptr) - 2;
  uint64_t tok = meta[1];
  uint64_t off = meta[0];
  MallocTracer::GetInstance()->TraceFree(tok);
  do_free(reinterpret_cast<char *>(meta) - off);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_malloc(size_t size) PERFTOOLS_THROW {
  if (ThreadCache::IsUseEmergencyMalloc()) {
    return tcmalloc::EmergencyMalloc(size);
  }
  void* result = add_malloc_tracing(size, do_malloc_or_cpp_alloc);
  MallocHook::InvokeNewHook(result, size);
  return result;
}

extern "C" PERFTOOLS_DLL_DECL void tc_free(void* ptr) PERFTOOLS_THROW {
  if (tcmalloc::IsEmergencyPtr(ptr)) {
    return tcmalloc::EmergencyFree(ptr);
  }
  MallocHook::InvokeDeleteHook(ptr);
  trace_free(ptr);
}

extern "C" PERFTOOLS_DLL_DECL void tc_cfree(void* ptr) PERFTOOLS_THROW
#ifdef TC_ALIAS
  TC_ALIAS(tc_free);
#else
{
  if (tcmalloc::IsEmergencyPtr(ptr)) {
    return tcmalloc::EmergencyFree(ptr);
  }
  MallocHook::InvokeDeleteHook(ptr);
  trace_free(ptr);
}
#endif

extern "C" PERFTOOLS_DLL_DECL void tc_free_sized(void *ptr, size_t size) PERFTOOLS_THROW {
  ASSERT(!tcmalloc::IsEmergencyPtr(ptr));
  MallocHook::InvokeDeleteHook(ptr);
  trace_free(ptr);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_calloc(size_t n, size_t elem_size) PERFTOOLS_THROW {
  if (ThreadCache::IsUseEmergencyMalloc()) {
    return tcmalloc::EmergencyCalloc(n, elem_size);
  }
  // Overflow check
  const size_t size = n * elem_size;
  if (elem_size != 0 && size / elem_size != n) return NULL;

  void* result = add_malloc_tracing(size, do_malloc_or_cpp_alloc);
  MallocHook::InvokeNewHook(result, size);
  if (result != NULL) {
    memset(result, 0, size);
  }
  return result;
}

extern "C" PERFTOOLS_DLL_DECL void* tc_realloc(void* old_ptr, size_t new_size) PERFTOOLS_THROW {
  if (old_ptr == NULL) {
    return tc_malloc(new_size);
  }

  if (tcmalloc::IsEmergencyPtr(old_ptr)) {
    return tcmalloc::EmergencyRealloc(old_ptr, new_size);
  }

  uint64_t *meta = reinterpret_cast<uint64_t *>(old_ptr) - 2;
  uint64_t tok = meta[1];
  uint64_t off = meta[0];
  uint64_t new_tok = MallocTracer::GetInstance()->TraceRealloc(tok, new_size);
  size_t extra_size = new_size + 16;
  if (extra_size < new_size) {
    extra_size = new_size;
  }
  // TODO: unbreak hooks
  void *rv = do_realloc(reinterpret_cast<char *>(meta) - off, extra_size);
  if (!rv) {
    return rv;
  }

  meta = static_cast<uint64_t *>(rv);
  meta[0] = 0;
  meta[1] = new_tok;
  return meta + 2;
}

ATTRIBUTE_SECTION(google_malloc)
void* cpp_throw_oom(size_t size) {
  return handle_oom(retry_malloc, reinterpret_cast<void *>(size),
                    true, false);
}

ATTRIBUTE_SECTION(google_malloc)
void* cpp_nothrow_oom(size_t size) {
  return handle_oom(retry_malloc, reinterpret_cast<void *>(size),
                    true, true);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_new(size_t size) {
  size_t new_size = tracing_adjust_size(size);
  void* p = do_malloc(new_size);
  if (PREDICT_FALSE(p == NULL)) {
    p = handle_oom(retry_malloc, reinterpret_cast<void *>(new_size), true, false);
  }
  p = tracing_pad(p, size);
  MallocHook::InvokeNewHook(p, size);
  return p;
}

extern "C" PERFTOOLS_DLL_DECL void* tc_new_nothrow(size_t size, const std::nothrow_t&) PERFTOOLS_THROW {
  size_t new_size = tracing_adjust_size(size);
  void* p = do_malloc(new_size);
  if (PREDICT_FALSE(p == NULL)) {
    p = handle_oom(retry_malloc, reinterpret_cast<void *>(new_size), true, true);
  }
  p = tracing_pad(p, size);
  MallocHook::InvokeNewHook(p, size);
  return p;
}

extern "C" PERFTOOLS_DLL_DECL void tc_delete(void* p) PERFTOOLS_THROW {
  MallocHook::InvokeDeleteHook(p);
  trace_free(p);
}

extern "C" PERFTOOLS_DLL_DECL void tc_delete_sized(void* p, size_t size) PERFTOOLS_THROW {
  MallocHook::InvokeDeleteHook(p);
  trace_free(p);
}

extern "C" PERFTOOLS_DLL_DECL void tc_delete_nothrow(void* p, const std::nothrow_t&) PERFTOOLS_THROW {
  tc_delete(p);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_newarray(size_t size) {
  return tc_new(size);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_newarray_nothrow(size_t size, const std::nothrow_t&)
    PERFTOOLS_THROW {
  return tc_new_nothrow(size, std::nothrow_t());
}

extern "C" PERFTOOLS_DLL_DECL void tc_deletearray(void* p) PERFTOOLS_THROW {
  tc_delete(p);
}

extern "C" PERFTOOLS_DLL_DECL void tc_deletearray_sized(void* p, size_t size) PERFTOOLS_THROW {
  tc_delete(p);
}

extern "C" PERFTOOLS_DLL_DECL void tc_deletearray_nothrow(void* p, const std::nothrow_t&) PERFTOOLS_THROW {
  tc_delete(p);
}

static inline ATTRIBUTE_ALWAYS_INLINE void* do_tracing_memalign(size_t align, size_t size) {
  size_t extra = (align < 16) ? 16 : align;
  if (size + extra < size) {
    return NULL;
  }
  void* p = do_memalign_or_cpp_memalign(align, size + extra);
  if (PREDICT_TRUE(p != NULL)) {
    uint64_t tok = MallocTracer::GetInstance()->TraceMalloc(size);
    uint64_t *meta = static_cast<uint64_t *>(p) + (extra - 16) / 8;
    meta[0] = extra - 16;
    meta[1] = tok;
    p = meta + 2;
  }
  return p;
}

extern "C" PERFTOOLS_DLL_DECL void* tc_memalign(size_t align, size_t size) PERFTOOLS_THROW {
  void *p = do_tracing_memalign(align, size);
  MallocHook::InvokeNewHook(p, size);
  return p;
}

// Implementation taken from tcmalloc/tcmalloc.cc
extern "C" PERFTOOLS_DLL_DECL int tc_posix_memalign(void** result_ptr, size_t align, size_t size)
    PERFTOOLS_THROW {
  if (((align % sizeof(void*)) != 0) ||
      ((align & (align - 1)) != 0) ||
      (align == 0)) {
    return EINVAL;
  }

  void* result = do_tracing_memalign(align, size);
  MallocHook::InvokeNewHook(result, size);
  if (result == NULL) {
    return ENOMEM;
  } else {
    *result_ptr = result;
    return 0;
  }
}

extern "C" PERFTOOLS_DLL_DECL void* tc_valloc(size_t size) PERFTOOLS_THROW {
  // Allocate >= size bytes starting on a page boundary
  void *p = do_tracing_memalign(getpagesize(), size);
  MallocHook::InvokeNewHook(p, size);
  return p;
}

extern "C" PERFTOOLS_DLL_DECL void* tc_pvalloc(size_t size) PERFTOOLS_THROW {
  // Round size up to a multiple of pages
  // then allocate memory on a page boundary
  int pagesize = getpagesize();
  size = RoundUp(size, pagesize);
  if (size == 0) {     // pvalloc(0) should allocate one page, according to
    size = pagesize;   // http://man.free4web.biz/man3/libmpatrol.3.html
  }
  void *p = do_tracing_memalign(pagesize, size);
  MallocHook::InvokeNewHook(p, size);
  return p;
}

// malloc_stats just falls through to the base implementation.
extern "C" PERFTOOLS_DLL_DECL void tc_malloc_stats(void) PERFTOOLS_THROW {
  do_malloc_stats();
}

extern "C" PERFTOOLS_DLL_DECL int tc_mallopt(int cmd, int value) PERFTOOLS_THROW {
  return do_mallopt(cmd, value);
}

#ifdef HAVE_STRUCT_MALLINFO
extern "C" PERFTOOLS_DLL_DECL struct mallinfo tc_mallinfo(void) PERFTOOLS_THROW {
  return do_mallinfo();
}
#endif

extern "C" PERFTOOLS_DLL_DECL size_t tc_malloc_size(void* ptr) PERFTOOLS_THROW {
  return MallocExtension::instance()->GetAllocatedSize(ptr);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_malloc_skip_new_handler(size_t size) PERFTOOLS_THROW {
  void* result = add_malloc_tracing(size, do_malloc);
  MallocHook::InvokeNewHook(result, size);
  return result;
}
