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

namespace tcmalloc {

struct LLRBNode {
  Span* value;
  LLRBNode* left, *right;
  bool color;
};

class LLRB {
  public:
   void Init();
   void Insert(Span* span);
   void Remove(Span* span);
   Span* GetBestFit(size_t pages);
   bool Includes(Span* span);

  private:
   static const bool RED = true;
   static const bool BLACK = false;

   LLRBNode* root_;

   LLRBNode* NewNode(Span* value);
   void DeleteNode(LLRBNode* node);
};

}  // namespace tcmalloc

#endif  // TCMALLOC_LLRB_H_
