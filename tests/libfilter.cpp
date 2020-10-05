/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#include <cassert>

#include <vector>
#include <string>

extern "C" {

std::string run(std::vector<std::string> const& inputs)
{
	auto result = std::string{"{ "};
	for (auto&& input : inputs) {
		assert(input != "ready");
		result += input + " ";
	}
	result += "}";

	return result;
}

}
