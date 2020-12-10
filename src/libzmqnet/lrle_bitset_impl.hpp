#pragma once

#include <cassert>
#include <cstring>
#include <utility>
#include <vector>

#include "lrle_bitset.h"

struct lrle_bitset
{
	std::vector<unsigned char> compressed;

	lrle_bitset() {}

	lrle_bitset(unsigned char const* buf, size_t len)
		: compressed{buf, buf + len}
	{}

	lrle_bitset(std::vector<unsigned char> const& compressed_)
		: compressed{compressed_}
	{}

	lrle_bitset(std::vector<unsigned char>&& expiring)
		: compressed{std::move(expiring)}
	{}

	// Optimized single rank constructor
	lrle_bitset(size_t n) {
		auto const cap = lrle_single_bit_size(n);
		unsigned char buf[cap];

		auto const len = lrle_encode_single_bit(n, buf, cap);
		if (len < 0) {
			throw std::runtime_error("lrle single bit encode failed");
		}

		compressed = std::vector<unsigned char>{buf, buf + len};
	}

	auto or_buf(unsigned char const* rhs, size_t rhs_len) const {
		unsigned char buf[(compressed.size() + rhs_len) * CHUNK_SIZE];

		auto const len = lrle_merge_default(buf, sizeof(buf),
			compressed.data(), compressed.size(), rhs, rhs_len);
		if (len < 0) {
			throw std::runtime_error("lrle merge failed");
		}

		return lrle_bitset{buf, (size_t)len};
	}

	auto operator| (lrle_bitset const& rhs) const {
		return or_buf(rhs.compressed.data(), rhs.compressed.size());
	}

	void or_eq_buf(unsigned char const* rhs, size_t rhs_len) {
		compressed = std::move(or_buf(rhs, rhs_len).compressed);
	}

	auto& operator|= (lrle_bitset const& rhs) {
		or_eq_buf(rhs.compressed.data(), rhs.compressed.size());
		return *this;
	}

	bool operator== (lrle_bitset const& rhs) const {
		if (compressed.size() != rhs.compressed.size()) {
			return false;
		}
		return compressed == rhs.compressed;
	}

	bool empty() const {
		return lrle_empty(compressed.data(), compressed.size());
	}

	operator bool() const {
		return lrle_any(compressed.data(), compressed.size());
	}

	size_t memsize() const {
		return compressed.size();
	}
};
