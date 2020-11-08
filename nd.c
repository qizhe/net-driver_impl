// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		DATACENTER ADMISSION CONTROL PROTOCOL(ND) 
 *
 * Authors:	Ross Biro
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Arnt Gulbrandsen, <agulbra@nvg.unit.no>
 *		Alan Cox, <alan@lxorguk.ukuu.org.uk>
 *		Hirokazu Takahashi, <taka@valinux.co.jp>
 */

#define pr_fmt(fmt) "ND: " fmt

#include <linux/uaccess.h>
#include <asm/ioctls.h>
#include <linux/memblock.h>
#include <linux/highmem.h>
#include <linux/swap.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/module.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/igmp.h>
#include <linux/inetdevice.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <net/tcp_states.h>
#include <linux/skbuff.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <net/net_namespace.h>
#include <net/icmp.h>
#include <net/inet_hashtables.h>
#include <net/ip_tunnels.h>
#include <net/route.h>
#include <net/checksum.h>
#include <net/xfrm.h>
#include <trace/events/udp.h>
#include <linux/static_key.h>
#include <trace/events/skb.h>
#include <net/busy_poll.h>
#include "nd_impl.h"
#include <net/sock_reuseport.h>
#include <net/addrconf.h>
#include <net/udp_tunnel.h>
#include <net/tcp.h>
// #include "linux_nd.h"
// #include "net_nd.h"
// #include "net_ndlite.h"
#include "uapi_linux_nd.h"
// struct udp_table nd_table __read_mostly;
// EXPORT_SYMBOL(nd_table);
#include "nd_host.h"

struct nd_peertab nd_peers_table;
EXPORT_SYMBOL(nd_peers_table);

long sysctl_nd_mem[3] __read_mostly;
EXPORT_SYMBOL(sysctl_nd_mem);

atomic_long_t nd_memory_allocated;
EXPORT_SYMBOL(nd_memory_allocated);

struct nd_match_tab nd_match_table;
EXPORT_SYMBOL(nd_match_table);

struct nd_params nd_params;
EXPORT_SYMBOL(nd_params);

struct nd_epoch nd_epoch;
EXPORT_SYMBOL(nd_epoch);

struct inet_hashinfo nd_hashinfo;
EXPORT_SYMBOL(nd_hashinfo);
#define MAX_ND_PORTS 65536
#define PORTS_PER_CHAIN (MAX_ND_PORTS / ND_HTABLE_SIZE_MIN)

void nd_rbtree_insert(struct rb_root *root, struct sk_buff *skb)
{
        struct rb_node **p = &root->rb_node;
        struct rb_node *parent = NULL;
        struct sk_buff *skb1;

        while (*p) {
                parent = *p;
                skb1 = rb_to_skb(parent);
                if (before(ND_SKB_CB(skb)->seq, ND_SKB_CB(skb1)->seq))
                        p = &parent->rb_left;
                else
                        p = &parent->rb_right;
        }
        rb_link_node(&skb->rbnode, parent, p);
        rb_insert_color(&skb->rbnode, root);
}

static void nd_rtx_queue_purge(struct sock *sk)
{
	struct rb_node *p = rb_first(&sk->tcp_rtx_queue);

	// nd_sk(sk)->highest_sack = NULL;
	while (p) {
		struct sk_buff *skb = rb_to_skb(p);

		p = rb_next(p);
		/* Since we are deleting whole queue, no need to
		 * list_del(&skb->tcp_tsorted_anchor)
		 */
		nd_rtx_queue_unlink(skb, sk);
		nd_wmem_free_skb(sk, skb);
	}
}

static void nd_ofo_queue_purge(struct sock *sk)
{
	struct nd_sock * dsk = nd_sk(sk);
	struct rb_node *p = rb_first(&dsk->out_of_order_queue);

	// nd_sk(sk)->highest_sack = NULL;
	while (p) {
		struct sk_buff *skb = rb_to_skb(p);

		p = rb_next(p);
		/* Since we are deleting whole queue, no need to
		 * list_del(&skb->tcp_tsorted_anchor)
		 */
		nd_ofo_queue_unlink(skb, sk);
		nd_rmem_free_skb(sk, skb);
	}
}

void nd_write_queue_purge(struct sock *sk)
{
	// struct nd_sock *dsk;
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&sk->sk_write_queue)) != NULL) {
		nd_wmem_free_skb(sk, skb);
	}
	nd_rtx_queue_purge(sk);
	skb = sk->sk_tx_skb_cache;
	if (skb) {
		__kfree_skb(skb);
		sk->sk_tx_skb_cache = NULL;
	}
	// sk_mem_reclaim(sk);
}

void nd_read_queue_purge(struct sock* sk) {
	struct sk_buff *skb;
	while ((skb = __skb_dequeue(&sk->sk_receive_queue)) != NULL) {
		nd_rmem_free_skb(sk, skb);
	}
	nd_ofo_queue_purge(sk);
}

int nd_err(struct sk_buff *skb, u32 info)
{
	return 0;
	// return __nd4_lib_err(skb, info, &nd_table);
}


int sk_wait_ack(struct sock *sk, long *timeo)
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	int rc = 0;
	add_wait_queue(sk_sleep(sk), &wait);
	while(1) {
		if(sk->sk_state == TCP_CLOSE)
			break;
		if (signal_pending(current))
			break;
		sk_set_bit(SOCKWQ_ASYNC_WAITDATA, sk);
		rc = sk_wait_event(sk, timeo, sk->sk_state == TCP_CLOSE, &wait);
		sk_clear_bit(SOCKWQ_ASYNC_WAITDATA, sk);
	}
	remove_wait_queue(sk_sleep(sk), &wait);

	return rc;
}
EXPORT_SYMBOL(sk_wait_ack);

