#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <tuple>
#include <algorithm>

#include "lrle_bitset.h"

using Tag = uint;
using Len = uint;
using byte = unsigned char;

namespace {

#if 0
#define LOG fprintf
#define ASSERT assert
#define USE_LOG
#else
#define LOG(...)
#define ASSERT(...)
#endif

}

auto constexpr static TAG_SIZE = 8;
auto constexpr static LEN_SIZE = (CHUNK_SIZE - 1) * 8;
auto constexpr static MAX_RUN  = (1 << LEN_SIZE) - 1;
auto constexpr static BITS_PER_CHUNK = CHUNK_SIZE * 8;

auto constexpr static canary = byte{0xcc};

struct Chunk
{
	union
	{
		byte bytes[CHUNK_SIZE];
		struct
		{
			Tag tag : TAG_SIZE __attribute__((packed));
			Len len : LEN_SIZE __attribute__((packed));
		};
	};

#if (CHUNK_SIZE == 2)
	constexpr Chunk() : bytes{0x00, 0x00} {}
#elif (CHUNK_SIZE == 4)
	constexpr Chunk() : bytes{0x00, 0x00, 0x00, 0x00} {}
#else
	#error "Implement trivial constructor for this chunk size"
#endif

	struct run_tag {};
	constexpr Chunk(Tag tag_, Len len_, run_tag) : tag{tag_}, len{len_} {}

	constexpr static Chunk run(Tag tag, Len len) {
		return Chunk{tag, len, run_tag{}};
	}

	struct raw_tag {};
	template <typename... Args>
	constexpr Chunk(raw_tag, Args&&... bytes_) : bytes{std::forward<Args>(bytes_)...} {}

	template <typename... Args>
	constexpr static Chunk raw(Args&&... bytes) {
		return Chunk{raw_tag{}, std::forward<Args>(bytes)...};
	}

	constexpr static Chunk raw(byte const* bytes) {
#if (CHUNK_SIZE == 2)
		return Chunk{raw_tag{}, bytes[0], bytes[1]};
#elif (CHUNK_SIZE == 4)
		return Chunk{raw_tag{}, bytes[0], bytes[1], bytes[2], bytes[3]};
#else
		#error "Implement byte pointer constructor for this chunk size"
#endif
	}

	constexpr bool is_run() const { return (tag == 0x00) || (tag == 0xff); }

	constexpr Chunk operator| (Chunk const rhs) const {
		Chunk c;
		for (Len i = 0; i < CHUNK_SIZE; i++) {
			c.bytes[i] = bytes[i] | rhs.bytes[i];
		}
		return c;
	}

	constexpr Chunk operator& (Chunk const rhs) const {
		Chunk c;
		for (Len i = 0; i < CHUNK_SIZE; i++) {
			c.bytes[i] = bytes[i] & rhs.bytes[i];
		}
		return c;
	}

	constexpr bool empty() const {
		for (Len i = 0; i < CHUNK_SIZE; i++) {
			if (bytes[i]) {
				return false;
			}
		}
		return true;
	}

	constexpr bool disjoint(Chunk const rhs) const {
		for (Len i = 0; i < CHUNK_SIZE; i++) {
			if (bytes[i] & rhs.bytes[i]) {
				return false;
			}
		}
		return true;
	}
};

static_assert(sizeof(Chunk) == CHUNK_SIZE);

static auto constexpr nextParity(Len const parity, Len const len)
{
	return ((parity + len) & (CHUNK_SIZE - 1));
}

#ifndef USE_LOG
	constexpr
#endif
static void logChunk(FILE* file, Chunk const& c)
{
	for (int i = 0; i < CHUNK_SIZE; i++) {
		LOG(file, i ? " %02x" : "%02x", c.bytes[i]);
	}
}

struct OutputStream
{
	Len parity;
	Chunk *tail_begin, *tail_end;
	Chunk *tail;

