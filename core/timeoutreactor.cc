#include "timeoutreactor.hh"

#include <sys/timerfd.h>
#include <stdexcept>
#include "poller.hh"

using namespace std;
using namespace std::chrono;
using namespace tbb;

TimeoutReactor::TimeoutReactor(Poller *poller, std::vector<int> possibleTimeouts)
	: Reactor(poller)
{
	fd.assign(timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK));
	if (fd < 0)
		throw system_error(errno, system_category());

	for (int i: possibleTimeouts)
		timers[i];
}

TimeoutReactor::~TimeoutReactor()
{
	try
	{
		poller->remove(fd);
	}
	catch(...) {}
}

void TimeoutReactor::start()
{
	static constexpr itimerspec ITSPEC = {
		.it_interval = INTERVAL,
		.it_value    = INTERVAL,
	};

	int rc = timerfd_settime(fd, 0, &ITSPEC, nullptr);
	if (rc < 0)
		throw system_error(errno, system_category());

	poller->add(this, fd, Poller::IN_EVENTS);
}

void TimeoutReactor::process(int fd, uint32_t events)
{
	(void)events;

	uint64_t res;
	int rc = read(fd, &res, sizeof(res));
	if (rc < 0)
		throw system_error(errno, system_category());

	auto now = system_clock().now();

	for (auto &[timeout, entry]: timers)
	{
		auto &[queue, lock] = entry;
		spin_mutex::scoped_lock scopedLock(lock);

		while (!queue.empty())
		{
			Timer *timer = &(queue.front());
			if (duration_cast<milliseconds>(now - timer->arm).count() < timeout)
				break;

			timer->trigger();
			queue.pop_front();
		}
	}
	
	poller->add(this, fd, Poller::IN_EVENTS);
}

void TimeoutReactor::deactivate()
{
	Reactor::deactivate();
	poller->remove(fd);
}

