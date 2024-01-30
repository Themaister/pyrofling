#include "lt_lut.hpp"

using namespace LT;

int main()
{
	uint16_t lut[DistributionTableEntries];
	double accum[4] = { 0.25, 0.5, 0.75, 1.0 };
	build_lookup_table(lut, accum, 4);
}