	OutputStream(byte* tail_, size_t const tail_len)
		: parity{0}
		, tail_begin{(Chunk*)tail_}
		, tail_end{(Chunk*)(tail_ + tail_len)}
		, tail{tail_begin}
	{
		ASSERT(tail_len >= CHUNK_SIZE);

		*tail = Chunk{};
	}

#ifndef USE_LOG
	constexpr
#endif
	int append(Chunk const c)
	{
		if (tail >= tail_end) {
			return -1;
		}

		LOG(stderr, "\tappend: (");
		logChunk(stderr, *tail);
		LOG(stderr,", %d) <-", parity);
		logChunk(stderr, c);
		LOG(stderr, ": ");

		// Starting output
		if (tail->empty()) {
			ASSERT(parity == 0);

			*tail = c;

			// If started a run, update parity
			if (c.is_run()) {
				parity = c.len % CHUNK_SIZE;
			}
		} else if (c.is_run()) {

			// Combine common runs
			if (tail->is_run() && (c.tag == tail->tag)) {

				// Check for length overflow
				auto const len_sum = uint64_t{c.len} + tail->len;

				// Run must be encoded in 2 chunks
				if (len_sum > MAX_RUN) {
					tail->len = MAX_RUN;
					tail++;
					tail->tag = c.tag;
					tail->len = len_sum - MAX_RUN;

				// Run can be encoded in 1 chunk
				} else {
					tail->len += c.len;
				}

			// Append run onto raw or nonmatching run
			} else {
				tail++;
				*tail = c;
			}

			parity = nextParity(parity, c.len);
		} else {

			// Append raw onto run
			if (tail->is_run()) {
				for (Len i = 0; i < parity; i++) {
					ASSERT(c.bytes[i] == canary);
				}

			// Append raw onto raw
			} else {
				ASSERT(parity == 0);
			}

			tail++;
			*tail = c;

			parity = 0;
		}


		LOG(stderr,"(");
		logChunk(stderr, *tail);
		LOG(stderr,", %d)\n", parity);

		return 0;
	}


#ifndef USE_LOG
	constexpr
#endif
	void trimTrailingZeroRuns()
	{
		while ((tail->tag == 0x00) && (tail > tail_begin)) {
			LOG(stderr, "trim: ");
			for (Len i = 0; i < CHUNK_SIZE; i++) {
				LOG(stderr, "%02x ", tail->bytes[i]);
			}
			LOG(stderr, "\n");

			tail--;
		}
	}

	constexpr size_t length() const
	{
		if (tail->empty()) {
			return 0;
		}

		return (byte*)(tail + 1) - (byte*)tail_begin;
	}
};

struct InputStream
{
	Chunk scratch;
	Chunk const *head_begin;
	Chunk const *head_end;
	Chunk const *head;

	constexpr bool more_chunks() const
	{
		return head < head_end;
	}

	constexpr bool done() const
	{
		return !more_chunks() && scratch.empty();
	}

	constexpr int read_next()
	{
		if (more_chunks()) {
			scratch = *head;
			head++;

			return 0;

		} else {
			if (scratch.empty()) {
				return -1;
			}

			scratch = Chunk{};
			return 0;
		}
	}

	InputStream(byte const* head_, size_t const head_len)
		: scratch{}
		, head_begin{(Chunk const*)head_}
		, head_end{(Chunk const*)(head_ + head_len)}
		, head{head_begin}
	{
		read_next();
	}

	constexpr Chunk takeRunAmount(int const len)
	{
		ASSERT(scratch.is_run());

		auto const c = Chunk::run(scratch.tag, len);

		if (scratch.len > len) {
			scratch.len -= len;
		} else {
			ASSERT(scratch.len == len);
			read_next();
		}

		return c;
	}

	constexpr Chunk takeRaw(Len const parity)
	{
		Chunk c;
		Len at = 0;

		while (at < CHUNK_SIZE) {
			if (done()) {
				return c;
			}

			if (scratch.is_run()) {
				auto const len = std::min(CHUNK_SIZE - at, scratch.len);
				for (Len i = 0; i < len; i++) {
					c.bytes[at + i] = scratch.tag;
				}

				at += len;

				if (scratch.len > len) {
					scratch.len -= len;
				} else if (scratch.len == len) {
					read_next();
				} else {
					ASSERT(false);
				}
			} else {
				for (Len i = 0; i < at; i++) {
					ASSERT(scratch.bytes[i] == canary);
				}

				for (Len i = at; i < CHUNK_SIZE; i++) {
					c.bytes[i] = scratch.bytes[i];
				}

				at = CHUNK_SIZE;

				read_next();
			}
		}

		return c;
	}

};

