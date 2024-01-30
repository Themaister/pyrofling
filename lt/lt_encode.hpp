#pragma once

#include <stddef.h>
#include <stdint.h>
#include <random>

namespace LT
{
class Encoder
{
public:
	void set_block_size(size_t size);
	void begin_encode(uint64_t seed, const void *data, size_t size);
	void generate_block(void *data);

private:
	std::default_random_engine rnd;
	size_t block_size = 0;
	const uint8_t *input_data = nullptr;
	size_t input_size = 0;
	size_t input_blocks = 0;
};
}