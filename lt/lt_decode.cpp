#include "lt_decode.hpp"
#include "lt_shuffle.hpp"
#include <algorithm>
#include <assert.h>
#include <string.h>

namespace HybridLT
{
void Decoder::set_block_size(size_t size)
{
	block_size = size;
}

static void xor_block(uint8_t * __restrict a, const uint8_t * __restrict b, size_t size)
{
	for (size_t i = 0; i < size; i++)
		a[i] ^= b[i];
}

void Decoder::seed_block(EncodedLink &block)
{
	block.indices = index_buffer.get() + index_buffer_offset;
	index_buffer_offset += output_blocks;
	block.resolved_indices = index_buffer.get() + index_buffer_offset;
	index_buffer_offset += output_blocks;

	shuffler.begin(output_blocks, num_xor_blocks);
	for (unsigned i = 0; i < num_xor_blocks; i++)
		block.indices[i] = shuffler.pick();
	block.num_indices = num_xor_blocks;
}

void Decoder::begin_decode(uint64_t seed, void *data, size_t size,
                           unsigned max_fec_blocks, unsigned num_xor_blocks_)
{
	output_data = static_cast<uint8_t *>(data);
	assert(size % block_size == 0);
	output_blocks = size / block_size;
	encoded_blocks.clear();
	encoded_blocks.resize(max_fec_blocks);
	num_xor_blocks = num_xor_blocks_;
	decoded_blocks = 0;
	decoded_block_mask.clear();
	decoded_block_mask.resize(output_blocks);
	ready_encoded_links.clear();

	reserve_indices(max_fec_blocks * 2 * output_blocks);

	shuffler.seed(seed);
	shuffler.flush();
	for (auto &b : encoded_blocks)
		seed_block(b);
}

void Decoder::reserve_indices(size_t num_indices)
{
	if (index_buffer_capacity < num_indices)
	{
		if (index_buffer_capacity == 0)
			index_buffer_capacity = 1;
		while (index_buffer_capacity < num_indices)
			index_buffer_capacity *= 2;

		index_buffer.reset(new uint16_t[index_buffer_capacity]);
	}

	index_buffer_offset = 0;
}

void Decoder::propagate_decoded_block(unsigned index)
{
	for (size_t i = 0, n = encoded_blocks.size(); i < n; i++)
	{
		auto &block = encoded_blocks[i];
		auto itr = std::find(block.indices, block.indices + block.num_indices, uint16_t(index));
		if (itr != block.indices + block.num_indices)
		{
			*itr = block.indices[--block.num_indices];

			if (block.data)
				xor_block(block.data, output_data + block_size * index, block_size);
			else
				block.resolved_indices[block.num_resolved_indices++] = uint16_t(index);

			if (block.num_indices == 1 && block.data)
				ready_encoded_links.push_back(i);
		}
	}
}

bool Decoder::mark_decoded_block(unsigned index)
{
	if (decoded_block_mask[index])
		return false;

	decoded_block_mask[index] = true;
	decoded_blocks++;
	return true;
}

void Decoder::drain_ready_block(EncodedLink &block)
{
	// Redundant block.
	if (block.num_indices == 0)
		return;

	assert(block.num_indices == 1);
	assert(block.data);
	size_t decoded_index = block.indices[0];
	block.num_indices = 0;

	if (mark_decoded_block(decoded_index))
	{
		memcpy(output_data + block_size * decoded_index, block.data, block_size);
		propagate_decoded_block(decoded_index);
	}
}

void Decoder::drain_ready_blocks()
{
	while (!ready_encoded_links.empty())
	{
		auto &block = encoded_blocks[ready_encoded_links.back()];
		ready_encoded_links.pop_back();
		drain_ready_block(block);
	}
}

bool Decoder::push_fec_block(unsigned index, void *data)
{
	assert(index < encoded_blocks.size());
	auto &block = encoded_blocks[index];
	block.data = static_cast<uint8_t *>(data);

	for (unsigned i = 0; i < block.num_resolved_indices; i++)
		xor_block(block.data, output_data + block_size * block.resolved_indices[i], block_size);
	block.num_resolved_indices = 0;

	if (block.num_indices == 1)
		ready_encoded_links.push_back(index);

	drain_ready_blocks();
	bool ret = decoded_blocks == output_blocks;

#ifndef NDEBUG
	if (ret)
	{
		for (auto &b : encoded_blocks)
			assert(b.num_indices == 0);
	}
#endif

	return ret;
}

bool Decoder::push_raw_block(unsigned index)
{
	if (mark_decoded_block(index))
		propagate_decoded_block(index);
	drain_ready_blocks();
	bool ret = decoded_blocks == output_blocks;

#ifndef NDEBUG
	if (ret)
	{
		for (auto &b : encoded_blocks)
			assert(b.num_indices == 0);
	}
#endif

	return ret;
}
}