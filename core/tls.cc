#include <errno.h>
#include <stdexcept>
#include <socks6util/socks6util.hh>
extern "C"
{
#include <private/pprio.h>
}
#include "rescheduleexception.hh"
#include "tlsexception.hh"
#include "poller.hh"
#include "tls.hh"
#include "../external/nspr_stuff.h"

using namespace std;

thread_local TLS::BlockDirection TLS::blockDirection;

void TLS::tlsHandleErr(int fd)
{
	PRErrorCode err = PR_GetError();
	if (err == PR_WOULD_BLOCK_ERROR || err == PR_IN_PROGRESS_ERROR)
	{
		if (blockDirection == BD_IN)
			throw RescheduleException(fd, Poller::IN_EVENTS);
		else /* BD_OUT */
			throw RescheduleException(fd, Poller::OUT_EVENTS);
	}
	if (err == PR_END_OF_FILE_ERROR)
		return;
	throw TLSException(err);
}

SECStatus TLS::canFalseStartCallback(PRFileDesc *fd, void *arg, PRBool *canFalseStart)
{
	(void)fd; (void)arg;

	*canFalseStart = PR_TRUE;
	return SECSuccess;
}

void TLS::descriptorDeleter(PRFileDesc *fd)
{
	delete fd;
}

TLS::TLS(TLSContext *ctx, int fd)
	: readFD(fd), writeFD(fd)
{
	static const PRIOMethods METHODS = {
		.file_type       = PR_DESC_SOCKET_TCP,
		.close           = dClose,
		.read            = dRead,
		.write           = dWrite,
		.available       = (PRAvailableFN)_PR_InvalidInt, //SocketAvailable,
		.available64     = (PRAvailable64FN)_PR_InvalidInt64, //SocketAvailable64,
		.fsync           = (PRFsyncFN)_PR_InvalidInt, //SocketSync,
		.seek            = (PRSeekFN)_PR_InvalidInt,
		.seek64          = (PRSeek64FN)_PR_InvalidInt64,
		.fileInfo        = (PRFileInfoFN)_PR_InvalidStatus,
		.fileInfo64      = (PRFileInfo64FN)_PR_InvalidStatus,
		.writev          = (PRWritevFN)_PR_InvalidInt, //SocketWritev,
		.connect         = (PRConnectFN)_PR_InvalidInt, //SocketConnect,
		.accept          = (PRAcceptFN)_PR_InvalidDesc, //SocketAccept,
		.bind            = (PRBindFN)_PR_InvalidInt, //SocketBind,
		.listen          = (PRListenFN)_PR_InvalidInt, //SocketListen,
		.shutdown        = (PRShutdownFN)_PR_InvalidInt, //SocketShutdown,
		.recv            = dRecv,
		.send            = dSend,
		.recvfrom        = (PRRecvfromFN)_PR_InvalidInt,
		.sendto          = (PRSendtoFN)_PR_InvalidInt,
		.poll            = (PRPollFN)_PR_InvalidInt16, //SocketPoll,
		.acceptread      = (PRAcceptreadFN)_PR_InvalidInt, //SocketAcceptRead,
		.transmitfile    = (PRTransmitfileFN)_PR_InvalidInt, //SocketTransmitFile,
		.getsockname     = dGetName,
		.getpeername     = dGetPeerName,
		.reserved_fn_6   = (PRReservedFN)_PR_InvalidInt,
		.reserved_fn_5   = (PRReservedFN)_PR_InvalidInt,
		.getsocketoption = (PRGetsocketoptionFN)_PR_InvalidInt, //_PR_SocketGetSocketOption,
		.setsocketoption = (PRSetsocketoptionFN)_PR_InvalidInt, //_PR_SocketSetSocketOption,
		.sendfile        = (PRSendfileFN)_PR_InvalidInt, //SocketSendFile,
		.connectcontinue = dConnectContinue,
		.reserved_fn_3   = (PRReservedFN)_PR_InvalidInt,
		.reserved_fn_2   = (PRReservedFN)_PR_InvalidInt,
		.reserved_fn_1   = (PRReservedFN)_PR_InvalidInt,
		.reserved_fn_0   = (PRReservedFN)_PR_InvalidInt
	};

	PRFileDesc *lowerDesc = new PRFileDesc();
	lowerDesc->identity = PR_NSPR_IO_LAYER;
	lowerDesc->methods = &METHODS;
	lowerDesc->secret = reinterpret_cast<PRFilePrivate *>(this);
	lowerDesc->dtor = descriptorDeleter;

	PRFileDesc *higherDesc = SSL_ImportFD(nullptr, lowerDesc);
	if (!higherDesc)
	{
		PR_Close(lowerDesc); //might return error
		throw TLSException();
	}

	descriptor.reset(higherDesc);
	
	/* set key + cert */
	if (ctx->isServer())
	{
		SECStatus rc = SSL_ConfigServerCert(descriptor.get(), ctx->getCert(), ctx->getKey(), nullptr, 0);
		if (rc != SECSuccess)
			throw TLSException();
	}

	//static const int CERT_VERIFY_DEPTH = 3; //TODO: do something with this?
	SECStatus rc = SSL_ResetHandshake(descriptor.get(), ctx->isServer());
	if (rc != SECSuccess)
		throw TLSException();

	rc = SSL_SetCanFalseStartCallback(descriptor.get(), canFalseStartCallback, nullptr);
	if (rc != SECSuccess)
		throw TLSException();
}

