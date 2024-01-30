#pragma once

#include <stdint.h>
#include <stddef.h>
#include <memory>
#include <random>

namespace HybridLT
{
class Shuffler
{
public:
	void seed(uint32_t seed);
	void begin(unsigned total_elements, unsigned selected_elements);
	unsigned pick();
	void flush();

private:
	std::minstd_rand rnd;
	std::unique_ptr<uint32_t []> data;
	unsigned capacity = 0;
	unsigned entries = 0;
};
}