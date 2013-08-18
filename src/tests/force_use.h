#ifndef FORCE_USE_H
#define FORCE_USE_H

static uintptr_t malloc_forcer;

static void force_use(void *ptr) {
  malloc_forcer ^= reinterpret_cast<uintptr_t>(ptr);
}


#endif
