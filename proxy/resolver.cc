#include "resolver.hh"

void Resolver::start() {}

void Resolver::process(int fd, uint32_t events)
{
	//TODO
}

void Resolver::resolve(boost::intrusive_ptr<ProxyUpstreamer> upstreamer, const std::string &hostname)
{
	//TODO
	upstreamer->resolvDone({});
}