static int nd_push(struct sock *sk) {
	struct inet_sock *inet = inet_sk(sk);
	struct sk_buff *skb;
	struct nd_sock *nsk = nd_sk(sk);
	while((skb = skb_peek(&sk->sk_write_queue)) != NULL) {
		/* construct nd_conn_request */
		struct nd_conn_request* req = kzalloc(sizeof(*req), GFP_KERNEL);
		struct ndhdr* hdr;
	
		nd_conn_init_request(req, -1);
		req->state = ND_CONN_SEND_CMD_PDU;
		req->pdu_len = sizeof(struct ndhdr) + skb->len;
		req->data_len = skb->len;
		hdr = req->hdr;
	// struct sk_buff* skb = __construct_control_skb(sk, 0);
	// struct nd_flow_sync_hdr* fh;
	// struct ndhdr* dh; 
	// if(unlikely(!req || !sync)) {
	// 	return -N;
	// }
	// fh = (struct nd_flow_sync_hdr *) skb_put(skb, sizeof(struct nd_flow_sync_hdr));
	
	// dh = (struct ndhdr*) (&sync->common);
		req->skb = skb;
		hdr->len = htons(skb->len);
		hdr->type = DATA;
		hdr->source = inet->inet_sport;
		hdr->dest = inet->inet_dport;
		hdr->check = 0;
		hdr->doff = (sizeof(struct ndhdr)) << 2;
		hdr->seq = htonl(nsk->sender.write_seq);
		skb_dequeue(&sk->sk_write_queue);
			// kfree_skb(skb);
		sk->sk_wmem_queued -= skb->len;
		/*increment write seq */
		nsk->sender.write_seq += skb->len;
		/* queue the request */
		nd_conn_queue_request(req, false, true);
	}
	return 0;
}

/* copy from kcm sendmsg */
static int nd_sendmsg_locked(struct sock *sk, struct msghdr *msg, size_t len)
{
	// struct nd_sock *nsk = nd_sk(sk);
	struct sk_buff *skb = NULL;
	size_t copy, copied = 0;
	long timeo = sock_sndtimeo(sk, msg->msg_flags & MSG_DONTWAIT);
	/* SOCK DGRAM? */
	int eor = (sk->sk_socket->type == SOCK_DGRAM) ?
		  !(msg->msg_flags & MSG_MORE) : !!(msg->msg_flags & MSG_EOR);
	int err = -EPIPE;
	int i = 0;
	/* Per tcp_sendmsg this should be in poll */
	sk_clear_bit(SOCKWQ_ASYNC_NOSPACE, sk);

	if (sk->sk_err)
		goto out_error;

	// if (nsk->seq_skb) {
	// 	/* Previously opened message */
	// 	head = nsk->seq_skb;
	// 	skb = nd_tx_msg(head)->last_skb;
	// 	goto start;
	// }
	// skb = nd_write_queue_tail(sk);
	// if(skb) {
	// 	goto start;
	// }
	/* Call the sk_stream functions to manage the sndbuf mem. */
	// if (!sk_stream_memory_free(sk)) {
	// 	nd_push(nsk);
	// 	set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
	// 	err = sk_stream_wait_memory(sk, &timeo);
	// 	if (err)
	// 		goto out_error;
	// }

	// if (msg_data_left(msg)) {
	// 	/* New message, alloc head skb */
	// 	skb = alloc_skb(0, sk->sk_allocation);
	// 	while (!skb) {
	// 		nd_push(nsk);
	// 		err = sk_stream_wait_memory(sk, &timeo);
	// 		if (err)
	// 			goto out_error;

	// 		skb = alloc_skb(0, sk->sk_allocation);
	// 	}

	// 	skb = head;

	// 	/* Set ip_summed to CHECKSUM_UNNECESSARY to avoid calling
	// 	 * csum_and_copy_from_iter from skb_do_copy_data_nocache.
	// 	 */
	// 	skb->ip_summed = CHECKSUM_UNNECESSARY;
	// }

	while (msg_data_left(msg)) {
		bool merge = true;
		struct page_frag *pfrag = sk_page_frag(sk);
		if (!sk_page_frag_refill(sk, pfrag))
			goto wait_for_memory;
		skb = nd_write_queue_tail(sk);
		if(!skb) 
			goto create_new_skb;
		i = skb_shinfo(skb)->nr_frags;
		if (!skb_can_coalesce(skb, i, pfrag->page,
			 pfrag->offset)) {
			if (i == MAX_SKB_FRAGS) {
				goto create_new_skb;
			}
			merge = false;
		}
		copy = min_t(int, msg_data_left(msg),
			     pfrag->size - pfrag->offset);

		if (!sk_wmem_schedule(sk, copy))
			goto wait_for_memory;

		err = skb_copy_to_page_nocache(sk, &msg->msg_iter, skb,
					       pfrag->page,
					       pfrag->offset,
					       copy);
		if (err)
			goto out_error;
		/* Update the skb. */
		if (merge) {
			skb_frag_size_add(&skb_shinfo(skb)->frags[i - 1], copy);
		} else {
			skb_fill_page_desc(skb, i, pfrag->page,
					   pfrag->offset, copy);
			get_page(pfrag->page);
		}
		pfrag->offset += copy;
		copied += copy;
		continue;

create_new_skb:
		if (!sk_stream_memory_free(sk)) {
			set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
			goto wait_for_memory;
		}
		skb = alloc_skb(0, sk->sk_allocation);
		if(!skb)
			goto wait_for_memory;
		__skb_queue_tail(&sk->sk_write_queue, skb);
		continue;


wait_for_memory:
		nd_push(sk);
		err = sk_stream_wait_memory(sk, &timeo);
		if (err)
			goto out_error;
	}

	if (eor) {
		bool not_busy = skb_queue_empty(&sk->sk_write_queue);
		if(not_busy) {
			nd_push(sk);
		}
	}

	// ND_STATS_ADD(nsk->stats.tx_bytes, copied);

	release_sock(sk);
	return copied;

out_error:
	nd_push(sk);

	// if (copied && sock->type == SOCK_SEQPACKET) {
	// 	/* Wrote some bytes before encountering an
	// 	 * error, return partial success.
	// 	 */
	// 	goto partial_message;
	// }

	// if (head != nsk->seq_skb)
	// 	kfree_skb(head);

	err = sk_stream_error(sk, msg->msg_flags, err);

	/* make sure we wake any epoll edge trigger waiter */
	if (unlikely(skb_queue_len(&sk->sk_write_queue) == 0 && err == -EAGAIN))
		sk->sk_write_space(sk);

	return err;
}

