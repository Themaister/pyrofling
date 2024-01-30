#include "lt_shuffle.hpp"
#include <assert.h>
#include <string.h>

namespace HybridLT
{
void Shuffler::seed(uint32_t seed)
{
	rnd.seed(seed);
}

void Shuffler::flush()
{
	entries = 0;
}

void Shuffler::begin(unsigned total_elements, unsigned selected_elements)
{
	assert(total_elements >= selected_elements);

	if (capacity < total_elements)
	{
		if (capacity == 0)
			capacity = 1;
		while (capacity < total_elements)
			capacity *= 2;

		std::unique_ptr<uint32_t []> newptr(new uint32_t[capacity]);
		if (data)
			memcpy(newptr.get(), data.get(), entries * sizeof(uint32_t));
		data = std::move(newptr);
	}

	// Ensure that we pick unique entries over N iterations.
	// This ensures we get FEC coverage of all inputs.
	if (entries < selected_elements)
	{
		for (unsigned i = 0; i < total_elements; i++)
			data[i] = i;
		entries = total_elements;
	}
}

unsigned Shuffler::pick()
{
	assert(entries);
	unsigned index = uint32_t(rnd()) % entries;
	unsigned ret = data[index];
	data[index] = data[--entries];
	return ret;
}
}