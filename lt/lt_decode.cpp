#include "lt_decode.hpp"
#include "lt_lut.hpp"
#include <algorithm>
#include <assert.h>
#include <string.h>

// See David J. C. MacKay - Information Theory, Inference and Learning Algorithms - Chapter 50 (2004) for details.

namespace LT
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

// Random select N unique elements in range [0, K) where N is random over the range [1, K].
static unsigned select_n_from(std::default_random_engine &rnd, uint16_t *indices, unsigned K)
{
	unsigned num_blocks = sample_degree_distribution(rnd() & DistributionMask, get_degree_distribution(K));
	printf("Decode NumBlocks %u\n", num_blocks);
	assert(num_blocks <= K);
	assert(K <= MaxNumBlocks);

	uint32_t l[MaxNumBlocks];
	for (uint32_t i = 0; i < K; i++)
		l[i] = i;

	for (uint32_t i = 0; i < num_blocks; i++)
	{
		uint32_t block_index = uint32_t(rnd()) % K;
		auto &idx = l[block_index];
		K--;
		indices[i] = idx;
		printf("  BlockIndex %u\n", indices[i]);
		idx = l[K];
	}

	return num_blocks;
}

void Decoder::seed_block(EncodedLink &block)
{
	block.num_indices = select_n_from(rnd, block.indices, output_blocks);
}

void Decoder::begin_decode(uint64_t seed, void *data, size_t size, size_t max_seq_blocks)
{
	rnd.seed(seed);
	output_data = static_cast<uint8_t *>(data);
	output_size = size;
	assert(size % block_size == 0);
	output_blocks = size / block_size;
	encoded_blocks.clear();
	encoded_blocks.resize(max_seq_blocks);
	decoded_blocks = 0;
	decoded_block_mask.clear();
	decoded_block_mask.resize(output_blocks);
	ready_encoded_links.clear();

	for (auto &b : encoded_blocks)
		seed_block(b);
}

void Decoder::mark_decoded_block(size_t index)
{
	for (size_t i = 0, n = encoded_blocks.size(); i < n; i++)
	{
		auto &block = encoded_blocks[i];
		auto itr = std::find(block.indices, block.indices + block.num_indices, uint16_t(index));
		if (itr != block.indices + block.num_indices)
		{
			*itr = block.indices[--block.num_indices];

			if (block.data)
			{
				xor_block(block.data, output_data + block_size * index, block_size);
			}
			else
			{
				// If we don't have data for this block yet, defer it.
				assert(block.num_resolved_indices < MaxXorBlocks);
				block.resolved_indices[block.num_resolved_indices++] = uint16_t(index);
			}

			if (block.num_indices == 1 && block.data)
				ready_encoded_links.push_back(i);
		}
	}
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

	if (!decoded_block_mask[decoded_index])
	{
		decoded_block_mask[decoded_index] = true;
		decoded_blocks++;

		memcpy(output_data + block_size * decoded_index, block.data, block_size);
		mark_decoded_block(decoded_index);
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

bool Decoder::push_block(size_t seq, void *data)
{
	assert(seq < encoded_blocks.size());
	auto &block = encoded_blocks[seq];
	block.data = static_cast<uint8_t *>(data);

	for (unsigned i = 0; i < block.num_resolved_indices; i++)
		xor_block(block.data, output_data + block_size * block.resolved_indices[i], block_size);
	block.num_resolved_indices = 0;

	if (block.num_indices == 1)
		ready_encoded_links.push_back(seq);

	drain_ready_blocks();
	return decoded_blocks == output_blocks;
}
}