int nd_sendmsg(struct sock *sk, struct msghdr *msg, size_t len)
{
	int ret = 0;
	lock_sock(sk);
	// nd_rps_record_flow(sk);
	ret = nd_sendmsg_locked(sk, msg, len);
	release_sock(sk);
	return ret;
}
EXPORT_SYMBOL(nd_sendmsg);

int nd_sendpage(struct sock *sk, struct page *page, int offset,
		 size_t size, int flags)
{
	printk(KERN_WARNING "unimplemented sendpage invoked on nd socket\n");
	return -ENOSYS;
// 	struct inet_sock *inet = inet_sk(sk);
// 	struct nd_sock *up = nd_sk(sk);
// 	int ret;

// 	if (flags & MSG_SENDPAGE_NOTLAST)
// 		flags |= MSG_MORE;

// 	if (!up->pending) {
// 		struct msghdr msg = {	.msg_flags = flags|MSG_MORE };

// 		/* Call nd_sendmsg to specify destination address which
// 		 * sendpage interface can't pass.
// 		 * This will succeed only when the socket is connected.
// 		 */
// 		ret = nd_sendmsg(sk, &msg, 0);
// 		if (ret < 0)
// 			return ret;
// 	}

// 	lock_sock(sk);

// 	if (unlikely(!up->pending)) {
// 		release_sock(sk);

// 		net_dbg_ratelimited("cork failed\n");
// 		return -EINVAL;
// 	}

// 	ret = ip_append_page(sk, &inet->cork.fl.u.ip4,
// 			     page, offset, size, flags);
// 	if (ret == -EOPNOTSUPP) {
// 		release_sock(sk);
// 		return sock_no_sendpage(sk->sk_socket, page, offset,
// 					size, flags);
// 	}
// 	if (ret < 0) {
// 		nd_flush_pending_frames(sk);
// 		goto out;
// 	}

// 	up->len += size;
// 	if (!(up->corkflag || (flags&MSG_MORE)))
// 		ret = nd_push_pending_frames(sk);
// 	if (!ret)
// 		ret = size;
// out:
// 	release_sock(sk);
// 	return ret;
// }

// #define ND_SKB_IS_STATELESS 0x80000000

// /* all head states (dst, sk, nf conntrack) except skb extensions are
//  * cleared by nd_rcv().
//  *
//  * We need to preserve secpath, if present, to eventually process
//  * IP_CMSG_PASSSEC at recvmsg() time.
//  *
//  * Other extensions can be cleared.
//  */
// static bool nd_try_make_stateless(struct sk_buff *skb)
// {
// 	if (!skb_has_extensions(skb))
// 		return true;

// 	if (!secpath_exists(skb)) {
// 		skb_ext_reset(skb);
// 		return true;
// 	}

// 	return false;
}

/* fully reclaim rmem/fwd memory allocated for skb */
static void nd_rmem_release(struct sock *sk, int size, int partial,
			     bool rx_queue_lock_held)
{
	struct nd_sock *up = nd_sk(sk);
	struct sk_buff_head *sk_queue;
	int amt;

	if (likely(partial)) {
		up->forward_deficit += size;
		size = up->forward_deficit;
		if (size < (sk->sk_rcvbuf >> 2) &&
		    !skb_queue_empty(&up->reader_queue))
			return;
	} else {
		size += up->forward_deficit;
	}
	up->forward_deficit = 0;

	/* acquire the sk_receive_queue for fwd allocated memory scheduling,
	 * if the called don't held it already
	 */
	sk_queue = &sk->sk_receive_queue;
	if (!rx_queue_lock_held)
		spin_lock(&sk_queue->lock);


	sk->sk_forward_alloc += size;
	amt = (sk->sk_forward_alloc - partial) & ~(SK_MEM_QUANTUM - 1);
	sk->sk_forward_alloc -= amt;

	if (amt)
		__sk_mem_reduce_allocated(sk, amt >> SK_MEM_QUANTUM_SHIFT);

	atomic_sub(size, &sk->sk_rmem_alloc);

	/* this can save us from acquiring the rx queue lock on next receive */
	skb_queue_splice_tail_init(sk_queue, &up->reader_queue);

	if (!rx_queue_lock_held)
		spin_unlock(&sk_queue->lock);
}

