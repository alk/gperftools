// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <algorithm>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <malloc.h>
#include <vector>
#include <string.h>

#define POINTERS_SIZE (32<<20)
#define SIZES_COUNT 12
#define EXTRA_SIZES 4

void *pointers[POINTERS_SIZE];
static int usable_sizes[SIZES_COUNT + EXTRA_SIZES];

static __attribute__((constructor))
void init_usable_sizes() {
  int sz = 16;
  for (int i = 0; i < SIZES_COUNT; i++) {
    usable_sizes[i] = sz;
    printf("sz[%d] = %d\n", i, sz);
    void *rv = malloc(sz + 1);
    sz = malloc_usable_size(rv);
    free(rv);
  }
}

class SimpleRand {
public:
  SimpleRand(uint64_t seed = 0) : seed_(seed) {}
  uint32_t Get() {
    seed_ = seed_ * 25214903917ULL + 11;
    return seed_ >> 15;
  }
  int GetSize() {
    return usable_sizes[Get() % (SIZES_COUNT+EXTRA_SIZES)];
  }
private:
  uint64_t seed_;
};

void* thread_body(void *dummy) {
  SimpleRand rnd(reinterpret_cast<uint64_t>(__builtin_frame_address(0)));
  do {
    int index = rnd.Get() % POINTERS_SIZE;
    int size = rnd.GetSize();
    void* new_ptr = NULL;
    if (size != 0) {
      new_ptr = malloc(size);
    }
    void* old_ptr = pointers[index];
    void* t;
    while ((t = __sync_val_compare_and_swap(&pointers[index], old_ptr, new_ptr)) != old_ptr) {
      old_ptr = t;
    }
    free(old_ptr);
  } while (true);
}

int main(int argc, char* argv[]) {
  int thread_count = std::max(2, argc >= 2 ? atoi(argv[1]) : 1) - 1;

  signal(SIGINT, [](int dummy) {
      exit(0);
    });

  SimpleRand rnd;

  for (int i = 0; i < POINTERS_SIZE; i++) {
    int size = rnd.GetSize();
    if (size != 0) {
      pointers[i] = malloc(size);
      memset(pointers[i], 0, 8);
    }
  }

  printf("Filled whole array.\n");

  std::vector<pthread_t> threads(thread_count);
  for (int i = 0; i < thread_count; i++) {
    pthread_t thr;
    int rv = pthread_create(&thr, NULL, thread_body, NULL);
    if (rv) {
      errno = rv;
      perror("pthread_create");
    }
  }
  thread_body(NULL);
  for (int i = 0; i < thread_count; i++) {
    pthread_join(threads[i], NULL);
  }
  return 0;
}