static auto mergeRunAmount(Len const parity,
	InputStream const& lhs, InputStream const& rhs)
{
	if (!lhs.scratch.is_run() || !rhs.scratch.is_run()) {
		return Len{0};
	}
	ASSERT(lhs.scratch.is_run() && rhs.scratch.is_run());

	auto const len = std::min(lhs.scratch.len, rhs.scratch.len);
	auto const parityIfAllMerged = nextParity(parity, len);

	if (len < parityIfAllMerged) {
		return Len{0};
	}

	if ((!lhs.more_chunks() && (lhs.scratch.len == len))
	 || (!rhs.more_chunks() && (rhs.scratch.len == len))) {
		return len;
	}

	return len - parityIfAllMerged;
}

static void stream_or(OutputStream& outputStream,
	InputStream& lhs, InputStream& rhs)
{
	if (auto const len = mergeRunAmount(outputStream.parity, lhs, rhs)) {
		LOG(stderr, "merge run ");
		logChunk(stderr, lhs.scratch);
		LOG(stderr, ", run ");
		logChunk(stderr, rhs.scratch);
		LOG(stderr, "\n");

		auto const runA = lhs.takeRunAmount(len);
		auto const runB = rhs.takeRunAmount(len);

		auto const runC = Chunk::run(runA.tag | runB.tag, len);

		outputStream.append(runC);
	} else {
		LOG(stderr, "merge ");
		logChunk(stderr, lhs.scratch);
		LOG(stderr, ", ");
		logChunk(stderr, rhs.scratch);
		LOG(stderr, " -> ");

		auto const rawA = lhs.takeRaw(outputStream.parity);
		auto const rawB = rhs.takeRaw(outputStream.parity);

		auto const rawC = rawA | rawB;

		logChunk(stderr, rawC);
		LOG(stderr, "\n");

		for (Len at = 0; at < CHUNK_SIZE; at++) {
			if ((rawC.bytes[at] == 0x00) || (rawC.bytes[at] == 0xff)) {
				auto const c = Chunk::run(rawC.bytes[at], 1);

				outputStream.append(c);
			} else {
				ASSERT(at == outputStream.parity);

				Chunk c = rawC;
				for (Len i = 0; i < at; i++) {
					c.bytes[i] = canary;
				}

				at = CHUNK_SIZE;

				outputStream.append(c);
			}
		}
	}

	LOG(stderr, "\t");
	logChunk(stderr, lhs.scratch);
	LOG(stderr, ",");
	logChunk(stderr, rhs.scratch);
	LOG(stderr, "\n");
}

static void stream_and(OutputStream& outputStream,
	InputStream& lhs, InputStream& rhs)
{
	if (auto const len = mergeRunAmount(outputStream.parity, lhs, rhs)) {
		LOG(stderr, "and run ");
		logChunk(stderr, lhs.scratch);
		LOG(stderr, ", run ");
		logChunk(stderr, rhs.scratch);
		LOG(stderr, "\n");

		auto const runA = lhs.takeRunAmount(len);
		auto const runB = rhs.takeRunAmount(len);

		auto const runC = Chunk::run(runA.tag & runB.tag, len);

		outputStream.append(runC);
	} else {
		LOG(stderr, "and ");
		logChunk(stderr, lhs.scratch);
		LOG(stderr, ", ");
		logChunk(stderr, rhs.scratch);
		LOG(stderr, " -> ");

		auto const rawA = lhs.takeRaw(outputStream.parity);
		auto const rawB = rhs.takeRaw(outputStream.parity);

		auto const rawC = rawA & rawB;

		logChunk(stderr, rawC);
		LOG(stderr, "\n");

		for (Len at = 0; at < CHUNK_SIZE; at++) {
			if ((rawC.bytes[at] == 0x00) || (rawC.bytes[at] == 0xff)) {
				auto const c = Chunk::run(rawC.bytes[at], 1);

				outputStream.append(c);
			} else {
				ASSERT(at == outputStream.parity);

				Chunk c = rawC;
				for (Len i = 0; i < at; i++) {
					c.bytes[i] = canary;
				}

				at = CHUNK_SIZE;

				outputStream.append(c);
			}
		}
	}

	LOG(stderr, "\t");
	logChunk(stderr, lhs.scratch);
	LOG(stderr, ",");
	logChunk(stderr, rhs.scratch);
	LOG(stderr, "\n");
}