void nd_destruct_sock(struct sock *sk)
{

	/* reclaim completely the forward allocated memory */
	unsigned int total = 0;
	// struct sk_buff *skb;
	// struct udp_hslot* hslot = udp_hashslot(sk->sk_prot->h.udp_table, sock_net(sk),
	// 				     nd_sk(sk)->nd_port_hash);
	printk("call destruct sock \n");
	/* clean the message*/
	// skb_queue_splice_tail_init(&sk->sk_receive_queue, &dsk->reader_queue);
	// while ((skb = __skb_dequeue(&dsk->reader_queue)) != NULL) {
	// 	total += skb->truesize;
	// 	kfree_skb(skb);
	// }

	nd_rmem_release(sk, total, 0, true);
	inet_sock_destruct(sk);
}
EXPORT_SYMBOL_GPL(nd_destruct_sock);

int nd_init_sock(struct sock *sk)
{
	struct nd_sock* dsk = nd_sk(sk);
	nd_set_state(sk, TCP_CLOSE);
	skb_queue_head_init(&nd_sk(sk)->reader_queue);
	dsk->core_id = raw_smp_processor_id();
	printk("init sock\n");
	// next_going_id 
	// printk("remaining tokens:%d\n", nd_epoch.remaining_tokens);
	// atomic64_set(&dsk->next_outgoing_id, 1);
	// initialize the ready queue and its lock
	sk->sk_destruct = nd_destruct_sock;
	dsk->unsolved = 0;
	WRITE_ONCE(dsk->num_sacks, 0);
	WRITE_ONCE(dsk->grant_nxt, 0);
	WRITE_ONCE(dsk->prev_grant_nxt, 0);
	WRITE_ONCE(dsk->new_grant_nxt, 0);

	INIT_LIST_HEAD(&dsk->match_link);
	WRITE_ONCE(dsk->sender.write_seq, 0);
	WRITE_ONCE(dsk->sender.snd_nxt, 0);
	WRITE_ONCE(dsk->sender.snd_una, 0);

	WRITE_ONCE(dsk->receiver.free_flow, false);
	WRITE_ONCE(dsk->receiver.rcv_nxt, 0);
	WRITE_ONCE(dsk->receiver.last_ack, 0);
	WRITE_ONCE(dsk->receiver.copied_seq, 0);
	WRITE_ONCE(dsk->receiver.max_grant_batch, 0);
	WRITE_ONCE(dsk->receiver.max_gso_data, 0);
	WRITE_ONCE(dsk->receiver.finished_at_receiver, false);
	WRITE_ONCE(dsk->receiver.flow_finish_wait, false);
	WRITE_ONCE(dsk->receiver.rmem_exhausted, 0);
	WRITE_ONCE(dsk->receiver.prev_grant_bytes, 0);
	WRITE_ONCE(dsk->receiver.in_pq, false);
	WRITE_ONCE(dsk->receiver.last_rtx_time, ktime_get());
	atomic_set(&dsk->receiver.in_flight_bytes, 0);
	atomic_set(&dsk->receiver.backlog_len, 0);
	dsk->start_time = ktime_get();
	hrtimer_init(&dsk->receiver.flow_wait_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_SOFT);
	dsk->receiver.flow_wait_timer.function = &nd_flow_wait_event;

	WRITE_ONCE(sk->sk_sndbuf, nd_params.wmem_default);
	WRITE_ONCE(sk->sk_rcvbuf, nd_params.rmem_default);
	kfree_skb(sk->sk_tx_skb_cache);
	sk->sk_tx_skb_cache = NULL;
	/* reuse tcp rtx queue*/
	sk->tcp_rtx_queue = RB_ROOT;
	dsk->out_of_order_queue = RB_ROOT;
	// printk("flow wait at init:%d\n", dsk->receiver.flow_wait);
	return 0;
}
EXPORT_SYMBOL_GPL(nd_init_sock);

/*
 *	IOCTL requests applicable to the ND protocol
 */

int nd_ioctl(struct sock *sk, int cmd, unsigned long arg)
{
	printk(KERN_WARNING "unimplemented ioctl invoked on ND socket\n");
	return -ENOSYS;
}
EXPORT_SYMBOL(nd_ioctl);


void nd_try_send_ack(struct sock *sk, int copied) {
	struct nd_sock *dsk = nd_sk(sk);
	struct inet_sock *inet = inet_sk(sk);
	if(copied > 0 && dsk->receiver.rcv_nxt >= dsk->receiver.last_ack + dsk->receiver.max_grant_batch / 5) {
		// int grant_len = min_t(int, len, dsk->receiver.max_gso_data);
		// int available_space = nd_space(sk);
		// if(grant_len > available_space || grant_len < )
		// 	return;
		// printk("try to send ack \n");
		nd_xmit_control(construct_ack_pkt(sk, dsk->receiver.rcv_nxt), sk, inet->inet_dport); 
		dsk->receiver.last_ack = dsk->receiver.rcv_nxt;
	}
}

bool nd_try_send_token(struct sock *sk) {
	if(test_bit(ND_TOKEN_TIMER_DEFERRED, &sk->sk_tsq_flags)) {
		// struct nd_sock *dsk = nd_sk(sk);
		// int grant_len = min_t(int, len, dsk->receiver.max_gso_data);
		// int available_space = nd_space(sk);
		// if(grant_len > available_space || grant_len < )
		// 	return;
		// printk("try to send token \n");
		int grant_bytes = calc_grant_bytes(sk);

		// printk("grant bytes delay:%d\n", grant_bytes);
		if (grant_bytes > 0) {
			// spin_lock_bh(&sk->sk_lock.slock);
			xmit_batch_token(sk, grant_bytes, false);
			// spin_unlock_bh(&sk->sk_lock.slock);
			return true;
		}
	}
	return false;

}
/*
 * 	This should be easy, if there is something there we
 * 	return it, otherwise we block.
 */

