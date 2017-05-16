#define KERN
#include "attribute.h"
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <asm/cacheflush.h>

#include "krkern.h"

static int kdata_sock_release( struct socket *sock );
static int kdata_sock_create( struct net *net, struct socket *sock, int protocol, int kern );
static int kdata_sock_mmap(struct file *file, struct socket *sock, struct vm_area_struct *vma );
static int kdata_bind(struct socket *sock, struct sockaddr *sa, int addr_len);
static int kdata_getsockopt(struct socket *sock, int level, int optname, char __user *optval, int __user *optlen);
static int kdata_recvmsg( struct kiocb *iocb, struct socket *sock, struct msghdr *msg, size_t len, int flags );
static int kdata_sendmsg( struct kiocb *iocb, struct socket *sock, struct msghdr *msg, size_t len );

static void kdata_init_control( struct kring_ring *ring );

static struct proto_ops kdata_ops = {
	.family = KDATA,
	.owner = THIS_MODULE,

	.release = kdata_sock_release,
	.bind = kdata_bind,
	.mmap = kdata_sock_mmap,
	.poll = kring_poll,
	.setsockopt = kring_setsockopt,
	.getsockopt = kdata_getsockopt,
	.ioctl = kring_ioctl,
	.recvmsg = kdata_recvmsg,
	.sendmsg = kdata_sendmsg,

	/* Not used. */
	.connect = sock_no_connect,
	.socketpair = sock_no_socketpair,
	.accept = sock_no_accept,
	.getname = sock_no_getname,
	.listen = sock_no_listen,
	.shutdown = sock_no_shutdown,
	.sendpage = sock_no_sendpage,
};

struct kring_params kdata_params =
{
	KDATA_NPAGES,

	KDATA_CTRL_SZ,

	KDATA_CTRL_OFF_HEAD,
	KDATA_CTRL_OFF_WRITER,
	KDATA_CTRL_OFF_READER,
	KDATA_CTRL_OFF_DESC,

	KDATA_DATA_SZ,

	KDATA_READERS,

	&kdata_init_control,
};

static inline struct kring_sock *kring_sk( const struct sock *sk )
{
	return container_of( sk, struct kring_sock, sk );
}

static struct proto kdata_proto = {
	.name = "KDATA",
	.owner = THIS_MODULE,
	.obj_size = sizeof(struct kring_sock),
};

static struct net_proto_family kdata_family_ops = {
	.family = KDATA,
	.create = kdata_sock_create,
	.owner = THIS_MODULE,
};


static int kdata_sock_release( struct socket *sock )
{
	struct sock *sk = sock->sk;

	if ( !sk )
		return 0;

	printk( "kdata_sock_release\n" );

	sock->sk = NULL;
	sock_put( sk );
	return 0;
}

static void decon_pgoff( unsigned long pgoff, unsigned long *rid, unsigned long *region )
{
	*rid = ( pgoff & KDATA_PGOFF_ID_MASK ) >> KDATA_PGOFF_ID_SHIFT;
	*region = ( pgoff & KDATA_PGOFF_REGION_MASK ) >> KDATA_PGOFF_REGION_SHIFT;
}

static int kdata_sock_mmap( struct file *file, struct socket *sock, struct vm_area_struct *vma )
{
	struct kring_sock *krs = kring_sk( sock->sk );
	struct kring_ringset *r = krs->ringset;
	unsigned long ring_id, region;

	/* Ensure bound. */
	if ( r == 0 ) {
		printk( "kdata mmap: socket not bound to ring\n" );
		return -EINVAL;
	}
	
	decon_pgoff( vma->vm_pgoff, &ring_id, &region );

	if ( ring_id > r->nrings ) {
		printk( "kdata mmap: error: rid > r->nrings\n" );
		return -EINVAL;
	}

	switch ( region  ) {
		case KDATA_PGOFF_CTRL: {
			printk( "mapping control region %p of ring %p-%lu\n", r->ring[ring_id].ctrl, r, ring_id );
			remap_vmalloc_range( vma, r->ring[ring_id].ctrl, 0 );
			break;
		}

		case KDATA_PGOFF_DATA: {
			int i;
			unsigned long uaddr = vma->vm_start;
			printk( "mapping data region %lu of ring %p-%lu\n", uaddr, r, ring_id );
			for ( i = 0; i < KDATA_NPAGES; i++ ) {
				vm_insert_page( vma, uaddr, r->ring[ring_id].pd[i].p );
				uaddr += PAGE_SIZE;
			}

			break;
		}
	}

	return 0;
}

