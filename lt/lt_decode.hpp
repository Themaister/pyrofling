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
	void begin_decode(uint64_t seed, void *data, size_t size,
	                  unsigned max_fec_blocks, unsigned num_xor_blocks);
	bool push_fec_block(unsigned index, void *data);
	// Mark that data in begin_decode now contains valid data, back-propagate through XOR blocks.
	bool push_raw_block(unsigned index);

private:
	std::default_random_engine rnd;
	size_t block_size = 0;
	uint8_t *output_data = nullptr;
	unsigned output_blocks = 0;
	unsigned decoded_blocks = 0;
	unsigned num_xor_blocks = 0;

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
	bool mark_decoded_block(unsigned index);
	void propagate_decoded_block(unsigned index);
};
}