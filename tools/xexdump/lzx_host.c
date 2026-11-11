/* SPDX-License-Identifier: GPL-3.0-only
 * Bounded in-memory adapter for the SDK-pinned libmspack LZX decoder.
 * Reference: sdk/src/system/lzx.cpp (ReXGlue/Xenia, BSD-3-Clause).
 * This adapter does not load or execute guest instructions.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mspack.h>
#include <lzx.h>

struct memory_file { unsigned char *data; size_t length, position; };
static int read_memory(struct mspack_file *file, void *buffer, int count) {
  struct memory_file *f = (struct memory_file *)file;
  if (count < 0) return -1;
  size_t n = (size_t)count;
  if (n > f->length - f->position) n = f->length - f->position;
  memcpy(buffer, f->data + f->position, n); f->position += n;
  return (int)n;
}
static int write_memory(struct mspack_file *file, void *buffer, int count) {
  struct memory_file *f = (struct memory_file *)file;
  if (count < 0 || (size_t)count > f->length - f->position) return -1;
  memcpy(f->data + f->position, buffer, (size_t)count); f->position += (size_t)count;
  return count;
}
static void *allocate(struct mspack_system *system, size_t bytes) {
  (void)system; return calloc(bytes, 1);
}
static void copy_memory(void *source, void *dest, size_t bytes) { memcpy(dest, source, bytes); }

int bo2_lzx_decode(const void *source, size_t source_bytes, void *dest, size_t dest_bytes,
                   uint32_t window_bytes, const void *reference, size_t reference_bytes) {
  if (!source || !dest || !source_bytes || !dest_bytes || dest_bytes > 128u*1024*1024 ||
      window_bytes < 32768 || window_bytes > 32u*1024*1024 ||
      (window_bytes & (window_bytes-1)) || reference_bytes > window_bytes ||
      (reference_bytes && !reference)) return -1;
  int bits = 0;
  for (uint32_t n = window_bytes; n > 1; n >>= 1) ++bits;
  struct mspack_system sys = {0};
  sys.read = read_memory; sys.write = write_memory;
  sys.alloc = allocate; sys.free = free; sys.copy = copy_memory;
  struct memory_file input = {(unsigned char *)source, source_bytes, 0};
  struct memory_file output = {(unsigned char *)dest, dest_bytes, 0};
  struct lzxd_stream *stream = lzxd_init(&sys, (struct mspack_file *)&input,
      (struct mspack_file *)&output, bits, 0, 0x8000, (off_t)dest_bytes, 0);
  if (!stream) return -2;
  if (reference) {
    memset(stream->window, 0, window_bytes - reference_bytes);
    memcpy(stream->window + window_bytes - reference_bytes, reference, reference_bytes);
    stream->ref_data_size = window_bytes;
  }
  int result = lzxd_decompress(stream, (off_t)dest_bytes);
  if (!result && output.position != dest_bytes) result = -3;
  lzxd_free(stream);
  return result;
}