static int validate_ring_name( const char *name )
{
	const char *p = name;
	const char *pe = name + KRING_NLEN;
	while ( 1 ) {
		/* Reached the end without validation. */
		if ( p == pe )
			return -1;

		/* Got to a valid string end. Finish. */
		if ( *p == 0 )
			return 0;

		if ( ! ( ( 'A' <= *p && *p <= 'Z' ) ||
				( 'a' <= *p && *p <= 'z' ) ||
				( '0' <= *p && *p <= '9' ) ||
				*p == '_' || *p == '-' || 
				*p == '.' ) )
		{
			/* Invalid char */
			return -1;
		}

		/* Okay, next char. */
		p += 1;
	}
}

static int kdata_allocate_reader_on_ring( struct kring_ring *ring )
{
	int reader_id;
	bool orig;

again:

	/* Search for a reader id that is free on the ring requested. */
	for ( reader_id = 0; reader_id < KDATA_READERS; reader_id++ ) {
		if ( !ring->reader[reader_id].allocated )
			break;
	}

	if ( reader_id == KDATA_READERS ) {
		/* No valid id found */
		return -EINVAL;
	}

	/* All okay. */
	orig = __sync_val_compare_and_swap( &ring->reader[reader_id].allocated, 0, 1 );
	if ( orig != 0 )
		goto again;

	ring->num_readers += 1;

	return reader_id;
}

static int kdata_allocate_reader_all_rings( struct kring_ringset *ringset )
{
	int i, reader_id;

	/* Search for a single reader id that is free across all the rings requested. */
	for ( reader_id = 0; reader_id < KDATA_READERS; reader_id++ ) {
		for ( i = 0; i < ringset->nrings; i++ ) {
			if ( ringset->ring[i].reader[reader_id].allocated )
				goto next_id;
		}

		/* Got through all the rings, break with a valid id. */
		goto good;

		next_id: {}
	}

	/* No valid id found (exited id loop). */
	return -EINVAL;

	/* All okay. */
	good: {}
	
	/* Allocate reader ids, increas reader count. */
	for ( i = 0; i < ringset->nrings; i++ ) {
		ringset->ring[i].reader[reader_id].allocated = true;
		ringset->ring[i].num_readers += 1;
	}
	
	return reader_id;
}

static int kdata_allocate_writer_on_ring( struct kring_ring *ring )
{
	int writer_id;

	/* Search for a writer id that is free on the ring requested. */
	for ( writer_id = 0; writer_id < KDATA_WRITERS; writer_id++ ) {
		if ( !ring->writer[writer_id].allocated )
			break;
	}

	if ( writer_id == KDATA_WRITERS ) {
		/* No valid id found */
		return -EINVAL;
	}

	/* All okay. */
	ring->writer[writer_id].allocated = true;
	ring->num_writers += 1;

	return writer_id;
}

