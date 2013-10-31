// Copyright (c) 2013, Google Inc., James Golick
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

// ---
// Author: James Golick <jamesgolick@gmail.com>

#ifndef TCMALLOC_LLRB_H_
#define TCMALLOC_LLRB_H_

#include <config.h>
#include "common.h"
#include "span.h"
#include <unistd.h>
#include <assert.h>
#include "rb.h"
#include <stdio.h>

namespace tcmalloc {

typedef struct LLRBNode llrb_node_t;
struct LLRBNode {
  rb_node(llrb_node_t) llrb_link;
  Span* value;
};

// Span compare for llrb
inline int llrb_cmp(llrb_node_t* x, llrb_node_t* y) {
  Span *a;
  Span *b;

  a = x->value;
  b = y->value;

  if (a == NULL ||
      (a->length < b->length || (a->length == b->length && a->start < b->start))) {
    return -1;
  } else if (a->length > b->length || (a->length == b->length && a->start > b->start)) {
    return 1;
  } else {
    return 0;
  }
}

typedef rb_tree(llrb_node_t) llrb_t;

rb_gen(static, llrb_, llrb_t, llrb_node_t, llrb_link, llrb_cmp);

class LLRB {
  public:
   void Init();
   void Insert(Span* span);
   void Remove(Span* span);
   Span* GetBestFit(size_t pages);
   bool Includes(Span* span);

  private:
   llrb_t tree_;

   LLRBNode* NewNode(Span* value);
   void DeleteNode(LLRBNode* node);
   void Remove(LLRBNode* node);
};

}  // namespace tcmalloc

#endif  // TCMALLOC_LLRB_H_
