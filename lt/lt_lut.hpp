#pragma once

#include <stdint.h>

namespace LT
{
static constexpr unsigned NumDistributionTableBits = 8;
static constexpr unsigned NumFractionalBits = 8;
static constexpr unsigned DistributionTableEntries = (1u << NumDistributionTableBits) + 1u;
static constexpr unsigned MaxNumBlocks = 1024;

const uint16_t *get_degree_distribution(unsigned num_blocks);

template <typename T>
static unsigned sample_degree_distribution(T &rnd, const uint16_t *distribution)
{
	uint32_t v = rnd();
	unsigned index = (v >> NumFractionalBits) & ((1u << NumDistributionTableBits) - 1u);
	unsigned frac = v & ((1u << NumFractionalBits) - 1u);

	// 8.8 fixed point.
	uint32_t lo = distribution[index];
	uint32_t hi = distribution[index + 1];

	// 8.16 fixed point
	auto l = lo * ((1u << NumFractionalBits) - frac) + hi * frac;

	// Floor
	l >>= (NumFractionalBits + NumFractionalBits);

	return l;
}

// table must have DistributionTableEntries count.
void build_lookup_table(uint16_t *table, const double *accum_density, unsigned count);
}