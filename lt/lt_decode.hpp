#pragma once
#include <random>
#include <vector>
#include <stddef.h>
#include "lt_lut.hpp"

namespace LT
{
class Decoder
{
public:
	void set_block_size(size_t size);
	void begin_decode(uint64_t seed, void *data, size_t size, size_t max_seq_blocks);
	bool push_block(size_t seq, void *data);

private:
	std::default_random_engine rnd;
	size_t block_size = 0;
	uint8_t *output_data = nullptr;
	size_t output_size = 0;
	size_t output_blocks = 0;
	size_t decoded_blocks = 0;

	struct EncodedLink
	{
		uint8_t *data = nullptr;
		uint16_t indices[MaxXorBlocks];
		unsigned num_indices = 0;
		uint16_t resolved_indices[MaxXorBlocks];
		unsigned num_resolved_indices = 0;
	};
	std::vector<unsigned> ready_encoded_links;
	std::vector<EncodedLink> encoded_blocks;
	std::vector<bool> decoded_block_mask;
	void seed_block(EncodedLink &block);
	void drain_ready_blocks();
	void drain_ready_block(EncodedLink &block);
	void mark_decoded_block(size_t index);
};
}