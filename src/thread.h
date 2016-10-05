#ifndef _SERVGEN_THREAD_H
#define _SERVGEN_THREAD_H

#include <iostream>
#include <pthread.h>
#include <vector>
#include <stdio.h>
#include <unistd.h>
#include <genf/list.h>
#include <stdint.h>

#include "list.h"
#include <vector.h>

#define IT_BLOCK_SZ 4098

struct ItWriter;
struct ItQueue;
struct Thread;

struct ItHeader
{
	unsigned short msgId;
	unsigned char writerId;
	unsigned int length;
	ItHeader *next;
};

struct ItBlock
{
	ItBlock( int size );

	char data[IT_BLOCK_SZ];

	unsigned int size;
	ItBlock *prev, *next;
};

struct ItWriter
{
	ItWriter();

	Thread *writer;
	Thread *reader;
	ItQueue *queue;
	int id;

	/* Write to the tail block, at tail offset. */
	ItBlock *head;
	ItBlock *tail;

	/* Head and tail offset. */
	int hoff;
	int toff;

	int mlen;

	ItHeader *toSend;

	ItWriter *prev, *next;
};

typedef List<ItWriter> ItWriterList;
typedef std::vector<ItWriter*> ItWriterVect;

struct ItQueue
{
	ItQueue( int blockSz = IT_BLOCK_SZ );

	void *allocBytes( ItWriter *writer, int size );
	void send( ItWriter *writer );

	ItHeader *wait();
	bool poll();
	void release( ItHeader *header );

	pthread_mutex_t mutex;
	pthread_cond_t cond;

	ItHeader *head, *tail;
	int blockSz;

	ItBlock *allocateBlock();
	void freeBlock( ItBlock *block );

	ItWriter *registerWriter( Thread *writer, Thread *reader );

	/* Free list for blocks. */
	ItBlock *free;

	/* The list of writers in the order of registration. */
	ItWriterList writerList;

	/* A vector for finding writers. This lets us identify the writer in the
	 * message header with only a byte. */
	ItWriterVect writerVect;
};

struct SelectFd
{
	enum Type { Listen = 1, Data };

	SelectFd( Type type, int fd )
		: type(type), fd(fd) {}

	Type type;
	int fd;

	SelectFd *prev, *next;
};

typedef List<SelectFd> SelectFdList;

struct PacketHeader;
struct PacketWriter
{
	PacketWriter( int fd )
		: fd(fd) {}

	int fd;
	int mlen;
	PacketHeader *toSend;
	Vector<char> buf;

	char *data() { return buf.data; }
	int length() { return buf.length(); }

	int allocBytes( int b )
	{
		int off = buf.length();
		buf.appendNew( b );
		return off;
	}

	void reset()
	{
		buf.empty();
	}
};

struct PacketHeader
{
	int msgId;
	int writerId;
	int length;
};

struct Thread
{
	Thread( const char *type )
	:
		type( type ),
		breakLoop( false ),
		recvRequiresSignal( false ),
		logFile( &std::cerr )
	{
	}

	const char *type;
	struct endp {};
	typedef List<Thread> ThreadList;

	pthread_t pthread;
	bool breakLoop;

	/* Set this true in a thread's constructor if the main loop is not driven
	 * by listening for genf messages. Signals will be sent automatically on
	 * message send. */
	bool recvRequiresSignal;

	std::ostream *logFile;
	ItQueue control;

	Thread *prev, *next;

	ThreadList childList;

	virtual int start() = 0;

	const Thread &log_prefix() { return *this; }

	virtual	int poll() = 0;
	int inetListen( uint16_t port );
	int selectLoop() { return pselectLoop( 0 ); }
	int pselectLoop( sigset_t *sigmask );
	int inetConnect( uint16_t port );
	virtual void accept( int fd ) {}
	virtual void data( SelectFd *fd ) {}

	SelectFdList selectFdList;
};

void *thread_start_routine( void *arg );

struct log_prefix { };
struct log_time { };

struct fdoutbuf
:
	public std::streambuf
{
	fdoutbuf( int fd )
	:
		fd(fd)
	{
	}

	int_type overflow( int_type c )
	{
		if ( c != EOF ) {
			char z = c;
			if ( write( fd, &z, 1 ) != 1 )
				return EOF;
		}
		return c;
	}

	std::streamsize xsputn( const char* s, std::streamsize num )
	{
		return write(fd,s,num);
	}

	int fd;
};

struct lfdostream
:
	public std::ostream
{
	lfdostream( int fd )
	:
		std::ostream( 0 ),
		buf( fd )
	{
		pthread_mutex_init( &mutex, 0 );
		rdbuf( &buf );
	}

	pthread_mutex_t mutex;
	fdoutbuf buf;
};

struct log_lock {};
struct log_unlock {};

std::ostream &operator <<( std::ostream &out, const Thread::endp & );
std::ostream &operator <<( std::ostream &out, const log_prefix & );
std::ostream &operator <<( std::ostream &out, const log_lock & );
std::ostream &operator <<( std::ostream &out, const log_unlock & );
std::ostream &operator <<( std::ostream &out, const log_time & );
std::ostream &operator <<( std::ostream &out, const Thread &thread );

namespace genf
{
	extern lfdostream *lf;
}

/* The log_prefix() expression can reference a struct or a function that
 * returns something used to write a different prefix. The macros don't care.
 * This allows for context-dependent log messages. */

#define log_FATAL( msg ) \
	*genf::lf << log_lock() << "FATAL: " << log_prefix() << \
	msg << std::endl << log_unlock() << endp()

#define log_ERROR( msg ) \
	*genf::lf << log_lock() << "ERROR: " << log_prefix() << \
	msg << std::endl << log_unlock()
	
#define log_message( msg ) \
	*genf::lf << log_lock() << "message: " << log_prefix() << \
	msg << std::endl << log_unlock()

#define log_warning( msg ) \
	*genf::lf << log_lock() << "warning: " << log_prefix() << \
	msg << std::endl << log_unlock()

extern long enabledRealms;

#define log_debug( realm, msg ) \
	if ( enabledRealms & realm ) \
		*genf::lf << log_lock() << "debug: " << log_prefix() << \
		msg << std::endl << log_unlock()

#endif