static int kdata_bind( struct socket *sock, struct sockaddr *sa, int addr_len )
{
	int reader_id = -1, writer_id = -1;
	struct kdata_addr *addr = (struct kdata_addr*)sa;
	struct kring_sock *krs;
	struct kring_ringset *ringset;

	if ( addr_len != sizeof(struct kdata_addr) ) {
		printk("kdata_bind: addr_len wrong size\n");
		return -EINVAL;
	}

	if ( validate_ring_name( addr->name ) < 0 ) {
		printk( "kdata_bind: bad ring name\n" );
		return -EINVAL;
	}
	
	if ( addr->mode != KRING_READ && addr->mode != KRING_WRITE ) {
		printk( "kdata_bind: bad mode, not read or write\n" );
		return -EINVAL;
	}
	
	ringset = kdata_find_ring( addr->name );
	if ( ringset == 0 ) {
		printk( "kdata_bind: bad mode, not read or write\n" );
		return -EINVAL;
	}

	if ( addr->ring_id != KDATA_RING_ID_ALL &&
			( addr->ring_id < 0 || addr->ring_id >= ringset->nrings ) )
	{
		printk( "kdata_bind: bad ring id %d\n", addr->ring_id );
		return -EINVAL;
	}

	/* Cannot write to all rings. */
	if ( addr->mode == KRING_WRITE && addr->ring_id == KDATA_RING_ID_ALL ) {
		printk( "kdata_bind: cannot write to ring id ALL\n" );
		return -EINVAL;
	}

	krs = kring_sk( sock->sk );

	if ( addr->mode == KRING_WRITE ) {
		/* Find a writer ID. */
		writer_id = kdata_allocate_writer_on_ring( &ringset->ring[addr->ring_id] );
		if ( writer_id < 0 )
			return writer_id;
	}
	else if ( addr->mode == KRING_READ ) {
		/* Find a reader ID. */
		if ( addr->ring_id != KDATA_RING_ID_ALL ) {
			/* Reader ID for ring specified. */
			reader_id = kdata_allocate_reader_on_ring( &ringset->ring[addr->ring_id] );
		}
		else {
			/* Find a reader ID that works for all rings. This can fail. */
			reader_id = kdata_allocate_reader_all_rings( ringset );
		}

		if ( reader_id < 0 )
			return reader_id;

	}

	krs->ringset = ringset;
	krs->ring_id = addr->ring_id;
	krs->mode = addr->mode;
	krs->writer_id = writer_id;
	krs->reader_id = reader_id;

	return 0;
}

static int kdata_getsockopt( struct socket *sock, int level, int optname, char __user *optval, int __user *optlen )
{
	int len;
	int val, lv = sizeof(val);
	void *data = &val;
	struct kring_sock *krs = kring_sk( sock->sk );

	if ( level != SOL_PACKET )
		return -ENOPROTOOPT;

	if ( get_user(len, optlen) )
		return -EFAULT;

	if ( len != lv )
		return -EINVAL;

	printk( "kdata_getsockopt\n" );

	switch ( optname ) {
		case KR_OPT_RING_N:
			val = krs->ringset->nrings;
			break;
		case KR_OPT_READER_ID:
			val = krs->reader_id;
			break;
		case KR_OPT_WRITER_ID:
			val = krs->writer_id;
			break;
	}

	if ( put_user( len, optlen ) )
		return -EFAULT;

	if ( copy_to_user( optval, data, len) )
		return -EFAULT;
	return 0;
}

static int kdata_kern_avail( struct kring_ringset *r, struct kring_sock *krs )
{
	if ( krs->ring_id != KDATA_RING_ID_ALL )
		return kdata_avail_impl( KDATA_CONTROL(r->ring[krs->ring_id]), krs->reader_id );
	else {
		int ring;
		for ( ring = 0; ring < r->nrings; ring++ ) {
			if ( kdata_avail_impl( KDATA_CONTROL(r->ring[ring]), krs->reader_id ) )
				return 1;
		}
		return 0;
	}
}

/* Waiting readers go to sleep with the recvmsg system call. */
static int kdata_recvmsg( struct kiocb *iocb, struct socket *sock, struct msghdr *msg, size_t len, int flags )
{
	struct kring_sock *krs = kring_sk( sock->sk );
	struct kring_ringset *r = krs->ringset;
	sigset_t blocked, oldset;
	int ret;
	wait_queue_head_t *wq;

	/* Allow kill, stop and the user sigs. This assumes we are operating under
	 * the genf program framework where we want to atomically unmask the user
	 * signals so we can receive genf inter-thread messages. */
	siginitsetinv( &blocked, sigmask(SIGKILL) | sigmask(SIGSTOP) |
			sigmask(SIGUSR1) | sigmask(SIGUSR2) );
	sigprocmask( SIG_SETMASK, &blocked, &oldset );

	// wq = krs->ring_id == KRING_RING_ID_ALL ? &r->reader_waitqueue 
	// : &r->ring[krs->ring_id].reader_waitqueue;

	wq = &r->waitqueue;

	ret = wait_event_interruptible( *wq, kdata_kern_avail( r, krs ) );

	if ( ret == -ERESTARTSYS ) {
		/* Interrupted by a signal. */
		memcpy(&current->saved_sigmask, &oldset, sizeof(oldset));
		set_restore_sigmask();
		return -EINTR;
	}

	sigprocmask(SIG_SETMASK, &oldset, NULL);
	return 0;
}

