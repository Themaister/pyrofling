#include "lt_decode.hpp"
#include "lt_shuffle.hpp"
#include <algorithm>
#include <assert.h>
#include <string.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif

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

void Decoder::seed_block(unsigned fec_index)
{
	auto &block = encoded_blocks[fec_index];
	block.resolved_indices = index_buffer.get() + index_buffer_offset;
	index_buffer_offset += output_blocks;

	shuffler.begin(output_blocks, num_xor_blocks);
	block.output_index = 0;
	for (unsigned i = 0; i < num_xor_blocks; i++)
	{
		unsigned output_index = shuffler.pick();
		block.output_index ^= output_index;
		auto *fec_mask = output_to_fec_mask + output_index * num_u32_masks_per_output;
		fec_mask[fec_index / 32] |= 1u << (fec_index & 31);
	}
	block.num_unresolved_indices = num_xor_blocks;
}

void Decoder::begin_decode(uint32_t seed, void *data, size_t size,
                           unsigned max_fec_blocks, unsigned num_xor_blocks_)
{
	output_data = static_cast<uint8_t *>(data);
	assert(size % block_size == 0);
	output_blocks = size / block_size;
	num_xor_blocks = num_xor_blocks_;
	decoded_blocks = 0;
	decoded_block_mask.clear();
	decoded_block_mask.resize(output_blocks);
	ready_encoded_links.clear();

	num_u32_masks_per_output = (max_fec_blocks + 31) / 32;

	reserve_indices(max_fec_blocks * output_blocks + num_u32_masks_per_output * output_blocks);
	output_to_fec_mask = index_buffer.get();
	memset(output_to_fec_mask, 0, num_u32_masks_per_output * output_blocks * sizeof(uint32_t));
	index_buffer_offset += num_u32_masks_per_output * output_blocks;

	shuffler.seed(seed);
	shuffler.flush();

	encoded_blocks.clear();
	encoded_blocks.resize(max_fec_blocks);
	for (unsigned i = 0; i < max_fec_blocks; i++)
		seed_block(i);
}

void Decoder::reserve_indices(size_t num_indices)
{
	if (index_buffer_capacity < num_indices)
	{
		if (index_buffer_capacity == 0)
			index_buffer_capacity = 1;
		while (index_buffer_capacity < num_indices)
			index_buffer_capacity *= 2;

		index_buffer.reset(new uint32_t[index_buffer_capacity]);
	}

	index_buffer_offset = 0;
}

#ifdef _MSC_VER
static inline unsigned find_lsb(uint32_t x)
{
	unsigned long result;
	if (_BitScanForward(&result, x))
		return result;
	else
		return 32;
}
#elif defined(__GNUC__)
static inline unsigned find_lsb(uint32_t v)
{
	return __builtin_ctz(v);
}
#else
#error "Missing impl for find_lsb."
#endif

void Decoder::propagate_decoded_block(unsigned output_index)
{
	uint32_t *fec_mask = output_to_fec_mask + output_index * num_u32_masks_per_output;
	for (unsigned i = 0; i < num_u32_masks_per_output; i++)
	{
		uint32_t &mask = fec_mask[i];
		while (mask)
		{
			unsigned bit = find_lsb(mask);
			mask &= ~(1u << bit);

			unsigned fec_index = i * 32u + bit;
			auto &block = encoded_blocks[fec_index];

			if (block.data)
				xor_block(block.data, output_data + block_size * output_index, block_size);
			else
				block.resolved_indices[block.num_resolved_indices++] = output_index;

			block.output_index ^= output_index;

			assert(block.num_unresolved_indices != 0);
			if (--block.num_unresolved_indices == 1 && block.data)
				ready_encoded_links.push_back(fec_index);
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
	if (block.num_unresolved_indices == 0)
		return;

	assert(block.num_unresolved_indices == 1);
	assert(block.data);
	assert(block.output_index < output_blocks);

	if (mark_decoded_block(block.output_index))
	{
		memcpy(output_data + block_size * block.output_index, block.data, block_size);
		propagate_decoded_block(block.output_index);
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

	if (block.num_unresolved_indices == 1)
		ready_encoded_links.push_back(index);

	drain_ready_blocks();
	bool ret = decoded_blocks == output_blocks;

#ifndef NDEBUG
	if (ret)
	{
		for (auto &b : encoded_blocks)
			assert(b.num_unresolved_indices == 0);
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
			assert(b.num_unresolved_indices == 0);
	}
#endif

	return ret;
}
}