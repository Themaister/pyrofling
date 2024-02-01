#include "lt_encode.hpp"
#include "lt_lut.hpp"
#include <string.h>
#include <assert.h>
#include <utility>

// See David J. C. MacKay - Information Theory, Inference and Learning Algorithms - Chapter 50 (2004) for details.

namespace LT
{
void Encoder::begin_encode(uint64_t seed, const void *data, size_t size)
{
	rnd.seed(seed);
	input_data = static_cast<const uint8_t *>(data);
	input_size = size;
	input_blocks = (input_size + block_size - 1) / block_size;
}

static void xor_block(uint8_t * __restrict a, const uint8_t * __restrict b, size_t size)
{
	for (size_t i = 0; i < size; i++)
		a[i] ^= b[i];
}

void Encoder::generate_block(void *data_, unsigned num_xor_blocks)
{
	auto *data = static_cast<uint8_t *>(data_);
	assert(num_xor_blocks <= input_blocks);

	uint32_t unused_input_blocks = input_blocks;
	uint32_t l[MaxNumBlocks];
	for (uint32_t i = 0; i < input_blocks; i++)
		l[i] = i;

	for (uint32_t i = 0; i < num_xor_blocks; i++)
	{
		uint32_t block_index = uint32_t(rnd()) % unused_input_blocks;
		auto &idx = l[block_index];

		if (i == 0)
		{
			if (idx + 1 == input_blocks)
				memcpy(data, input_data + block_size * idx, input_size - idx * block_size);
			else
				memcpy(data, input_data + block_size * idx, block_size);
		}
		else
		{
			if (idx + 1 == input_blocks)
				xor_block(data, input_data + block_size * idx, input_size - idx * block_size);
			else
				xor_block(data, input_data + block_size * idx, block_size);
		}

		idx = l[--unused_input_blocks];
	}
}

void Encoder::set_block_size(size_t size)
{
	block_size = size;
}
}