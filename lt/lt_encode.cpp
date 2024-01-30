#include "lt_encode.hpp"

namespace LT
{
void Encoder::begin_encode(uint64_t seed, const void *data, size_t size)
{
	rnd.seed(seed);
	input_data = static_cast<const uint8_t *>(data);
	input_size = size;
	input_blocks = (input_size + block_size - 1) / block_size;
}

void Encoder::generate_block(void *data_)
{
	auto *data = static_cast<uint8_t *>(data_);

}

void Encoder::set_block_size(size_t size)
{
	block_size = size;
}
}