constexpr ssize_t num_ranks(byte const* __restrict src_bytes, size_t src_len)
{
	ssize_t total = 0;
	Len parity = 0;

	for (auto src = src_bytes; src < src_bytes + src_len;) {
		auto const c = Chunk::raw(src);
		if (c.is_run()) {
			ASSERT(c.len > 0);

			auto const len = c.len;

			total += len * 8;
			parity = nextParity(parity, len);

			src += sizeof(Chunk);
		} else {
			for (Len i = 0; i < parity; i++) {
				ASSERT(src->bytes[i] == canary);
			}

			total += (CHUNK_SIZE - parity) * 8;
			parity = 0;

			src += sizeof(Chunk);
		}
	}

	return total;
}

ssize_t lrle_compress_default(byte* __restrict dst_bytes, size_t dst_cap, byte const* __restrict src_bytes_begin, size_t src_len)
{
	LOG(stderr, "start compress\n");

	auto outputStream = OutputStream{dst_bytes, dst_cap};

	auto const src_bytes_end = src_bytes_begin + src_len;

	for (auto src_bytes = src_bytes_begin; src_bytes < src_bytes_end;) {
		if (outputStream.tail < outputStream.tail_end) {

			if ((*src_bytes == 0x00) || (*src_bytes == 0xff)) {
				auto c = Chunk::run(*src_bytes, 1);
				src_bytes++;

				while ((*src_bytes == c.tag) && (src_bytes < src_bytes_end)) {
					c.len++;
					src_bytes++;
				}

				outputStream.append(c);
			} else {
				auto c = Chunk{};

				for (Len i = 0; i < outputStream.parity; i++) {
					c.bytes[i] = canary;
				}
				for (Len i = 0; i < (CHUNK_SIZE - outputStream.parity); i++) {
					c.bytes[outputStream.parity + i] = src_bytes[i];
				}

				src_bytes += (CHUNK_SIZE - outputStream.parity);

				outputStream.append(c);
			}
		} else {
			return -1;
		}
	}

	outputStream.trimTrailingZeroRuns();

	auto const result_len = outputStream.length();

	LOG(stderr, "compress done:\n\t");
	for (size_t i = 0; i < result_len; i++) {
		LOG(stderr, "%02x ", dst_bytes[i]);
	}
	LOG(stderr, "len: %ld ranks: %ld\n\n", result_len, num_ranks(dst_bytes, result_len));

	return result_len;
}

ssize_t lrle_decompress_default(byte* __restrict dst_bytes, size_t dst_cap, byte const* __restrict src_bytes, size_t src_len)
{
	auto dst = dst_bytes;
	Len parity = 0;

	for (auto src = (Chunk*)src_bytes; src < (Chunk*)(src_bytes + src_len);) {
		if (dst < dst_bytes + dst_cap) {

			if (src->is_run()) {
				ASSERT(src->len > 0);

				auto const len = src->len;

				::memset(dst, src->tag, len);
				dst += src->len;
				parity = nextParity(parity, len);

				src++;
			} else {
				for (Len i = 0; i < parity; i++) {
					ASSERT(src->bytes[i] == canary);
				}

				for (Len i = parity; i < CHUNK_SIZE; i++) {
					*dst = src->bytes[i];
					dst++;
				}
				parity = 0;

				src++;
			}

		} else {
			return -1;
		}
	}

	return dst - dst_bytes;
}

ssize_t lrle_or_default(
	unsigned char* __restrict dst_bytes, size_t dst_cap,
	unsigned char const* __restrict lhs_bytes, size_t lhs_len,
	unsigned char const* __restrict rhs_bytes, size_t rhs_len)
{
	LOG(stderr, "start or: \n\t");
	auto const lhs_ranks = num_ranks(lhs_bytes, lhs_len);
	for (size_t i = 0; i < lhs_len; i++) {
		LOG(stderr, "%02x ", lhs_bytes[i]);
	}
	LOG(stderr, ": %ld\n\t", lhs_ranks);
	auto const rhs_ranks = num_ranks(rhs_bytes, rhs_len);
	for (size_t i = 0; i < rhs_len; i++) {
		LOG(stderr, "%02x ", rhs_bytes[i]);
	}
	LOG(stderr, ": %ld\n", rhs_ranks);

	if ((lhs_len == 0) && (rhs_len == 0)) {
		return 0;
	}

	auto outputStream = OutputStream{dst_bytes, dst_cap};

	auto lhs = InputStream{lhs_bytes, lhs_len};
	auto rhs = InputStream{rhs_bytes, rhs_len};

	while (!lhs.done() && !rhs.done()) {

		if (outputStream.tail < outputStream.tail_end) {
			stream_or(outputStream, lhs, rhs);
		} else {
			return -1;
		}

		while (lhs.scratch.is_run() && (lhs.scratch.len == 0) && (!lhs.done())) { lhs.read_next(); }
		while (rhs.scratch.is_run() && (rhs.scratch.len == 0) && (!rhs.done())) { rhs.read_next(); }
	}

	while (!lhs.done()) {
		if (outputStream.tail < outputStream.tail_end) {
			outputStream.append(lhs.scratch);
			lhs.read_next();
		} else {
			return -1;
		}
	}

	while (!rhs.done()) {
		if (outputStream.tail < outputStream.tail_end) {
			outputStream.append(rhs.scratch);
			rhs.read_next();
		} else {
			return -1;
		}
	}

	outputStream.trimTrailingZeroRuns();

	auto const result_len = outputStream.length();

	LOG(stderr, "or done:\n\t");
	for (size_t i = 0; i < result_len; i++) {
		LOG(stderr, "%02x ", dst_bytes[i]);
	}
	LOG(stderr, "len: %ld ranks: %ld\n\n", result_len, num_ranks(dst_bytes, result_len));

	return result_len;
}

