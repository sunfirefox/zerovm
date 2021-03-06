/*
 * Copyright 2008 The Native Client Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can
 * be found in the LICENSE file.
 */
/*
 * Copyright (c) 2012, LiteStack, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * NaCl Generic I/O interface.
 */
#ifndef GIO_H_
#define GIO_H_

#include <stdio.h>
#include "src/main/tools.h"

EXTERN_C_BEGIN

struct Gio; /* fwd */

/*
 * In the generic I/O package, -1 is used consistently to indicate
 * error.  Check the return value for equality to -1 and do not check
 * that it's negative.
 *
 * Here's the rationale.  Note that the Read and Write virtual
 * functions, like the read/write system calls, return ssize_t but
 * takes a count argument that's of type size_t.  Since
 * sizeof(ssize_t) == sizeof(size_t), this means that we either never
 * perform the full operation if the count argument would be negative
 * if viewed as an ssize_t, or we just do it and expect the caller to
 * do the right thing.  The only negative value that we reserve as an
 * error indication is -1, and this is also an unreasonable input
 * value on any sane, von-Neumann machine: there must be at least one
 * byte of code in the address space, and at least for Read, is
 * read-only text, and for Write it is extremely unlikely that any
 * application will want to write out (almost) the entire address
 * space.
 *
 * When -1 is used to indicate an error, errno is set to indicate the
 * error.
 */
struct GioVtbl {
  /*
   * Read virtual fn.  Like read syscall: returns number of bytes
   * actually read, -1 on error, so 0 indcates EOF.  Depending on
   * subclass, there may be short reads even before EOF, but all short
   * reads must return at least one byte, so that this is
   * distinguishable from EOF.
   */
  ssize_t (*Read)(struct Gio  *vself,
                   void       *buf,
                   size_t     count);

  /*
   * Write virtual fn.  Like write syscall: returns number of bytes
   * actually written, -1 on error.  Depending on subclass, there may
   * be short writes.
   */
  ssize_t (*Write)(struct Gio *vself,
                   const void *buf,
                   size_t     count);

  /*
   * Seek virtual function.  Like the lseek syscall.  whence is one of
   * SEEK_SET, SEEK_CUR, SEEK_END.  There is no ftell -- use
   * Seek(self, 0, SEEK_CUR) to obtain the current position.  Whether
   * seeking beyond the end of an Gio object and writing results in
   * defined behavior depends on the subclass involved, i.e., some
   * subclasses may grow/extend the object (e.g., file, shared
   * memory), and others may not (e.g., in-memory snapshot).
   */
  off_t   (*Seek)(struct Gio  *vself,
                  off_t       offset,
                  int         whence);

  /* only used for write, 0 on success, -1 on error */
  int     (*Flush)(struct Gio *vself);

  /* returns 0 on success, -1 on error */
  int     (*Close)(struct Gio *vself);

  /*
   * Will implicitly close if not already closed, but no error
   * reporting, other than possibly logging.
   */
  void    (*Dtor)(struct Gio  *vself);
};

struct Gio {
  struct GioVtbl const    *vtbl;
};

struct GioFile {
  struct Gio  base;
  FILE        *iop;
};

struct GioMemoryFile {
  struct Gio  base;
  char        *buffer;
  size_t      len;
  size_t      curpos;
  /* invariant: 0 <= curpos && curpos <= len */
  /* if curpos == len, everything has been read */
};

void GioMemoryFileCtor(struct GioMemoryFile *self, char *buffer, size_t len);

ssize_t GioMemoryFileRead(struct Gio *vself, void *buf, size_t count);

ssize_t GioMemoryFileWrite(struct Gio *vself, const void *buf, size_t count);

off_t GioMemoryFileSeek(struct Gio *vself, off_t offset, int whence);

int GioMemoryFileFlush(struct Gio *vself);

int GioMemoryFileClose(struct Gio *vself);

void GioMemoryFileDtor(struct Gio *vself);

struct GioMemoryFileSnapshot {
  struct GioMemoryFile base;
};

int GioMemoryFileSnapshotCtor(struct GioMemoryFileSnapshot *self, char *fn);

void GioMemoryFileSnapshotDtor(struct Gio *vself);

EXTERN_C_END

#endif  /* GIO_H_ */