/* The sendmsg system call is used for waking up waiting readers. */
static int kdata_sendmsg( struct kiocb *iocb, struct socket *sock, struct msghdr *msg, size_t len )
{
	struct kring_sock *krs = kring_sk( sock->sk );
	struct kring_ringset *r = krs->ringset;
	wake_up_interruptible_all( &r->waitqueue );
	return 0;
}

static void kdata_sock_destruct( struct sock *sk )
{
	struct kring_sock *krs = kring_sk( sk );
	int i;

	if ( krs == 0 ) {
		printk("kdata_sock_destruct: don't have krs\n" );
		return;
	}

	printk( "kdata_sock_destruct mode: %d ring_id: %d reader_id: %d\n",
			krs->mode, krs->ring_id, krs->reader_id );

	switch ( krs->mode ) {
		case KRING_WRITE: {
			krs->ringset->ring[krs->ring_id].num_writers -= 1;
			break;
		}
		case KRING_READ: {

			/* One ring or all? */
			if ( krs->ring_id != KDATA_RING_ID_ALL ) {
				struct kdata_control *control = KDATA_CONTROL(krs->ringset->ring[krs->ring_id]);

				krs->ringset->ring[krs->ring_id].reader[krs->reader_id].allocated = false;

				if ( control->reader[krs->reader_id].entered ) {
					kdata_off_t prev = control->reader[krs->reader_id].rhead;
					kdata_reader_release( krs->reader_id, &control[0], prev );
				}
			}
			else {
				for ( i = 0; i < krs->ringset->nrings; i++ ) {
					struct kdata_control *control = KDATA_CONTROL(krs->ringset->ring[i]);

					krs->ringset->ring[i].reader[krs->reader_id].allocated = false;

					if ( control->reader[krs->reader_id].entered ) {
						kdata_off_t prev = control->reader[krs->reader_id].rhead;
						kdata_reader_release( krs->reader_id, &control[0], prev );
					}
				}
			}
		}
	}
}

int kdata_sock_create( struct net *net, struct socket *sock, int protocol, int kern )
{
	struct sock *sk;

//	/* Privs? */
//	if ( !capable( CAP_NET_ADMIN ) )
//		return -EPERM;

	if ( sock->type != SOCK_RAW )
		return -ESOCKTNOSUPPORT;

	if ( protocol != htons(ETH_P_ALL) )
		return -EPROTONOSUPPORT;

	sk = sk_alloc( net, PF_INET, GFP_KERNEL, &kdata_proto );

	if ( sk == NULL )
		return -ENOMEM;

	sock->ops = &kdata_ops;
	sock_init_data( sock, sk );

	sk->sk_family = KDATA;
	sk->sk_destruct = kdata_sock_destruct;

	return 0;
}

int kdata_kopen( struct kdata_kern *kring, const char *rsname, int ring_id, enum KRING_MODE mode )
{
	int reader_id = -1, writer_id = -1;

	struct kring_ringset *ringset = kdata_find_ring( rsname );
	if ( ringset == 0 )
		return -1;
	
	if ( ring_id < 0 || ring_id >= ringset->nrings )
		return -1;

	kring_copy_name( kring->name, rsname );
	kring->ringset = ringset;
	kring->ring_id = ring_id;

	if ( mode == KRING_WRITE ) {
		/* Find a writer ID. */
		writer_id = kdata_allocate_writer_on_ring( &ringset->ring[ring_id] );
		if ( writer_id < 0 )
			return writer_id;
	}
	else if ( mode == KRING_READ ) {
		/* Find a reader ID. */
		if ( ring_id != KDATA_RING_ID_ALL ) {
			/* Reader ID for ring specified. */
			reader_id = kdata_allocate_reader_on_ring( &ringset->ring[ring_id] );
		}
		else {
			/* Find a reader ID that works for all rings. This can fail. */
			reader_id = kdata_allocate_reader_all_rings( ringset );
		}

		if ( reader_id < 0 )
			return reader_id;

		/*res = */kdata_prep_enter( KDATA_CONTROL(ringset->ring[ring_id]), 0 );
		//if ( res < 0 ) {
		//	kdata_func_error( KRING_ERR_ENTER, 0 );
		//	return -1;
		//}
	}

	/* Set up the user read/write struct for unified read/write operations between kernel and user space. */
	kring->user.socket = -1;
	kring->user.control = KDATA_CONTROL(ringset->ring[ring_id]);
	kring->user.data = 0;
	kring->user.pd = ringset->ring[ring_id].pd;
	kring->user.mode = mode;
	kring->user.ring_id = ring_id;
	kring->user.reader_id = reader_id;
	kring->user.writer_id = writer_id;
	kring->user.nrings = ringset->nrings;

	return 0;
}