ssize_t lrle_and_default(
	unsigned char* __restrict dst_bytes, size_t dst_cap,
	unsigned char const* __restrict lhs_bytes, size_t lhs_len,
	unsigned char const* __restrict rhs_bytes, size_t rhs_len)
{
	LOG(stderr, "start and: \n\t");
	auto const lhs_ranks = num_ranks(lhs_bytes, lhs_len);
	for (size_t i = 0; i < lhs_len; i++) {
		LOG(stderr, "%02x ", lhs_bytes[i]);
	}
	LOG(stderr, ": %ld\n\t", lhs_ranks);
	auto const rhs_ranks = num_ranks(rhs_bytes, rhs_len);
	for (size_t i = 0; i < rhs_len; i++) {
		LOG(stderr, "%02x ", rhs_bytes[i]);
	}
	LOG(stderr, ": %ld\n", rhs_ranks);

	if ((lhs_len == 0) || (rhs_len == 0)) {
		return 0;
	}

	auto outputStream = OutputStream{dst_bytes, dst_cap};

	auto lhs = InputStream{lhs_bytes, lhs_len};
	auto rhs = InputStream{rhs_bytes, rhs_len};

	while (!lhs.done() && !rhs.done()) {

		if (outputStream.tail < outputStream.tail_end) {
				stream_and(outputStream, lhs, rhs);
		} else {
			return -1;
		}

		while (lhs.scratch.is_run() && (lhs.scratch.len == 0) && (!lhs.done())) { lhs.read_next(); }
		while (rhs.scratch.is_run() && (rhs.scratch.len == 0) && (!rhs.done())) { rhs.read_next(); }
	}

	outputStream.trimTrailingZeroRuns();

	auto const result_len = outputStream.length();

	LOG(stderr, "and done:\n\t");
	for (size_t i = 0; i < result_len; i++) {
		LOG(stderr, "%02x ", dst_bytes[i]);
	}
	LOG(stderr, "len: %ld ranks: %ld\n\n", result_len, num_ranks(dst_bytes, result_len));

	return result_len;
}

ssize_t lrle_single_bit_size(size_t n)
{
	// Run of zeros before single bit position
	auto const preZeroChunks = (n < TAG_SIZE) ? 0 : ((n + MAX_RUN - 1) / MAX_RUN);

	auto const singleBitChunk = 1;

	// Size of packed bitset in bytes
	auto const packedSize = CHUNK_SIZE * (preZeroChunks + singleBitChunk);

	return packedSize;
}

