/*
 * kidgames -- random numbers on the target. See kgrand.h.
 */

#include "kgrand.h"
#include "../../common/zrng.h"

uint32_t kg_rand(uint32_t n)
{
	if (!n) return 0;
	return z_rng_below(n);
}

void kg_rand_init(void)
{
	/*
	 * z_rng_reseed() pulls from the TRNG where one exists. It is safe
	 * to call on a board without one -- zrng.c falls back to its own
	 * stirred state rather than blocking or failing -- so there is no
	 * probe to do here and no error to report.
	 */
	z_rng_reseed();
}
