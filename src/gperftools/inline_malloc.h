/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
/*
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
 */
#ifndef GPERFTOOLS_INLINE_MALLOC_H_
#define GPERFTOOLS_INLINE_MALLOC_H_

// TODO: drop C++ comments
// TODO: figure out includes
// TODO: deal with rest of TODOs

#ifdef __GNUC__
#include <stdlib.h> /* for malloc */

#pragma push_macro("PERFTOOLS_DLL_DECL")

#ifdef _WIN32
#  define PERFTOOLS_DLL_DECL  __declspec(dllimport)
#else
#  define PERFTOOLS_DLL_DECL
#endif


#ifdef __cplusplus

#include <new> /* for operator new */

extern "C" {
#endif /* __cplusplus */

  PERFTOOLS_DLL_DECL void* tc_malloc(size_t size);
  PERFTOOLS_DLL_DECL void* tc_malloc_small_with_cl(size_t size,
                                                   size_t cl, size_t list_off);

  PERFTOOLS_DLL_DECL void tc_free_sized(void *ptr, size_t size);
  PERFTOOLS_DLL_DECL void tc_free_small_with_cl(void *ptr, size_t size,
                                                size_t cl, size_t list_off);

#ifdef __cplusplus
}
#endif /* __cplusplus */

static inline __attribute__((always_inline))
void tc_malloc_cl_and_size(size_t size,
                                         size_t *osize, size_t *cl) {
  if (size == 0) {
    *osize = 8;
    *cl = 1;
    return;
  }
  if (size <= 16) {
    *osize = (size + 7) & ~7;
    *cl = *osize / 8;
    return;
  }
  if (size <= 256) {
    *osize = (size + 15) & ~15;
    *cl = *osize / 16 + 1; /* 1 is due to cl 1 at 8 */
    return;
  }
  if (size <= 512) {
    *osize = (size + 31) & ~31;
    *cl = *osize / 32 + 9; /* 9 is due to smaller size classes */
    return;
  }
  *osize = (size + 63) & ~63;
  *cl = *osize / 64 + 17;
}

// TODO: magic number
static inline __attribute__((always_inline))
void *tc_call_small_malloc(size_t s,
                           void *(*malloc_small_w_cl)(size_t, size_t, size_t)) {
  size_t rsize, cl;
  tc_malloc_cl_and_size(s, &rsize, &cl);
  return malloc_small_w_cl(rsize, cl, cl * 24);
}

static inline __attribute__((always_inline))
void tc_call_small_free(void *p, size_t s,
                        void (*free_small_w_cl)(void *, size_t,
                                                size_t, size_t)) {
  size_t rsize, cl;
  tc_malloc_cl_and_size(s, &rsize, &cl);
  free_small_w_cl(p, rsize, cl, cl * 24);
}

static inline void *tc_malloc_inline(size_t size) {
  if (!__builtin_constant_p(size) || size > 1024) {
    return tc_malloc(size);
  }
  return tc_call_small_malloc(size, tc_malloc_small_with_cl);
}

static inline void tc_free_sized_inline(void *p, size_t size) {
  if (!__builtin_constant_p(size) || size > 1024) {
    tc_free_sized(p, size);
    return;
  }
  tc_call_small_free(p, size, tc_free_small_with_cl);
}

#ifdef __cplusplus

extern "C" {
  PERFTOOLS_DLL_DECL void* tc_new(size_t size);
  PERFTOOLS_DLL_DECL void* tc_new_nothrow(size_t size,
                                          const std::nothrow_t&);

  PERFTOOLS_DLL_DECL void* tc_newarray(size_t size);
  PERFTOOLS_DLL_DECL void* tc_newarray_nothrow(size_t size,
                                               const std::nothrow_t&);

  PERFTOOLS_DLL_DECL void* tc_new_small_with_cl(size_t size,
                                                size_t cl, size_t list_off);
  PERFTOOLS_DLL_DECL void* tc_new_small_with_cl_nothrow(size_t size,
                                                        size_t cl,
                                                        size_t list_off);

  PERFTOOLS_DLL_DECL void* tc_newar_small_with_cl(size_t size,
                                                  size_t cl, size_t list_off);
  PERFTOOLS_DLL_DECL void* tc_newar_small_with_cl_nothrow(size_t size,
                                                          size_t cl,
                                                          size_t list_off);

  PERFTOOLS_DLL_DECL void tc_delete_sized(void* p, size_t size) throw();
  PERFTOOLS_DLL_DECL void tc_deletearray_sized(void* p, size_t size) throw();

  PERFTOOLS_DLL_DECL void tc_delete_small_with_cl(void *ptr, size_t size,
                                                  size_t cl, size_t list_off);
  PERFTOOLS_DLL_DECL void tc_deletear_small_with_cl(void *ptr, size_t size,
                                                    size_t cl, size_t list_off);
}

/* clang rightfully complains that standard forbids inline global
 * operator new/delete. But we know what we're doing. On the other
 * hand, current clangs don't have real support for builtin_constant_p
 * (always evaluates to false), so it doesn't really support
 * inline-malloc. */
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winline-new-delete"
#endif /* __clang__ */

// TODO: debugallocation support and tests
__attribute__((always_inline))
inline void* operator new(size_t s) throw(std::bad_alloc) {
  if (!__builtin_constant_p(s) || s > 1024) {
    return tc_new(s);
  }
  return tc_call_small_malloc(s, tc_new_small_with_cl);
}

__attribute__((always_inline))
inline void* operator new[](size_t s) throw(std::bad_alloc) {
  if (!__builtin_constant_p(s) || s > 1024) {
    return tc_newarray(s);
  }
  return tc_call_small_malloc(s, tc_newar_small_with_cl);
}

__attribute__((always_inline))
inline void* operator new(size_t s, const std::nothrow_t& nt) throw (){
  if (!__builtin_constant_p(s) || s > 1024) {
    return tc_new_nothrow(s, nt);
  }
  return tc_call_small_malloc(s, tc_new_small_with_cl_nothrow);
}

__attribute__((always_inline))
inline void* operator new[](size_t s, const std::nothrow_t& nt) throw() {
  if (!__builtin_constant_p(s) || s > 1024) {
    return tc_newarray_nothrow(s, nt);
  }
  return tc_call_small_malloc(s, tc_newar_small_with_cl_nothrow);
}

// TODO: proper c++14 support here
#if __cplusplus >= 201402L

inline void operator delete(void* p, size_t s) {
  if (!__builtin_constant_p(s) || s > 1024) {
    return tc_delete_sized(p, s);
  }
  tc_call_small_free(p, s, tc_delete_small_with_cl);
}

inline void operator delete[](void* p, size_t s) {
  if (!__builtin_constant_p(s) || s > 1024) {
    return tc_delete_sized(p, s);
  }
  tc_call_small_free(p, s, tc_deletear_small_with_cl);
}

#endif /* __cplusplus >= 201402L */

#endif /* __cplusplus */

#pragma pop_macro("PERFTOOLS_DLL_DECL")

#ifndef NO_INLINE_MALLOC_DEFINE

#define malloc(s) tc_malloc_inline(s)
#define tc_free_sized(p, s) tc_free_sized_inline(p, s)

#endif /* NO_INLINE_MALLOC_DEFINE */

#ifdef __clang__
#pragma clang diagnostic pop
#endif /* __clang__ */

#endif /* __GNUC__ */


#endif  /* GPERFTOOLS_INLINE_MALLOC_H_ */