int nd_recvmsg(struct sock *sk, struct msghdr *msg, size_t len, int nonblock,
		int flags, int *addr_len)
{

	struct nd_sock *dsk = nd_sk(sk);
	int copied = 0;
	// u32 peek_seq;
	u32 *seq;
	unsigned long used;
	int err;
	// int inq;
	int target;		/* Read at least this many bytes */
	long timeo;
	// int trigger_tokens = 1;
	struct sk_buff *skb, *last, *tmp;
	// u32 urg_hole = 0;
	// struct scm_timestamping_internal tss;
	// int cmsg_flags;
	// printk("recvmsg: sk->rxhash:%u\n", sk->sk_rxhash);
	// printk("rcvmsg core:%d\n", raw_smp_processor_id());

	nd_rps_record_flow(sk);

	// if (unlikely(flags & MSG_ERRQUEUE))
	// 	return inet_recv_error(sk, msg, len, addr_len);
	// printk("start recvmsg \n");
	target = sock_rcvlowat(sk, flags & MSG_WAITALL, len);
	// printk("target bytes:%d\n", target);

	if (sk_can_busy_loop(sk) && skb_queue_empty_lockless(&sk->sk_receive_queue) &&
	    (sk->sk_state == ND_RECEIVER))
		sk_busy_loop(sk, nonblock);

	lock_sock(sk);
	err = -ENOTCONN;


	// cmsg_flags = tp->recvmsg_inq ? 1 : 0;
	timeo = sock_rcvtimeo(sk, nonblock);

	if (sk->sk_state != ND_RECEIVER)
		goto out;
	/* Urgent data needs to be handled specially. */
	// if (flags & MSG_OOB)
	// 	goto recv_urg;

	// if (unlikely(tp->repair)) {
	// 	err = -EPERM;
		// if (!(flags & MSG_PEEK))
		// 	goto out;

		// if (tp->repair_queue == TCP_SEND_QUEUE)
		// 	goto recv_sndq;

		// err = -EINVAL;
		// if (tp->repair_queue == TCP_NO_QUEUE)
		// 	goto out;

		/* 'common' recv queue MSG_PEEK-ing */
//	}

	seq = &dsk->receiver.copied_seq;
	// if (flags & MSG_PEEK) {
	// 	peek_seq = dsk->receiver.copied_seq;
	// 	seq = &peek_seq;
	// }

	do {
		u32 offset;

		/* Are we at urgent data? Stop if we have read anything or have SIGURG pending. */
		// if (tp->urg_data && tp->urg_seq == *seq) {
		// 	if (copied)
		// 		break;
		// 	if (signal_pending(current)) {
		// 		copied = timeo ? sock_intr_errno(timeo) : -EAGAIN;
		// 		break;
		// 	}
		// }

		/* Next get a buffer. */

		last = skb_peek_tail(&sk->sk_receive_queue);
		skb_queue_walk_safe(&sk->sk_receive_queue, skb, tmp) {
			last = skb;

			/* Now that we have two receive queues this
			 * shouldn't happen.
			 */
			if (WARN(before(*seq, ND_SKB_CB(skb)->seq),
				 "ND recvmsg seq # bug: copied %X, seq %X, rcvnxt %X, fl %X\n",
				 *seq, ND_SKB_CB(skb)->seq, dsk->receiver.rcv_nxt,
				 flags))
				break;

			offset = *seq - ND_SKB_CB(skb)->seq;
			// if (unlikely(TCP_SKB_CB(skb)->tcp_flags & TCPHDR_SYN)) {
			// 	pr_err_once("%s: found a SYN, please report !\n", __func__);
			// 	offset--;
			// }
			if (offset < skb->len) {
				goto found_ok_skb; 
			}
			else {
				WARN_ON(true);
				// __skb_unlink(skb, &sk->sk_receive_queue);

				// kfree_skb(skb);
				// atomic_sub(skb->truesize, &sk->sk_rmem_alloc);
			}
			// if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN)
			// 	goto found_fin_ok;
			// WARN(!(flags & MSG_PEEK),
			//      "TCP recvmsg seq # bug 2: copied %X, seq %X, rcvnxt %X, fl %X\n",
			//      *seq, ND_SKB_CB(skb)->seq, dsk->receiver.rcv_nxt, flags);
		}

		/* Well, if we have backlog, try to process it now yet. */

		if (copied >= target && !READ_ONCE(sk->sk_backlog.tail))
			break;

		if (copied) {
			if (sk->sk_err ||
			    sk->sk_state == TCP_CLOSE ||
			    (sk->sk_shutdown & RCV_SHUTDOWN) ||
			    !timeo ||
			    signal_pending(current))
				break;
		} else {
			if (sock_flag(sk, SOCK_DONE))
				break;

			if (sk->sk_err) {
				copied = sock_error(sk);
				break;
			}

			if (sk->sk_shutdown & RCV_SHUTDOWN)
				break;

			if (sk->sk_state == TCP_CLOSE) {
				/* This occurs when user tries to read
				 * from never connected socket.
				 */
				copied = -ENOTCONN;
				break;
			}

			if (!timeo) {
				copied = -EAGAIN;
				break;
			}

			if (signal_pending(current)) {
				copied = sock_intr_errno(timeo);
				break;
			}
		}

		// tcp_cleanup_rbuf(sk, copied);
		nd_try_send_token(sk);
		// printk("release sock");
		if (copied >= target) {
			/* Do not sleep, just process backlog. */
			/* Release sock will handle the backlog */
			// printk("call release sock1\n");
			release_sock(sk);
			lock_sock(sk);
		} else {
			sk_wait_data(sk, &timeo, last);
		}

		// if ((flags & MSG_PEEK) &&
		//     (peek_seq - copied - urg_hole != tp->copied_seq)) {
		// 	net_dbg_ratelimited("TCP(%s:%d): Application bug, race in MSG_PEEK\n",
		// 			    current->comm,
		// 			    task_pid_nr(current));
		// 	peek_seq = dsk->receiver.copied_seq;
		// }
		continue;

found_ok_skb:
		/* Ok so how much can we use? */
		used = skb->len - offset;
		if (len < used)
			used = len;
		nd_try_send_token(sk);

		/* Do we have urgent data here? */
		// if (tp->urg_data) {
		// 	u32 urg_offset = tp->urg_seq - *seq;
		// 	if (urg_offset < used) {
		// 		if (!urg_offset) {
		// 			if (!sock_flag(sk, SOCK_URGINLINE)) {
		// 				WRITE_ONCE(*seq, *seq + 1);
		// 				urg_hole++;
		// 				offset++;
		// 				used--;
		// 				if (!used)
		// 					goto skip_copy;
		// 			}
		// 		} else
		// 			used = urg_offset;
		// 	}
		// }

		if (!(flags & MSG_TRUNC)) {
			err = skb_copy_datagram_msg(skb, offset, msg, used);
			// printk("copy data done: %d\n", used);
			if (err) {
				/* Exception. Bailout! */
				if (!copied)
					copied = -EFAULT;
				break;
			}
		}

		WRITE_ONCE(*seq, *seq + used);
		copied += used;
		len -= used;
		if (used + offset < skb->len)
			continue;
		__skb_unlink(skb, &sk->sk_receive_queue);
		atomic_sub(skb->truesize, &sk->sk_rmem_alloc);
		kfree_skb(skb);

		// if (copied > 3 * trigger_tokens * dsk->receiver.max_gso_data) {
		// 	// nd_try_send_token(sk);
		// 	trigger_tokens += 1;
			
		// }
		// nd_try_send_token(sk);

		// tcp_rcv_space_adjust(sk);

// skip_copy:
		// if (tp->urg_data && after(tp->copied_seq, tp->urg_seq)) {
		// 	tp->urg_data = 0;
		// 	tcp_fast_path_check(sk);
		// }
		// if (used + offset < skb->len)
		// 	continue;

		// if (TCP_SKB_CB(skb)->has_rxtstamp) {
		// 	tcp_update_recv_tstamps(skb, &tss);
		// 	cmsg_flags |= 2;
		// }
		// if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN)
		// 	goto found_fin_ok;
		// if (!(flags & MSG_PEEK))
		// 	sk_eat_skb(sk, skb);
		continue;

// found_fin_ok:
		/* Process the FIN. */
		// WRITE_ONCE(*seq, *seq + 1);
		// if (!(flags & MSG_PEEK))
		// 	sk_eat_skb(sk, skb);
		// break;
	} while (len > 0);

	/* According to UNIX98, msg_name/msg_namelen are ignored
	 * on connected socket. I was just happy when found this 8) --ANK
	 */

	/* Clean up data we have read: This will do ACK frames. */
	// tcp_cleanup_rbuf(sk, copied);
	nd_try_send_token(sk);
	if (dsk->receiver.copied_seq == dsk->total_length) {
		printk("call tcp close in the recv msg\n");
		nd_set_state(sk, TCP_CLOSE);
	} else {
		// nd_try_send_token(sk);
	}
	release_sock(sk);

	// if (cmsg_flags) {
	// 	if (cmsg_flags & 2)
	// 		tcp_recv_timestamp(msg, sk, &tss);
	// 	if (cmsg_flags & 1) {
	// 		inq = tcp_inq_hint(sk);
	// 		put_cmsg(msg, SOL_TCP, TCP_CM_INQ, sizeof(inq), &inq);
	// 	}
	// }
	// printk("recvmsg\n");
	return copied;

out:
	release_sock(sk);
	return err;

// recv_urg:
// 	err = tcp_recv_urg(sk, msg, len, flags);
// 	goto out;

// recv_sndq:
// 	// err = tcp_peek_sndq(sk, msg, len);
// 	goto out;
}

