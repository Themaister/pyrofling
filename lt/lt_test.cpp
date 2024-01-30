#include "lt_shuffle.hpp"
#include "lt_encode.hpp"
#include "lt_decode.hpp"
#include <cmath>
#include <initializer_list>
#include <random>
#include <array>
#include <string.h>

using namespace HybridLT;

#if 0
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
#endif

static int test_encoder()
{
	Encoder encoder;
	Decoder decoder;
	encoder.set_block_size(sizeof(uint32_t));
	decoder.set_block_size(sizeof(uint32_t));

	std::default_random_engine rnd{2000};
	std::uniform_real_distribution<float> dist(0.0f, 1.0f);

	double total_non_dropped = 0.0;
	std::vector<uint32_t> encoded;

	constexpr unsigned num_data_blocks = 5000;
	constexpr unsigned num_iterations = 1000;
	unsigned successful_iterations = 0;
	constexpr unsigned num_lost_packets = 8;
	constexpr unsigned num_xor_blocks = 256;

	for (unsigned num_fec_blocks = 100; num_fec_blocks < 1000; num_fec_blocks++)
	{
		successful_iterations = 0;
		total_non_dropped = 0.0;
		printf("FEC blocks = %u\n", num_fec_blocks);
		for (unsigned iter = 0; iter < num_iterations; iter++)
		{
			std::array<uint32_t, num_data_blocks> buf;
			encoded.clear();

			for (auto &e: buf)
			{
				e = uint32_t(rnd());
				encoded.push_back(e);
			}

			auto seed = rnd();

			encoder.seed(seed);
			encoder.flush();

			for (unsigned i = 0; i < num_fec_blocks; i++)
			{
				uint32_t fec;
				encoder.generate(&fec, buf.data(), buf.size() * sizeof(buf.front()), num_xor_blocks);
				encoded.push_back(fec);
			}

			auto received = encoded;
			decoder.begin_decode(seed, received.data(), buf.size() * sizeof(buf.front()),
			                     num_fec_blocks, num_xor_blocks);

			size_t seq;
			size_t non_dropped = 0;
			for (seq = 0; seq < encoded.size(); seq++)
			{
				if (seq < num_lost_packets)
				{
					received[seq] = 0xdeadca7;
					continue;
				}

				non_dropped++;

				if (seq < num_data_blocks)
				{
					if (decoder.push_raw_block(seq))
						break;
				}
				else
				{
					if (decoder.push_fec_block(seq - num_data_blocks, &received[seq]))
						break;
				}
			}

			if (seq == encoded.size())
				continue;

			if (memcmp(received.data(), buf.data(), buf.size() * sizeof(buf.front())) != 0)
				return 1;

			total_non_dropped += double(non_dropped);
			successful_iterations++;
		}

		printf("  %u packets with %u lost packets -> combined packet fail rate: %.3f %%\n",
		       num_data_blocks, num_lost_packets,
		       100.0 * double(num_iterations - successful_iterations) / double(num_iterations));
	}

	return 0;
}

int main()
{
	//if (test_distribution() != 0)
	//	return 1;

	if (test_encoder() != 0)
		return 1;

	return 0;
}