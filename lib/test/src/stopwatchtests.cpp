#include "thirdparty/doctest.h"
#include "lib/stopwatch.hpp"

#include <thread>

TEST_CASE("stopwatch")
{
	SUBCASE("instant")
	{
		lib::stopwatch stopwatch;
		stopwatch.start();
		stopwatch.stop();

		CHECK_EQ(stopwatch.elapsed<lib::stopwatch::ms, int>(), 0);
	}

	SUBCASE("delay")
	{
		constexpr unsigned int delay = 5;
		lib::stopwatch stopwatch;

		stopwatch.start();
		std::this_thread::sleep_for(lib::stopwatch::ms(delay));
		stopwatch.stop();

		CHECK_EQ(stopwatch.elapsed<lib::stopwatch::ms, int>(), delay);
	}
}
