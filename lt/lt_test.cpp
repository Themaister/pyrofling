#include "lt_lut.hpp"
#include "lt_encode.hpp"
#include "lt_decode.hpp"
#include <cmath>
#include <initializer_list>
#include <random>
#include <array>

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

static int test_distribution()
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
		v &= DistributionMask;
		counts[sample_degree_distribution(v, lut) - 1]++;
	}

	for (unsigned i = 0; i < 6; i++)
		printf("Ratio: %.3f %%\n", 100.0 * double(counts[i]) / 1e6);

	return 0;
}

static int test_encoder()
{
	Encoder encoder;
	Decoder decoder;
	encoder.set_block_size(sizeof(uint32_t));
	decoder.set_block_size(sizeof(uint32_t));

	const std::array<uint32_t, 15> buf = { 5, 9, 10, 14, 19, 80, 19, 2, 3, 90, 400, 800, 1000, 2000, 4000 };

	std::array<uint32_t, buf.size()> output;
	encoder.begin_encode(1234, buf.data(), sizeof(buf));

	std::array<uint32_t, 32> encoded;
	for (auto &e : encoded)
		encoder.generate_block(&e);
	auto received = encoded;

	decoder.begin_decode(1234, output.data(), output.size() * sizeof(output.front()), encoded.size());

	size_t seq;
	for (seq = 0; seq < 32; seq++)
		if (decoder.push_block(seq, &received[seq]))
			break;

	return 0;
}

int main()
{
	if (test_distribution() < 0)
		return 1;

	if (test_encoder() < 0)
		return 1;

	return 0;
}