ssize_t lrle_encode_single_bit(size_t n,
	unsigned char* __restrict dst_bytes, size_t dst_cap)
{
	auto const zeroRunLen = n / TAG_SIZE;

	// The number of chunks that will be filled with MAX_RUN zeros
	auto const fullZeroChunks = zeroRunLen / MAX_RUN;

	// Run length of last non-full zero chunk
	auto const lastZeroRunLen = zeroRunLen - (fullZeroChunks * MAX_RUN);

	auto dst = (Chunk*)dst_bytes;
	auto const dst_end = (Chunk const*)(dst_bytes + dst_cap);
	Len parity = 0;

	if (dst + fullZeroChunks < dst_end) {
		for (uint i = 0; i < fullZeroChunks; i++) {
			*dst = Chunk::run(0x00, MAX_RUN);

			dst++;
		}

		parity = (parity + (fullZeroChunks * MAX_RUN)) % CHUNK_SIZE;
	} else {
		return -1;
	}

	if (lastZeroRunLen > 0) {
		if (dst < dst_end) {
			*dst = Chunk::run(0x00, lastZeroRunLen);
			parity = nextParity(parity, lastZeroRunLen);

			dst++;

		} else {
			return -1;
		}
	}

	auto singleBitChunk = Chunk{};
	for (Len i = 0; i < parity; i++) {
		singleBitChunk.bytes[i] = 0xcc;
	}
	auto const chunkBitPosition  = n % TAG_SIZE;
	singleBitChunk.bytes[parity] = (1 << chunkBitPosition);
	LOG(stderr, "singleBitChunk %lu: ", n);
	for (ssize_t i = 0; i < CHUNK_SIZE; i++) {
		LOG(stderr, "%02x ", singleBitChunk.bytes[i]);
	}
	LOG(stderr, "\n");

	*dst = singleBitChunk;

	auto const result_len = ((byte*)dst + CHUNK_SIZE) - dst_bytes;
	LOG(stderr, "single bit encode done:\n\t");
	for (ssize_t i = 0; i < result_len; i++) {
		LOG(stderr, "%02x ", dst_bytes[i]);
	}
	LOG(stderr, "len: %ld ranks: %ld\n\n", result_len, num_ranks(dst_bytes, result_len));

	return result_len;
}

struct NoOutputStream
{
	Len parity = 0;
	Chunk tail = {};

	void append(Chunk const c)
	{
		// Starting output
		if (tail.empty()) {
			ASSERT(parity == 0);

			tail = c;

			// If started a run, update parity
			if (c.is_run()) {
				parity = c.len % CHUNK_SIZE;
			}
		} else if (c.is_run()) {

			// Combine common runs
			if (tail.is_run() && (c.tag == tail.tag)) {

				// Check for length overflow
				auto const len_sum = uint64_t{c.len} + tail.len;

				// Run must be encoded in 2 chunks
				if (len_sum > MAX_RUN) {
					tail.tag = c.tag;
					tail.len = len_sum - MAX_RUN;

				// Run can be encoded in 1 chunk
				} else {
					tail.len += c.len;
				}

			// Append run onto raw or nonmatching run
			} else {
				tail = c;
			}

			parity = nextParity(parity, c.len);
		} else {

			// Append raw onto run
			if (tail.is_run()) {
				for (Len i = 0; i < parity; i++) {
					ASSERT(c.bytes[i] == canary);
				}

			// Append raw onto raw
			} else {
				ASSERT(parity == 0);
			}

			tail = c;

			parity = 0;
		}
	}
};

bool lrle_any_intersection(
	unsigned char const* __restrict lhs_bytes, size_t lhs_len,
	unsigned char const* __restrict rhs_bytes, size_t rhs_len) {

	if ((lhs_len == 0) || (rhs_len == 0)) {
		return false;
	}

	auto outputStream = NoOutputStream{};

	auto lhs = InputStream{lhs_bytes, lhs_len};
	auto rhs = InputStream{rhs_bytes, rhs_len};

	while (!lhs.done() && !rhs.done()) {

		if (auto const len = mergeRunAmount(outputStream.parity, lhs, rhs)) {
			auto const runA = lhs.takeRunAmount(len);
			auto const runB = rhs.takeRunAmount(len);

			if (runA.tag && runB.tag) { return true; }

			auto const runC = Chunk::run(runA.tag | runB.tag, len);

			outputStream.append(runC);
		} else {
			auto const rawA = lhs.takeRaw(outputStream.parity);
			auto const rawB = rhs.takeRaw(outputStream.parity);

			if (!rawA.disjoint(rawB)) { return true; }

			auto const rawC = rawA | rawB;

			for (Len at = 0; at < CHUNK_SIZE; at++) {
				if ((rawC.bytes[at] == 0x00) || (rawC.bytes[at] == 0xff)) {
					auto const c = Chunk::run(rawC.bytes[at], 1);

					outputStream.append(c);
				} else {
					ASSERT(at == outputStream.parity);

					Chunk c = rawC;
					for (Len i = 0; i < at; i++) {
						c.bytes[i] = canary;
					}

					at = CHUNK_SIZE;

					outputStream.append(c);
				}
			}
		}
	}

	return false;
}

