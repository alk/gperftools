/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2024, gperftools Contributors
 * All rights reserved.
 *
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>
#include <link.h>

#include <string>

#include <cxxabi.h>

#include "symbolize.h"


bool no_demangle;

std::string phdr_binary;
uintptr_t phdr_binary_addr = uintptr_t{8} << 30;

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *info,
				    size_t size, void* data),
		    void *data) {
  struct dl_phdr_info info;
  memset(&info, 0, sizeof(info));
  info.dlpi_addr = phdr_binary_addr;
  info.dlpi_name = phdr_binary.c_str();

  return callback(&info, sizeof(info), data);
}

uintptr_t must_parse_address(char* arg) {
  char* endptr = nullptr;
  long long input_addr = strtoll(arg, &endptr, 16);
  if (*endptr != '\0') {
    printf("failed to parse address: %s\n", arg);
    exit(0);
  }

  uintptr_t addr = static_cast<uintptr_t>(input_addr);
  if (addr < phdr_binary_addr) {
    addr += phdr_binary_addr;
  }

  return addr;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string_view{argv[1]} == "--no-demangle") {
    no_demangle = true;
    argc--;
    argv++;
  }

  if (argc < 3) {
    printf("need 2 args!\n");
    return 1;
  }

  phdr_binary = argv[1];

  uintptr_t last_addr = 0;

  tcmalloc::SymbolizerAPI::With(
    [&] (const tcmalloc::SymbolizerAPI& api) {
      for (int i = 2; i < argc; i++) {
        api.Add(must_parse_address(argv[i]));
      }
    },
    [&] (const tcmalloc::SymbolizeOutcome& o) {
      if (o.pc != last_addr) {
        printf("0x%zx\n", o.pc - phdr_binary_addr);
        last_addr = o.pc;
      }
      auto fn = no_demangle ? o.original_function : o.function;
      printf("%s\n",
             fn ? fn : "??");
      if (o.filename) {
        printf("%s:%d\n", o.filename, o.lineno);
      } else {
        printf("??:0\n");
      }
    });
}