int kdata_kclose( struct kdata_kern *kring )
{
	kring->ringset->ring[kring->ring_id].num_writers -= 1;
	return 0;
}

static void kdata_write_single( struct kdata_kern *kring, int dir,
		const struct sk_buff *skb, int offset, int write, int len )
{
	struct kdata_packet_header *h;
	void *pdata;

	/* Which ringset? */
	struct kring_ringset *r = kring->ringset;

	h = kdata_write_FIRST( &kring->user );

	h->len = len;
	h->dir = (char) dir;

	pdata = (char*)(h + 1);
	skb_copy_bits( skb, offset, pdata, write );

	kdata_write_SECOND( &kring->user );

	/* track the number of packets produced. Note we don't account for overflow. */
	__sync_add_and_fetch( &KDATA_CONTROL( r->ring[kring->ring_id] )->head->produced, 1 );

	#if 0
	for ( id = 0; id < KRING_READERS; id++ ) {
		if ( r->ring[kring->ring_id].reader[id].allocated ) {
			unsigned long long diff =
					r->ring[kring->ring_id].control.writer->produced -
					r->ring[kring->ring_id].control.reader[id].units;
			// printk( "diff: %llu\n", diff );
			if ( diff > ( KDATA_NPAGES / 2 ) ) {
				printk( "half full: ring_id: %d reader_id: %d\n", kring->ring_id, id );
			}
		}
	}
	#endif

	wake_up_interruptible_all( &r->waitqueue );
}

void kdata_kwrite( struct kdata_kern *kring, int dir, const struct sk_buff *skb )
{
	int offset = 0, write, len;

	while ( true ) {
		/* Number of bytes left in the packet (maybe overflows) */
		len = skb->len - offset;
		if ( len <= 0 )
			break;

		/* What can we write this time? */
		write = len;
		if ( write > kdata_packet_max_data() )
			write = kdata_packet_max_data();

		kdata_write_single( kring, dir, skb, offset, write, len );

		offset += write;
	}
}

int kdata_kavail( struct kdata_kern *kring )
{
	struct kring_ring *ring = &kring->ringset->ring[kring->ring_id];
	struct kdata_control *control = KDATA_CONTROL( *ring );
	int reader_id = 0;

	return ring->num_writers > 0 &&
		( control->reader[reader_id].rhead != control->head->whead );
}

void kdata_knext_plain( struct kdata_kern *kring, struct kdata_plain *plain )
{
	struct kdata_plain_header *h;

	h = (struct kdata_plain_header*) kdata_next_generic( &kring->user );

	plain->len = h->len;
	plain->bytes = (unsigned char*)(h + 1);
}


static void kdata_init_control( struct kring_ring *r )
{
	KDATA_CONTROL(*r)->head->whead = KDATA_CONTROL(*r)->head->wresv = kdata_prev( 0 );
}

int kdata_init(void)
{
	int rc;

	sock_register(&kdata_family_ops);

	if ( (rc = proto_register(&kdata_proto, 0) ) != 0 )
		return rc;

	return 0;
}

void kdata_exit(void)
{
	sock_unregister( KDATA );

	proto_unregister( &kdata_proto );
}

EXPORT_SYMBOL_GPL(kdata_kopen);
EXPORT_SYMBOL_GPL(kdata_kclose);
EXPORT_SYMBOL_GPL(kdata_kwrite);
EXPORT_SYMBOL_GPL(kdata_kavail);
EXPORT_SYMBOL_GPL(kdata_knext_plain);