int nd_pre_connect(struct sock *sk, struct sockaddr *uaddr, int addr_len)
{
	if (addr_len < sizeof(struct sockaddr_in))
 		return -EINVAL;

 	return BPF_CGROUP_RUN_PROG_INET4_CONNECT_LOCK(sk, uaddr);
}
EXPORT_SYMBOL(nd_pre_connect);

int nd_disconnect(struct sock *sk, int flags)
{
	struct inet_sock *inet = inet_sk(sk);
 	/*
 	 *	1003.1g - break association.
 	 */

 	sk->sk_state = TCP_CLOSE;
 	inet->inet_daddr = 0;
 	inet->inet_dport = 0;
 	sock_rps_reset_rxhash(sk);
 	sk->sk_bound_dev_if = 0;
 	if (!(sk->sk_userlocks & SOCK_BINDADDR_LOCK)) {
 		inet_reset_saddr(sk);
 		if (sk->sk_prot->rehash &&
 		    (sk->sk_userlocks & SOCK_BINDPORT_LOCK))
 			sk->sk_prot->rehash(sk);
 	}

 	if (!(sk->sk_userlocks & SOCK_BINDPORT_LOCK)) {
 		sk->sk_prot->unhash(sk);
 		inet->inet_sport = 0;
 	}
 	sk_dst_reset(sk);
 	return 0;
}
EXPORT_SYMBOL(nd_disconnect);

