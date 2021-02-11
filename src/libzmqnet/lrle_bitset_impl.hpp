#pragma once

#include <cassert>
#include <cstring>
#include <utility>
#include <vector>
#include <set>

// For a description of the LRLE bitset implementation, see
// https://connect.us.cray.com/confluence/display/DTOOLS/Rank+Bitset+Representations#RankBitsetRepresentations-2.LimitedRunLengthEncoding
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

	lrle_bitset(std::set<size_t> const& ranks)
		: compressed{}
	{
		for (auto&& rank : ranks) {
			// Create buffer for bitset containing one rank
			auto const rhs_cap = lrle_single_bit_size(rank);
			unsigned char rhs[rhs_cap];

			// Encode single rank bitset
			auto const rhs_len = lrle_encode_single_bit(rank, rhs, rhs_cap);
			if (rhs_len < 0) {
				throw std::runtime_error("lrle single bit encode failed");
			}

			// Create buffer for result and add single rank into bitset
			unsigned char lhs[(compressed.size() + rhs_len) * CHUNK_SIZE];
			auto const lhs_len = lrle_or_default(lhs, sizeof(lhs),
				compressed.data(), compressed.size(), rhs, rhs_len);
			if (lhs_len < 0) {
				throw std::runtime_error("lrle merge failed");
			}

			// Move result into bitset
			compressed = std::vector<unsigned char>{lhs, lhs + lhs_len};
		}
	}

	void clear() {
		compressed.clear();
	}

	auto or_buf(unsigned char const* rhs, size_t rhs_len) const {
		unsigned char buf[(compressed.size() + rhs_len) * CHUNK_SIZE];

		auto const len = lrle_or_default(buf, sizeof(buf),
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

	auto and_buf(unsigned char const* rhs, size_t rhs_len) const {
		unsigned char buf[(compressed.size() + rhs_len) * CHUNK_SIZE];

		auto const len = lrle_and_default(buf, sizeof(buf),
			compressed.data(), compressed.size(), rhs, rhs_len);
		if (len < 0) {
			throw std::runtime_error("lrle and failed");
		}

		return lrle_bitset{buf, (size_t)len};
	}

	auto eq_buf(unsigned char const* rhs, size_t rhs_len) const {
		return (compressed.size() == rhs_len)
			&& (::memcmp(compressed.data(), rhs, rhs_len) == 0);
	}

	bool operator== (lrle_bitset const& rhs) const {
		return eq_buf(rhs.compressed.data(), rhs.compressed.size());
	}

	bool operator!= (lrle_bitset const& rhs) const {
		return !(*this == rhs);
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

	std::string to_string() const {
		auto hb_to_hex = [](unsigned char const halfbyte) {
			if (halfbyte < 10U) {
				return (char)('0' + halfbyte);
			} else {
				return (char)('a' + (halfbyte - 10U));
			}
		};
		std::string result;
		result.reserve(compressed.size() * 2);
		for (size_t i = 0; i < compressed.size(); i++) {
			result.push_back(hb_to_hex(compressed[i] >> 4));
			result.push_back(hb_to_hex(compressed[i] & 0x0f));
			result.push_back(' ');
		}
		return result;
	}
};
