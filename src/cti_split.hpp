/******************************************************************************\
 * cti_split.hpp - Header file for splitting strings.
 *
 * Copyright 2019-2020 Hewlett Packard Enterprise Development LP.
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 ******************************************************************************/

#pragma once

#include <sstream>
#include <array>
#include <utility>

namespace cti {

namespace split {

    static inline std::string removeLeadingWhitespace(const std::string& str, const std::string& whitespace = " \t") {
        const auto startPos = str.find_first_not_of(whitespace);
        if (startPos == std::string::npos) return "";
        const auto endPos = str.find_last_not_of(whitespace);
        return str.substr(startPos, endPos - startPos + 1);
        }

    template <std::size_t N>
    static inline std::array<std::string, N> string(std::string const& line, char delim = ' ') {
        auto result = std::array<std::string, N>{};
        auto linestream = std::stringstream{line};
        auto token = std::string{};

        auto cursor = std::size_t{};
        for (cursor = 0; cursor < N - 1; cursor++) {
            if (std::getline(linestream, token, delim)) {
                result[cursor] = std::move(token);
            } else {
                break;
            }
        }

        std::getline(linestream, result[cursor]);

        return result;
    }

} /* namespace cti::split */

} /* namespace cti */