int nd_v4_early_demux(struct sk_buff *skb)
{
	// struct net *net = dev_net(skb->dev);
	// struct in_device *in_dev = NULL;
	// const struct iphdr *iph;
	// const struct ndhdr *uh;
	// struct sock *sk = NULL;
	// struct dst_entry *dst;
	// int dif = skb->dev->ifindex;
	// int sdif = inet_sdif(skb);
	// int ours;

	/* validate the packet */
	// printk("early demux");
	return 0; 
	// if(skb->pkt_type != PACKET_HOST) {
	// 	return 0;
	// }
	// if (!pskb_may_pull(skb, skb_transport_offset(skb) + sizeof(struct ndhdr)))
	// 	return 0;

	// iph = ip_hdr(skb);
	// uh = nd_hdr(skb);

    // // if (th->doff < sizeof(struct tcphdr) / 4)
    // //             return 0;
    // sk = __nd_lookup_established(dev_net(skb->dev), &nd_hashinfo,
    //                                iph->saddr, uh->source,
    //                                iph->daddr, ntohs(uh->dest),
    //                                skb->skb_iif, sdif);

    // if (sk) {
    //         skb->sk = sk;
    //         skb->destructor = sock_edemux;
    //         if (sk_fullsock(sk)) {
    //                 struct dst_entry *dst = READ_ONCE(sk->sk_rx_dst);

    //                 if (dst)
    //                         dst = dst_check(dst, 0);
    //                 if (dst &&
    //                     inet_sk(sk)->rx_dst_ifindex == skb->skb_iif)
    //                         skb_dst_set_noref(skb, dst);
    //         }
    // }
	// return 0;
}


int nd_rcv(struct sk_buff *skb)
{
	// printk("receive nd rcv\n");
	// skb_dump(KERN_WARNING, skb, false);
	struct ndhdr* dh;
	// printk("skb->len:%d\n", skb->len);
	if (!pskb_may_pull(skb, sizeof(struct ndhdr)))
		goto drop;		/* No space for header. */
	dh = nd_hdr(skb);
	// printk("dh == NULL?: %d\n", dh == NULL);
	// printk("receive pkt: %d\n", dh->type);
	// printk("end ref \n");
	if(dh->type == DATA) {
		return nd_handle_data_pkt(skb);
		// return __nd4_lib_rcv(skb, &nd_table, IPPROTO_VIRTUAL_SOCK);
	} else if (dh->type == SYNC) {
		return nd_handle_sync_pkt(skb);
	} else if (dh->type == TOKEN) {
		return nd_handle_token_pkt(skb);
	} else if (dh->type == FIN) {
		return nd_handle_fin_pkt(skb);
	} else if (dh->type == ACK) {
		return nd_handle_ack_pkt(skb);
	} else if (dh->type == SYNC_ACK) {
		return nd_handle_sync_ack_pkt(skb);
	}
	//  else if (dh->type == SYNC_ACK) {
	// 	return nd_handle_sync_ack_pkt(skb);
	// }
	// else if (dh->type == RTS) {
	// 	return nd_handle_rts(skb, &nd_match_table, &nd_epoch);
	// } else if (dh->type == GRANT) {
	// 	return nd_handle_grant(skb, &nd_match_table, &nd_epoch);
	// } else if (dh->type == ACCEPT) {
	// 	return nd_handle_accept(skb, &nd_match_table, &nd_epoch);
	// }


drop:

	kfree_skb(skb);
	return 0;

	return 0;
	// return __nd4_lib_rcv(skb, &nd_table, IPPROTO_VIRTUAL_SOCK);
}


void nd_destroy_sock(struct sock *sk)
{
	// struct udp_hslot* hslot = udp_hashslot(sk->sk_prot->h.udp_table, sock_net(sk),
	// 				     nd_sk(sk)->nd_port_hash);
	struct nd_sock *up = nd_sk(sk);
	// struct inet_sock *inet = inet_sk(sk);
	struct rcv_core_entry *entry = &rcv_core_tab.table[raw_smp_processor_id()];
	local_bh_disable();
	bh_lock_sock(sk);
	hrtimer_cancel(&up->receiver.flow_wait_timer);
	test_and_clear_bit(ND_WAIT_DEFERRED, &sk->sk_tsq_flags);
	up->receiver.flow_finish_wait = false;
	if(sk->sk_state == ND_SENDER || sk->sk_state == ND_RECEIVER) {
		printk("send fin pkt\n");
		// nd_xmit_control(construct_fin_pkt(sk), sk, inet->inet_dport); 
	}
	printk("reach here:%d", __LINE__);
	nd_set_state(sk, TCP_CLOSE);
	// nd_flush_pending_frames(sk);
	nd_write_queue_purge(sk);
	nd_read_queue_purge(sk);
	
	bh_unlock_sock(sk);
	local_bh_enable();

	// printk("sk->sk_wmem_queued:%d\n",sk->sk_wmem_queued);
	spin_lock_bh(&entry->lock);
	// printk("dsk->match_link:%p\n", &up->match_link);
	if(up->receiver.in_pq)
		nd_pq_delete(&entry->flow_q, &up->match_link);
	spin_unlock_bh(&entry->lock);
	// if (static_branch_unlikely(&nd_encap_needed_key)) {
	// 	if (up->encap_type) {
	// 		void (*encap_destroy)(struct sock *sk);
	// 		encap_destroy = READ_ONCE(up->encap_destroy);
	// 		if (encap_destroy)
	// 			encap_destroy(sk);
	// 	}
	// 	if (up->encap_enabled)
	// 		static_branch_dec(&nd_encap_needed_key);
	// }
}


