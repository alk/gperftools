/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil -*- */
#include <stdio.h>
#include <stdlib.h>

typedef void *(*mptr)(size_t s);

volatile mptr malloc_i;

int main(void)
{
  printf("p1: %p\np2: %p\n", malloc(128), (void *)malloc);
  malloc_i = malloc;
  printf("p3: %p\n", malloc_i(128));
  return 0;
}
