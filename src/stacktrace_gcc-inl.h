// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#ifndef BASE_STACKTRACE_GCC_INL_H_
#define BASE_STACKTRACE_GCC_INL_H_

namespace {

  template <int N, int I>
  struct gst_gcc_simple_bt {
    static __attribute__((noinline))
    void *getfp(void **rv) {
      if (__builtin_frame_address(I) == 0) {
	return NULL;
      }
      void *p = __builtin_return_address(I);
      if (p == NULL) {
	return NULL;
      }
      *rv = p;
      return reinterpret_cast<void *>(gst_gcc_simple_bt<N, I+1>::getfp);
    }

    typedef void *(*getfp_fn_t)(void **rv);

    __attribute__((noinline))
    int operator ()(void **result, int max_depth, int skip_count) {
      // one for this method and one for rec
      skip_count += 2;

      getfp_fn_t fn = getfp;
      int i;

      for (i = 0; i < max_depth; i++) {
        void *pc;
	fn = reinterpret_cast<getfp_fn_t>(fn(&pc));
        if (fn == NULL) {
          break;
        }
        if (i >= skip_count) {
          result[i - skip_count] = pc;
        }
      }

      return i;
    }
  };

  template <int N>
  struct gst_gcc_simple_bt<N, N> : public gst_gcc_simple_bt<N, N-1> {
    static void *getfp(void **rv) {
      return NULL;
    }
  };

} // namespace

#endif  // BASE_STACKTRACE_GCC_INL_H_

// Note: this part of the file is included several times.
// Do not put globals below.

// The following 4 functions are generated from the code below:
//   GetStack{Trace,Frames}()
//   GetStack{Trace,Frames}WithContext()
//
// These functions take the following args:
//   void** result: the stack-trace, as an array
//   int* sizes: the size of each stack frame, as an array
//               (GetStackFrames* only)
//   int max_depth: the size of the result (and sizes) array(s)
//   int skip_count: how many stack pointers to skip before storing in result
//   void* ucp: a ucontext_t* (GetStack{Trace,Frames}WithContext only)

static int GET_STACK_TRACE_OR_FRAMES {
#if IS_STACK_FRAMES
  memset(sizes, 0, sizeof(*sizes) * max_depth);
#endif

  // one for this function
  skip_count += 1;
  gst_gcc_simple_bt<64, 0> bt;

  int n = bt(result, max_depth, skip_count);

  // make sure we don't tail-call capture
  (void)(const_cast<void * volatile *>(result))[0];
  return n;
}
