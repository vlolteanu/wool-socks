#ifndef TLS_HH
#define TLS_HH

#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include <socks6util/socks6util.hh>
#include <ssl.h>
#include <prio.h>
#include <private/pprio.h>
#include "tlscontext.hh"
#include "streambuffer.hh"

class Proxifier;

class TLS: public boost::intrusive_ref_counter<TLS>
{
	int rfd;
	int wfd;

	bool connectCalled;
	bool handshakeFinished;
	
	struct Descriptor
	{
		PRFileDesc *fileDescriptor;
		
		Descriptor(int fd);
		
		~Descriptor();
		
		operator PRFileDesc *()
		{
			return fileDescriptor;
		}
	};
	
	Descriptor descriptor;

	static void handshakeCallback(PRFileDesc *fd, void *clientData);

	static SECStatus canFalseStartCallback(PRFileDesc *fd, void *arg, PRBool *canFalseStart);

public:
	TLS(TLSContext *ctx, int fd);
	
	~TLS();
	
	void setReadFD(int fd);
	
	void setWriteFD(int fd);
	
	void tlsConnect(S6U::SocketAddress *addr, StreamBuffer *buf, bool useEarlyData);
	
	void tlsAccept(StreamBuffer *buf);
	
	size_t tlsWrite(StreamBuffer *buf);
	
	size_t tlsRead(StreamBuffer *buf);
};

#endif // TLS_HH