TLS::~TLS()
{
}

void TLS::setReadFD(int fd)
{
	this->readFD = fd;
	//TODO: something else?
}

void TLS::setWriteFD(int fd)
{
	this->writeFD = fd;
	//TODO: something else?
}

void TLS::tlsConnect(S6U::SocketAddress *addr, StreamBuffer *buf, bool useEarlyData)
{
	if (!useEarlyData)
		SSL_OptionSet(descriptor.get(), SSL_ENABLE_0RTT_DATA, PR_FALSE);

	this->addr = *addr;
	attemptSendTo = true;

	tlsWrite(buf);
}

size_t TLS::tlsWrite(StreamBuffer *buf)
{
	PRInt32 bytes = PR_Write(descriptor.get(), buf->getHead(), buf->usedSize());
	if (bytes < 0)
	{
		tlsHandleErr(writeFD);
		bytes = 0;
	}
	
	buf->unuse(bytes);
	return bytes;
}

size_t TLS::tlsRead(StreamBuffer *buf)
{
	PRInt32 bytes = PR_Read(descriptor.get(), buf->getTail(), buf->availSize());
	if (bytes < 0)
	{
		tlsHandleErr(readFD);
		bytes = 0;
	}
	
	buf->use(bytes);
	return bytes;
}

PRStatus PR_CALLBACK TLS::dClose(PRFileDesc *fd)
{
	fd->secret = nullptr;
	fd->dtor(fd);

	return PR_SUCCESS;
}

PRInt32 PR_CALLBACK TLS::dRecv(PRFileDesc *fd, void *buf, PRInt32 amount, PRIntn flags, PRIntervalTime timeout)
{
	(void)timeout;

	TLS *tls = reinterpret_cast<TLS *>(fd->secret);

	int rc = recv(tls->readFD, buf, amount, flags);
	if (rc < 0)
		_MD_unix_map_recv_error(errno);
	
	TLS::blockDirection = BD_IN;

	return rc;
}

PRInt32 PR_CALLBACK TLS::dRead(PRFileDesc *fd, void *buf, PRInt32 amount)
{
	return dRecv(fd, buf, amount, MSG_NOSIGNAL, PR_INTERVAL_NO_TIMEOUT);
}

PRInt32 PR_CALLBACK TLS::dSend(PRFileDesc *fd, const void *buf, PRInt32 amount, PRIntn flags, PRIntervalTime timeout)
{
	(void)timeout;

	TLS *tls = reinterpret_cast<TLS *>(fd->secret);

	int rc;

	if (tls->attemptSendTo)
	{
		tls->attemptSendTo = false;
		rc = sendto(tls->writeFD, buf, amount, flags | MSG_FASTOPEN, &tls->addr.sockAddress, tls->addr.size());

	}
	else
	{
		rc = send(tls->writeFD, buf, amount, flags);
	}

	if (rc < 0)
		_MD_unix_map_send_error(errno);
	
	TLS::blockDirection = BD_OUT;

	return rc;
}

PRInt32 PR_CALLBACK TLS::dWrite(PRFileDesc *fd, const void *buf, PRInt32 amount)
{
	return dSend(fd, buf, amount, MSG_NOSIGNAL, PR_INTERVAL_NO_TIMEOUT);
}

PRStatus PR_CALLBACK TLS::dConnectContinue(PRFileDesc *fd, PRInt16 outFlags)
{
	(void)outFlags;

	TLS *tls = reinterpret_cast<TLS *>(fd->secret);

	int err;
	socklen_t optlen = sizeof(err);
	int rc = getsockopt(tls->readFD, SOL_SOCKET, SO_ERROR, &err, &optlen);
	if (rc < 0)
	{
		_MD_unix_map_connect_error(errno);
		return PR_FAILURE;
	}
	else if (err != 0)
	{
		_MD_unix_map_connect_error(err);
		return PR_FAILURE;
	}
	
	TLS::blockDirection = BD_IN;
	
	return PR_SUCCESS;
}

PRStatus PR_CALLBACK TLS::dGetName(PRFileDesc *fd, PRNetAddr *addr)
{
	TLS *tls = reinterpret_cast<TLS *>(fd->secret);

	socklen_t addrLen = sizeof(PRNetAddr);
	int rc = getsockname(tls->readFD, (struct sockaddr *) addr, &addrLen);
	if (rc < 0)
	{
		_MD_unix_map_getsockname_error(errno);
		return PR_FAILURE;
	}

	if (addr->raw.family == AF_INET6)
		addr->raw.family = PR_AF_INET6;

	return PR_SUCCESS;
}

PRStatus PR_CALLBACK TLS::dGetPeerName(PRFileDesc *fd, PRNetAddr *addr)
{
	TLS *tls = reinterpret_cast<TLS *>(fd->secret);

	socklen_t addrLen = sizeof(PRNetAddr);
	int rc = getpeername(tls->readFD, (struct sockaddr *) addr, &addrLen);
	if (rc < 0)
	{
		if (errno == ENOTCONN && tls->addr.isValid())
		{
			memcpy(addr, &tls->addr.storage, tls->addr.size());
		}
		else
		{
			_MD_unix_map_getpeername_error(errno);
			return PR_FAILURE;
		}
	}

	if (addr->raw.family == AF_INET6)
		addr->raw.family = PR_AF_INET6;

	return PR_SUCCESS;
}
