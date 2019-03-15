#include <socks6util/socks6util.hh>
#include <socks6msg/socks6msg.hh>
#include <system_error>
#include <fcntl.h>
#include "../core/poller.hh"
#include "proxifier.hh"
#include "proxifierdownstreamer.hh"
#include "proxifierupstreamer.hh"

using namespace std;

static const size_t HEADROOM = 512; //more than enough for any request

ProxifierUpstreamer::ProxifierUpstreamer(Proxifier *proxifier, int *pSrcFD, TLSContext *clientCtx, std::shared_ptr<WindowSupplicant> windowSupplicant)
	: StreamReactor(proxifier->getPoller(), SS_SENDING), proxifier(proxifier), state(S_CONNECTING), wallet(proxifier->getWallet()), windowSupplicant(windowSupplicant)
{
	buf.makeHeadroom(HEADROOM);

	srcSock.fd.assign(*pSrcFD);
	*pSrcFD = -1;
	
	dstSock.fd.assign(socket(proxifier->getProxyAddr()->storage.ss_family, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP));
	if (dstSock.fd < 0)
		throw system_error(errno, system_category());
	if (clientCtx != nullptr)
		dstSock.tls = new TLS(clientCtx, dstSock.fd);
	
	int rc = S6U::Socket::getOriginalDestination(srcSock.fd, &dest.storage);
	if (rc < 0)
		throw system_error(errno, system_category());
}

void ProxifierUpstreamer::start()
{
	try
	{
		srcSock.serverHandshake(&buf);
	}
	catch (exception &)
	{
		throw runtime_error("Unexpected exception in null handshake");
	}
	
	/* read initial data opportunistically */
	try
	{
		srcSock.sockRecv(&buf);
	}
	catch (RescheduleException &) {}

	S6M::Request req(SOCKS6_REQUEST_CONNECT, dest.getAddress(), dest.getPort());

	ssize_t tfoPayload = S6U::Socket::tfoPayloadSize(srcSock.fd);
	if (tfoPayload > 0)
		req.getOptionSet()->setTFOPayload(tfoPayload);

	if (proxifier->getUsername()->length() > 0)
		req.getOptionSet()->setUsernamePassword(proxifier->getUsername(), proxifier->getPassword());

	if (windowSupplicant.get() != nullptr)
		windowSupplicant->process(&req);

	S6U::RequestSafety::Recommendation recommendation = S6U::RequestSafety::recommend(req, dstSock.tls != nullptr, buf.usedSize());
	uint32_t token;
	if (recommendation.useToken && wallet->extract(&token))
	{
		req.getOptionSet()->setToken(token);
		recommendation.tokenSpent(dstSock.tls != nullptr);
	}

	uint8_t reqBuf[HEADROOM];
	S6M::ByteBuffer bb(reqBuf, sizeof(reqBuf));
	req.pack(&bb);
	buf.prepend(bb.getBuf(), bb.getUsed());

	/* connect */
	dstSock.sockConnect(*proxifier->getProxyAddr(), &buf, recommendation.tfoPayload, recommendation.earlyData);

	poller->add(this, dstSock.fd, Poller::OUT_EVENTS);
}

void ProxifierUpstreamer::process(int fd, uint32_t events)
{
	switch (state)
	{
	case S_CONNECTING:
	{
		state = S_HANDSHAKING;
		[[fallthrough]];
	}
	case S_HANDSHAKING:
	{
		dstSock.clientHandshake();

		poller->assign(new ProxifierDownstreamer(this));

		state = S_STREAM;
		streamState = buf.usedSize() > 0 ? SS_SENDING : SS_RECEIVING;
		
		if (buf.usedSize() > 0)
			streamState = SS_SENDING;
		else
			streamState = SS_RECEIVING;
		[[fallthrough]];
	}
		
	case S_STREAM:
		StreamReactor::process(fd, events);
		break;
	}
}
