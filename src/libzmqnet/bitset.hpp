#pragma once

#include <cstddef>
#include <climits>

#include <vector>
#include <string>

class unpacked_bitset {

public: // variables
	size_t N;
	std::vector<unsigned char> bytes;

public: // interface
	unpacked_bitset(size_t N_)
		: N{N_}
		, bytes((N + CHAR_BIT - 1) / CHAR_BIT, 0x0)
	{}

	void set(size_t i) {
		if (i >= N) {
			throw std::logic_error("bitset size mismatch");
		}

		bytes[i / CHAR_BIT] |= 1 << (i % CHAR_BIT);
	}

	bool get(size_t const i) const {
		return (bytes[i / CHAR_BIT] >> (i % CHAR_BIT)) & 1;
	}

	auto operator| (unpacked_bitset const& rhs) const {
		assert(N == rhs.N);
		auto result = unpacked_bitset{N};
		for (size_t i = 0; i < N; i++) {
			if (get(i) || rhs.get(i)) {
				result.set(i);
			}
		}
		return result;
	}
	auto& operator|= (unpacked_bitset const& rhs) {
		assert(N == rhs.N);
		for (size_t i = 0; i < N; i++) {
			bytes[i] = (((bytes[i / CHAR_BIT] | (rhs.bytes[i / CHAR_BIT])) >> (i % CHAR_BIT)) & 1);
		}
		return *this;
	}

	bool operator== (unpacked_bitset const& rhs) const {
		for (size_t i = 0; i < bytes.size(); i++) {
			if (bytes[i] != rhs.bytes[i]) {
				return false;
			}
		}
		return true;
	}

	size_t memsize() const {
		return bytes.size();
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
		result.reserve(bytes.size() * 2);
		for (size_t i = 0; i < bytes.size(); i++) {
			result.push_back(hb_to_hex(bytes[i] >> 4));
			result.push_back(hb_to_hex(bytes[i] & 0x0f));
			result.push_back(' ');
		}
		return result;

		throw std::runtime_error("lrle to_string failed");
	}
};

