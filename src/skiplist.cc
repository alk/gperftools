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

#include <config.h>
#include "common.h"
#include "skiplist.h"
#include "span.h"
#include "static_vars.h"
#include <sys/resource.h>

namespace tcmalloc {

SkiplistNode* Skiplist::NewNode(Span* value) {
  SkiplistNode* result = Static::skiplist_node_allocator()->New();
  memset(result, 0, sizeof(*result));
  result->value = value;
  return result;
}

void Skiplist::DeleteNode(SkiplistNode* node) {
  Static::skiplist_node_allocator()->Delete(node);
}

void Skiplist::Init() {
  level_ = 0;
  head_ = NewNode(NULL);
}

void Skiplist::Insert(Span* span) {
  SkiplistNode* update[kSkiplistHeight];
  SkiplistNode* x = head_;

  for (int i = level_; i >= 0; i--) {
    while(x->forward[i] && SpanCompare(x->forward[i]->value, span) == -1) {
      x = x->forward[i];
    }
    update[i] = x;
  }

  unsigned int lvl = random_level();
  if (lvl > level_) {
    if (level_ == kSkiplistHeight - 1) {
      lvl = level_;
    } else {
      // increase the level of the list by a maximum of 1 as per the paper
      lvl = level_ + 1;
      level_ = lvl;
      update[lvl] = head_;
    }
  }

  x = NewNode(span);
  span->skiplist_node_ptr = x;
  for(int i = 0; i <= lvl; i++) {
    ASSERT(update[i] != x);

    x->forward[i] = update[i]->forward[i];
    if (update[i]->forward[i])
      update[i]->forward[i]->backward[i] = x;

    update[i]->forward[i] = x;
    x->backward[i] = update[i];
  }
}

void Skiplist::Remove(Span* span) {
  if (span->skiplist_node_ptr) {
    SkiplistNode* x = span->skiplist_node_ptr;

    for(int i = 0; i <= level_ && x->backward[i]; i++) {
      ASSERT(x->backward[i]->forward[i] == x);
      x->backward[i]->forward[i] = x->forward[i];
      ASSERT(!x->forward[i] || x->forward[i]->backward[i] == x);
      if (x->forward[i])
        x->forward[i]->backward[i] = x->backward[i];
      ASSERT(x->backward[i] != x->forward[i]);
    }

    DeleteNode(x);

    while(level_ > 0 && !head_->forward[level_]) {
      level_--;
    }

    span->skiplist_node_ptr = NULL;
  }
}

Span* Skiplist::GetBestFit(size_t pages) {
  SkiplistNode* x = head_;

  for (int i = level_; i >= 0; i--) {
    while(x->forward[i] &&
	  x->forward[i]->value->length < pages) {
      x = x->forward[i];
    }

    if (x->forward[0]) {
      x = x->forward[0];
      Span* rv = x->value;

      if (rv && rv->length >= pages) {
        Remove(rv);
        return rv;
      }
    }
  }

  return NULL;
}

bool Skiplist::Includes(Span* span) {
  SkiplistNode* x = head_;
  while(x->forward[0] != NULL) {
    if (x->forward[0]->value == span) {
      return true;
    }
    x = x->forward[0];
  }

  return false;
}

void Skiplist::Print() {
  fprintf(stderr, "printing skip list of level: %d\n", level_);
  for(int i = level_; i >= 0; i--) {
    fprintf(stderr, "level %d: [", i);
    SkiplistNode* x = head_;
    while(x->forward[i] != NULL) {
      fprintf(stderr, "[%lu,%u,%p]", x->forward[i]->value->length,
                                  (unsigned int)x->forward[i]->value->start,
				  x->forward[i]->value);
      x = x->forward[i];
    }
    fprintf(stderr, "]\n");
  }
}

}
