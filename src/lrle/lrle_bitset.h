/******************************************************************************\
 * Copyright 2019 Cray Inc.  All Rights Reserved.
 *
 * Unpublished Proprietary Information.
 * This unpublished work is protected to trade secret, copyright and other laws.
 * Except as permitted by contract or express written permission of Cray Inc.,
 * no part of this work or its content may be used, reproduced or disclosed
 * in any form.
 *
 ******************************************************************************/

#ifndef LRLE_BITSET_H_
#define LRLE_BITSET_H_

#include <sys/types.h>

#define CHUNK_SIZE 4

#ifdef __cplusplus
extern "C" {
#endif

ssize_t lrle_compress_default(
	unsigned char*       __restrict dst_bytes, size_t dst_cap,
	unsigned char const* __restrict src_bytes, size_t src_len);

ssize_t lrle_decompress_default(
	unsigned char*       __restrict dst_bytes, size_t dst_cap,
	unsigned char const* __restrict src_bytes, size_t src_len);

ssize_t lrle_or_default(
	unsigned char* __restrict dst_bytes, size_t dst_cap,
	unsigned char const* __restrict lhs_bytes, size_t lhs_len,
	unsigned char const* __restrict rhs_bytes, size_t rhs_len);

ssize_t lrle_and_default(
	unsigned char* __restrict dst_bytes, size_t dst_cap,
	unsigned char const* __restrict lhs_bytes, size_t lhs_len,
	unsigned char const* __restrict rhs_bytes, size_t rhs_len);

ssize_t lrle_single_bit_size(size_t n);

ssize_t lrle_encode_single_bit(size_t n,
	unsigned char* __restrict dst_bytes, size_t dst_cap);

bool lrle_any_intersection(
	unsigned char const* __restrict lhs_bytes, size_t lhs_len,
	unsigned char const* __restrict rhs_bytes, size_t rhs_len);

struct lrle_context
{
	int in_chunk;
	unsigned char const* cursor;
	size_t pos, offset, parity;
};
int reset_lrle_context(lrle_context* context);

// Return index of next bit set in encoded bitset.
// If result is negative, bitset is completed.
// Current progress is stored in `context`, if `context` is NULL, the first set bit is returned.
ssize_t lrle_next_set_bit(unsigned char const* src_bytes, size_t src_len, lrle_context* context);

size_t lrle_count(unsigned char const* __restrict src_bytes, size_t src_len);

bool lrle_contains(unsigned char const* __restrict src_bytes, size_t src_len, size_t idx);

bool lrle_empty(unsigned char const* __restrict src_bytes, size_t src_len);

// Return 0 if not full, otherwise count
size_t lrle_full(unsigned char const* __restrict src_bytes, size_t src_len);

bool lrle_any(unsigned char const* __restrict src_bytes, size_t src_len);

#ifdef __cplusplus
}
#endif

#endif
