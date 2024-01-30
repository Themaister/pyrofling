#include "lt_encode.hpp"
#include "lt_shuffle.hpp"
#include <string.h>
#include <assert.h>
#include <utility>

namespace HybridLT
{
static void xor_block(uint8_t * __restrict a, const uint8_t * __restrict b, size_t size)
{
	for (size_t i = 0; i < size; i++)
		a[i] ^= b[i];
}

void Encoder::seed(uint32_t seed)
{
	shuffler.seed(seed);
}

void Encoder::flush()
{
	shuffler.flush();
}

void Encoder::generate(void *xor_data_, const void *input_data_, size_t size, unsigned num_xor_blocks)
{
	auto *xor_data = static_cast<uint8_t *>(xor_data_);
	auto *input_data = static_cast<const uint8_t *>(input_data_);

	unsigned input_blocks = (size + block_size - 1) / block_size;
	shuffler.begin(input_blocks, num_xor_blocks);

	for (uint32_t i = 0; i < num_xor_blocks; i++)
	{
		unsigned idx = shuffler.pick();

		if (i == 0)
		{
			if (idx + 1 == input_blocks)
			{
				size_t copy_size = size - idx * block_size;
				memcpy(xor_data, input_data + block_size * idx, copy_size);
				memset(xor_data + copy_size, 0, block_size - copy_size);
			}
			else
				memcpy(xor_data, input_data + block_size * idx, block_size);
		}
		else
		{
			if (idx + 1 == input_blocks)
				xor_block(xor_data, input_data + block_size * idx, size - idx * block_size);
			else
				xor_block(xor_data, input_data + block_size * idx, block_size);
		}
	}
}

void Encoder::set_block_size(size_t size)
{
	block_size = size;
}
}