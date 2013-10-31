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
#include "llrb.h"
#include "span.h"
#include "static_vars.h"
#include <sys/resource.h>

#include <stdio.h>

namespace tcmalloc {

LLRBNode* LLRB::NewNode(Span* value) {
  LLRBNode* result = Static::llrb_node_allocator()->New();
  memset(result, 0, sizeof(*result));
  result->value = value;
  return result;
}

void LLRB::DeleteNode(LLRBNode* node) {
  Static::llrb_node_allocator()->Delete(node);
}

void LLRB::Init() {
  llrb_new(&tree_);
}

void LLRB::Insert(Span* span) {
  LLRBNode *node = NewNode(span);
  llrb_insert(&tree_, node);
}

void LLRB::Remove(Span* span) {
  LLRBNode search;
  LLRBNode* found;

  search.value = span;
  found = llrb_search(&tree_, &search);

  if (found)
    Remove(found);
}

void LLRB::Remove(LLRBNode* node) {
  llrb_remove(&tree_, node);
  DeleteNode(node);
}

Span* LLRB::GetBestFit(size_t pages) {
  LLRBNode *node = tree_.rbt_root;

  if (node != &tree_.rbt_nil) {				
    while(true) {
      if (node->value->length >= pages) {
	if (node->llrb_link.rbn_left != &tree_.rbt_nil &&
	    node->llrb_link.rbn_left->value->length >= pages) {
	  node = node->llrb_link.rbn_left;
	} else {
	  break;
	}
      } else {
	if (node->llrb_link.rbn_right != &tree_.rbt_nil) {
	  node = node->llrb_link.rbn_right;
	} else {
	  break;
	}
      }
    }

    if (node->value->length >= pages) {
      Span* ret = node->value;
      Remove(node);
      return ret;
    }
  }

  return NULL;
}

bool LLRB::Includes(Span* span) {
  return false;
}
}