int reset_lrle_context(lrle_context* context) {
	if (context == nullptr) {
		return -1;
	}

	context->in_chunk = 1;
	context->cursor = nullptr;
	context->pos = 0;
	context->offset = 0;
	context->parity = 0;

	return 0;
}

ssize_t lrle_next_set_bit(unsigned char const* src_bytes, size_t src_len, lrle_context* context)
{
	lrle_context _context;
	if (context == nullptr) {
		context = &_context;
		reset_lrle_context(context);
	}

	if (context->cursor == nullptr) {
		context->cursor = src_bytes;
	}

	auto const src_end = (src_bytes + src_len);

	while (true) {
		if (context->cursor >= src_end) {
			return -1;
		}

		LOG(stderr, "in chunk: %d, cursor: %02x %02x %02x %02x, offset %lu, pos %lu, parity %lu\n",
			context->in_chunk, context->cursor[0], context->cursor[1], context->cursor[2], context->cursor[3], context->offset, context->pos, context->parity);

		if (context->in_chunk) {
			auto const src = (Chunk*)context->cursor;

			if (src->is_run()) {
				auto const len = src->len;
				if (src->tag == 0xff) {
					if (context->offset < (len * 8)) {
						context->offset++;
						return context->pos++;
					} else {
						context->cursor += sizeof(Chunk);
						context->offset = 0;
						context->parity = nextParity(context->parity, len);
					}
				} else {
					context->cursor += sizeof(Chunk);
					context->pos += (len * 8);
					context->offset = 0;
					context->parity = nextParity(context->parity, len);
				}
			} else {
				context->in_chunk = 0;
				context->offset += (context->parity * 8);
				context->parity = 0;
			}
		} else {
			if (context->offset < BITS_PER_CHUNK) {
				auto const rawByte = context->cursor[context->offset / 8];
				if (rawByte == 0x00) {
					context->pos += 8;
					context->offset += 8;
				} else if (rawByte & (1 << (context->offset % 8))) {
					context->offset++;
					return context->pos++;
				} else {
					context->offset++;
					context->pos++;
				}
			} else {
				context->in_chunk = 1;
				context->cursor += sizeof(Chunk);
				context->offset = 0;
			}
		}
	}

	return -1;
}

size_t lrle_count(unsigned char const* __restrict src_bytes, size_t src_len) {
	// TODO: Improve
	auto context = lrle_context{};
	reset_lrle_context(&context);
	auto count = size_t{0};
	while (lrle_next_set_bit(src_bytes, src_len, &context) >= 0) {
		count++;
	}
	return count;
}

bool lrle_contains(unsigned char const* __restrict src_bytes, size_t src_len, size_t idx) {
	auto context = lrle_context{};
	reset_lrle_context(&context);
	while (true) {
		auto next_bit = lrle_next_set_bit(src_bytes, src_len, &context);
		if ((next_bit < 0) || (next_bit > idx)) {
			return false;
		} else if (next_bit == idx) {
			return true;
		}
	}
}

bool lrle_empty(unsigned char const* __restrict src_bytes, size_t src_len) {
	return lrle_next_set_bit(src_bytes, src_len, nullptr) < 0;
}

size_t lrle_full(unsigned char const* __restrict src_bytes, size_t src_len) {
	if (src_len == 0) {
		return 0;
	}

	auto count = size_t{0};
	auto src = (Chunk const*)src_bytes;
	auto src_end = (Chunk const*)(src_bytes + src_len);
	auto parity = Len{0};

	while (src < src_end) {
		auto const c = *(src++);

		// Last chunk could be padded 0xff
		if (!c.is_run()) {
			if ((src < src_end - 1) || (parity > 0)) {
				return 0;
			}

			// Check that non-canary bits are 0xff
			for (Len i = parity; i < CHUNK_SIZE; i++) {
				if (c.bytes[i] != 0xff) {
					return 0;
				}

				// Add 8 bits (0xff) to count
				count += 8;
			}

			return count;
		}

		if (c.tag == 0x00) {
			return 0;
		}

		// Add 8 bits (0xff) per len to count
		count += c.len * 8;
	}

	return count;
}

bool lrle_any(unsigned char const* __restrict src_bytes, size_t src_len) {
	return !lrle_empty(src_bytes, src_len);
}
