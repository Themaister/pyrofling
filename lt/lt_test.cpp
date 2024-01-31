#include "lt_lut.hpp"
#include <cmath>
#include <initializer_list>
#include <random>

using namespace LT;

static uint32_t p_to_fixed(double p)
{
	constexpr auto FixedMultiplier = double(1u << (NumFractionalBits + NumDistributionTableBits));
	return uint32_t(std::round(p * FixedMultiplier));
}

static bool validate_accum(uint16_t *lut, std::initializer_list<double> accum)
{
	build_lookup_table(lut, &*accum.begin(), accum.size());

	unsigned expected_for_p0 = 0;
	unsigned i = 0;
	for (auto v : accum)
	{
		i++;
		if (v == 0.0)
			continue;

		// First non-empty bin.
		if (!expected_for_p0)
			expected_for_p0 = i << (NumFractionalBits + NumFractionalBits);

		uint32_t fixed_p = sample_degree_distribution_fixed(p_to_fixed(v), lut);
		uint32_t expected_p = (i + 1) << (NumFractionalBits + NumFractionalBits);
		int err = std::abs(int(fixed_p) - int(expected_p));
		if (err > int(1 << (NumFractionalBits + NumFractionalBits - 6)))
			return false;
	}

	uint32_t fixed_p = sample_degree_distribution_fixed(p_to_fixed(0), lut);
	if (fixed_p != expected_for_p0)
		return false;

	return true;
}

int main()
{
	uint16_t lut[NumDistributionTableEntries];
	if (!validate_accum(lut, { 0.0, 0.1, 1.0 }))
		return 1;
	if (!validate_accum(lut, { 0.1, 0.9, 1.0 }))
		return 1;
	if (!validate_accum(lut, { 0.1, 0.7, 1.0 }))
		return 1;
	if (!validate_accum(lut, { 0.1, 0.7, 0.8, 0.85, 0.93, 1.0 }))
		return 1;

	unsigned counts[16] = {};
	std::default_random_engine rnd{42};
	for (unsigned i = 0; i < 1000000; i++)
	{
		uint32_t v = rnd();
		v &= (1u << (NumFractionalBits + NumDistributionTableBits)) - 1u;
		counts[sample_degree_distribution(v, lut) - 1]++;
	}

	for (unsigned i = 0; i < 6; i++)
		printf("Ratio: %.3f %%\n", 100.0 * double(counts[i]) / 1e6);

	return 0;
}