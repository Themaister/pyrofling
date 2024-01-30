#include "lt_lut.hpp"
#include <assert.h>
#include <cmath>
#include <vector>
#include <memory>
#include <algorithm>
#include <stdio.h>

namespace LT
{

// Empiric observation.
static constexpr unsigned MaxK_over_S = 16;

struct LTDist
{
	LTDist();
	void build_entry(unsigned num_blocks);
	std::unique_ptr<uint16_t[]> table;

	uint16_t *get(unsigned num_blocks);
};
static LTDist dist;

uint16_t *LTDist::get(unsigned num_blocks)
{
	assert(num_blocks <= MaxNumBlocks);
	assert(num_blocks != 0);
	return table.get() + (num_blocks - 1) * DistributionTableEntries;
}

LTDist::LTDist()
{
	table.reset(new uint16_t[MaxNumBlocks * DistributionTableEntries]);
	for (unsigned i = 1; i <= MaxNumBlocks; i++)
		build_entry(i);
}

void build_lookup_table(uint16_t *table, const double *accum_density, unsigned count)
{
	table[0] = 1u << NumFractionalBits;
	table[DistributionTableEntries - 1] = count << NumFractionalBits;

	for (unsigned i = 1; i < DistributionTableEntries; i++)
	{
		double target_integrated_density = double(i) / double(DistributionTableEntries - 1);
		auto itr = std::upper_bound(accum_density, accum_density + count, target_integrated_density);
		double upper = *itr;
		double lower = 0.0;

		if (itr == accum_density)
		{
			if (count == 1)
			{
				table[i] = 1u << NumFractionalBits;
				continue;
			}
		}
		else
		{
			lower = *(itr - 1);
		}

		double frac = (target_integrated_density - lower) / (upper - lower);
		table[i] = ((uint32_t(itr - accum_density) + 1) << NumFractionalBits) +
		           uint32_t(std::round(frac * double(1u << NumFractionalBits)));
	}
}

// See David J. C. MacKay - Information Theory, Inference and Learning Algorithms - Chapter 50 (2004) for details.

void LTDist::build_entry(unsigned num_blocks)
{
	constexpr double c = 0.3;
	constexpr double delta = 0.5;
	double rho[MaxK_over_S];
	double tau[MaxK_over_S];
	double p[MaxK_over_S];
	const double K = num_blocks;

	// S = expected number of degree-one checks. It should be low, but not too low.
	const double S = c * std::log(K / delta) * std::sqrt(K);

	// Book does not mention how to round S to integer.
	unsigned int_S = unsigned(std::ceil(S));
	unsigned K_over_S = (num_blocks + int_S - 1) / int_S;

	assert(K_over_S <= MaxK_over_S);

	double S_over_K = S / K;

	// Soliton distribution
	rho[0] = 1.0 / K;
	for (unsigned i = 2; i <= K_over_S; i++)
		rho[i - 1] = 1.0 / (double(i) * double(i - 1));

	// After K/S condition for tau is met, the probability is vanishingly small, so truncate the rho table.

	// Robustness adjustment
	for (unsigned i = 1; i < K_over_S; i++)
		tau[i - 1] = S_over_K / double(i);
	tau[K_over_S - 1] = S_over_K * std::log(S / delta);

	double Z = 0.0;
	for (unsigned i = 0; i < K_over_S; i++)
		Z += rho[i] + tau[i];

	double inv_Z = 1.0 / Z;

	for (unsigned i = 0; i < K_over_S; i++)
		p[i] = (rho[i] + tau[i]) * inv_Z;

	fprintf(stderr, "K/S = %u, Z = %f, K = %u, S = %f, mu(1) * K = %.6f, mu(%u) = %.6f\n",
			K_over_S, Z, num_blocks, S, p[0] * K,
	        K_over_S, p[K_over_S - 1]);

	for (unsigned i = 1; i < K_over_S; i++)
		p[i] += p[i - 1];

	build_lookup_table(get(num_blocks), p, K_over_S);
}

const uint16_t *get_degree_distribution(unsigned num_blocks)
{
	return dist.get(num_blocks);
}
}