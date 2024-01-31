#pragma once

#include <stdint.h>

namespace LT
{
static constexpr unsigned NumDistributionTableBits = 8;
static constexpr unsigned NumFractionalBits = 8;
static constexpr unsigned NumDistributionTableEntries = (1u << NumDistributionTableBits) + 1u;
static constexpr unsigned MaxNumBlocks = 1024;
static constexpr uint32_t DistributionMask = (1u << (NumDistributionTableBits + NumFractionalBits)) - 1u;

// Empiric observation based on MaxNumBlocks.
static constexpr unsigned MaxXorBlocks = 16;

const uint16_t *get_degree_distribution(unsigned num_blocks);

static inline unsigned sample_degree_distribution_fixed(uint32_t fractional_index, const uint16_t *distribution)
{
	if (fractional_index >= (1u << (NumDistributionTableBits + NumFractionalBits)))
		return distribution[1u << NumDistributionTableBits] << NumFractionalBits;

	unsigned index = (fractional_index >> NumFractionalBits) & ((1u << NumDistributionTableBits) - 1u);
	unsigned frac = fractional_index & ((1u << NumFractionalBits) - 1u);

	// 8.8 fixed point.
	uint32_t lo = distribution[index];
	uint32_t hi = distribution[index + 1];

	// 8.16 fixed point
	auto l = lo * ((1u << NumFractionalBits) - frac) + hi * frac;

	return l;
}

static inline unsigned sample_degree_distribution(uint32_t fractional_index, const uint16_t *distribution)
{
	return sample_degree_distribution_fixed(fractional_index, distribution) >> (NumFractionalBits + NumFractionalBits);
}

// table must have DistributionTableEntries count.
void build_lookup_table(uint16_t *table, const double *accum_density, unsigned count);
}