#pragma once

#include <stddef.h>
#include <stdint.h>
#include <random>
#include "lt_shuffle.hpp"

namespace HybridLT
{
class Encoder
{
public:
	void set_block_size(size_t size);
	void seed(uint64_t seed);
	void flush();
	void generate(void *xor_data, const void *input_data, size_t size, unsigned num_xor_blocks);

private:
	Shuffler shuffler;
	size_t block_size = 0;
};
}