int nd_setsockopt(struct sock *sk, int level, int optname,
		   char __user *optval, unsigned int optlen)
{
	printk(KERN_WARNING "unimplemented setsockopt invoked on ND socket:"
			" level %d, optname %d, optlen %d\n",
			level, optname, optlen);
	return -EINVAL;
	// if (level == SOL_VIRTUAL_SOCK)
	// 	return nd_lib_setsockopt(sk, level, optname, optval, optlen,
	// 				  nd_push_pending_frames);
	// return ip_setsockopt(sk, level, optname, optval, optlen);
}

// #ifdef CONFIG_COMPAT
// int compat_nd_setsockopt(struct sock *sk, int level, int optname,
// 			  char __user *optval, unsigned int optlen)
// {
// 	if (level == SOL_VIRTUAL_SOCK)
// 		return nd_lib_setsockopt(sk, level, optname, optval, optlen,
// 					  nd_push_pending_frames);
// 	return compat_ip_setsockopt(sk, level, optname, optval, optlen);
// }
// #endif

int nd_lib_getsockopt(struct sock *sk, int level, int optname,
		       char __user *optval, int __user *optlen)
{
	printk(KERN_WARNING "unimplemented getsockopt invoked on ND socket:"
			" level %d, optname %d\n", level, optname);
	return -EINVAL;
	// struct nd_sock *up = nd_sk(sk);
	// int val, len;

	// if (get_user(len, optlen))
	// 	return -EFAULT;

	// len = min_t(unsigned int, len, sizeof(int));

	// if (len < 0)
	// 	return -EINVAL;

	// switch (optname) {
	// case ND_CORK:
	// 	val = up->corkflag;
	// 	break;

	// case ND_ENCAP:
	// 	val = up->encap_type;
	// 	break;

	// case ND_NO_CHECK6_TX:
	// 	val = up->no_check6_tx;
	// 	break;

	// case ND_NO_CHECK6_RX:
	// 	val = up->no_check6_rx;
	// 	break;

	// case ND_SEGMENT:
	// 	val = up->gso_size;
	// 	break;
	// default:
	// 	return -ENOPROTOOPT;
	// }

	// if (put_user(len, optlen))
	// 	return -EFAULT;
	// if (copy_to_user(optval, &val, len))
	// 	return -EFAULT;
	// return 0;
}
EXPORT_SYMBOL(nd_lib_getsockopt);

int nd_getsockopt(struct sock *sk, int level, int optname,
		   char __user *optval, int __user *optlen)
{
	printk(KERN_WARNING "unimplemented getsockopt invoked on ND socket:"
			" level %d, optname %d\n", level, optname);
	return -EINVAL;
}

__poll_t nd_poll(struct file *file, struct socket *sock, poll_table *wait)
{
	printk(KERN_WARNING "unimplemented poll invoked on ND socket\n");
	return -ENOSYS;
}
EXPORT_SYMBOL(nd_poll);

int nd_abort(struct sock *sk, int err)
{
	printk(KERN_WARNING "unimplemented abort invoked on ND socket\n");
	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(nd_abort);

u32 nd_flow_hashrnd(void)
{
	static u32 hashrnd __read_mostly;

	net_get_random_once(&hashrnd, sizeof(hashrnd));

	return hashrnd;
}
EXPORT_SYMBOL(nd_flow_hashrnd);

static void __nd_sysctl_init(struct net *net)
{
	net->ipv4.sysctl_udp_rmem_min = SK_MEM_QUANTUM;
	net->ipv4.sysctl_udp_wmem_min = SK_MEM_QUANTUM;

#ifdef CONFIG_NET_L3_MASTER_DEV
	net->ipv4.sysctl_udp_l3mdev_accept = 0;
#endif
}

static int __net_init nd_sysctl_init(struct net *net)
{
	__nd_sysctl_init(net);
	return 0;
}

static struct pernet_operations __net_initdata nd_sysctl_ops = {
	.init	= nd_sysctl_init,
};

void __init nd_init(void)
{
	unsigned long limit;
	// unsigned int i;

	printk("try to add nd table \n");

	nd_hashtable_init(&nd_hashinfo, 0);

	limit = nr_free_buffer_pages() / 8;
	limit = max(limit, 128UL);
	sysctl_nd_mem[0] = limit / 4 * 3;
	sysctl_nd_mem[1] = limit;
	sysctl_nd_mem[2] = sysctl_nd_mem[0] * 2;

	__nd_sysctl_init(&init_net);
	/* 16 spinlocks per cpu */
	// nd_busylocks_log = ilog2(nr_cpu_ids) + 4;
	// nd_busylocks = kmalloc(sizeof(spinlock_t) << nd_busylocks_log,
	// 			GFP_KERNEL);
	// if (!nd_busylocks)
	// 	panic("ND: failed to alloc nd_busylocks\n");
	// for (i = 0; i < (1U << nd_busylocks_log); i++)
	// 	spin_lock_init(nd_busylocks + i);
	if (register_pernet_subsys(&nd_sysctl_ops)) 
		panic("ND: failed to init sysctl parameters.\n");

	printk("ND init complete\n");

}

void nd_destroy() {
	printk("try to destroy peer table\n");
	printk("try to destroy nd socket table\n");
	nd_hashtable_destroy(&nd_hashinfo);
	// kfree(nd_busylocks);
}