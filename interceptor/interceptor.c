/**
 * @file interceptor/interceptor.c
 * @brief The Trustbase TCP middle functions.
 */

#ifndef KBUILD_MODNAME
#	define KBUILD_MODNAME KBUILD_STR(trustbase_linux)
#endif
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/syscalls.h> // For kallsyms lookups
#include <linux/sched.h> // For current (pointer to task)
#include <linux/pid.h> // For pid_t
#include <linux/socket.h> // For socket structures
#include <linux/slab.h> // For memory allocation
#include <linux/tcp.h> // For TCP structures
#include <net/ip.h>
#include <linux/netfilter_ipv4.h> // For nat_ops registration	
#include <linux/delay.h>

#include "../util/ktb_logging.h" // For logging
#include "interceptor.h"
#include "connection_state.h" // For accessing handler functions

#define NAT_SOCKOPT_BASE	85
#define NAT_SOCKOPT_SET		(NAT_SOCKOPT_BASE)
#define NAT_SOCKOPT_GET		(NAT_SOCKOPT_BASE)
#define NAT_SOCKOPT_MAX	(NAT_SOCKOPT_BASE + 1)

// NAT functionality for sslsplit
static int set_orig_dst(struct sock *sk, int cmd, void __user *user, unsigned int len);
static int get_orig_dst(struct sock *sk, int cmd, void __user *user, int *len);
static struct nf_sockopt_ops nat_ops = {
	.pf = PF_INET,
	.set_optmin = NAT_SOCKOPT_SET,
	.set_optmax = NAT_SOCKOPT_MAX,
	.set = set_orig_dst,
	.get_optmin = NAT_SOCKOPT_GET,
	.get_optmax = NAT_SOCKOPT_MAX,
	.get = get_orig_dst,
	.owner = THIS_MODULE,
};

// TCP IPv6-specific reference functions
int (*ref_tcp_v4_connect)(struct sock *sk, struct sockaddr *uaddr, int addr_len);
// TCP IPv4-specific wrapper functions
int (*ref_tcp_v6_connect)(struct sock *sk, struct sockaddr *uaddr, int addr_len);
// TCP General reference functions
int (*ref_tcp_disconnect)(struct sock *sk, int flags);
void (*ref_tcp_close)(struct sock *sk, long timeout);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
int (*ref_tcp_sendmsg)(struct sock *sk, struct msghdr *msg, size_t size);
int (*ref_tcp_recvmsg)(struct sock *sk, struct msghdr *msg, size_t len, int nonblock, int flags, int *addr_len);
#else
int (*ref_tcp_sendmsg)(struct kiocb *iocb, struct sock *sk, struct msghdr *msg, size_t size);
int (*ref_tcp_recvmsg)(struct kiocb *iocb, struct sock *sk, struct msghdr *msg, size_t len, int nonblock, int flags, int *addr_len);
#endif

// Reference function for tcp v4 and v6 accept() calls
struct sock *(*ref_inet_csk_accept)(struct sock *sk, int flags, int *err, bool kern);

// TCP IPv4-specific reference functions
int new_tcp_v4_connect(struct sock *sk, struct sockaddr *uaddr, int addr_len);
// TCP IPv6-specific wrapper functions
int new_tcp_v6_connect(struct sock *sk, struct sockaddr *uaddr, int addr_len);
// TCP General wrapper functions
int new_tcp_disconnect(struct sock *sk, int flags);
void new_tcp_close(struct sock *sk, long timeout);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
int new_tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size);
int new_tcp_recvmsg(struct sock *sk, struct msghdr *msg, size_t len, int nonblock, int flags, int *addr_len);
#else
int new_tcp_sendmsg(struct kiocb *iocb, struct sock *sk, struct msghdr *msg, size_t size);
int new_tcp_recvmsg(struct kiocb *iocb, struct sock *sk, struct msghdr *msg, size_t len, int nonblock, int flags, int *addr_len);
#endif
// New function for tcp v4 and v6 accept() calls
struct sock* new_inet_csk_accept(struct sock *sk, int flags, int *err, bool kern);


// Helpers
static conn_state_t* start_conn_state(pid_t pid, pid_t tgid, struct sockaddr *uaddr, int is_ipv6, int addr_len, struct socket* sock);
static int stop_conn_state(conn_state_t* conn_state);

// Variables for NAT engine
struct proxy_accept_list_t proxy_accept_list; 
static int nat_ops_registered;

// Global ops registration
static proxy_handler_ops_t* ops;

// OS Structures to hook into for interception
extern struct proto tcp_prot;
struct proto * tcpv6_prot_ptr;

/**
 * Register the proxy by storing the old TCP function pointers, and replacing them with custom functions.
 * @param reg_ops the struct containg the custom operation functions.
 * @pre System TCP pointers point to the original tcp_prot functions.
 * @post System TCP pointers point to custom functions and ops has pointers to the correct Trustbase operation functions.
 * @return 0
 */
int proxy_register(proxy_handler_ops_t* reg_ops) {
	// Initialize buckets in hash table
	conn_state_init_all();

	ops = reg_ops;

	// Save all references to original TCP functionality and override them with wrappers
	ktblog(LOG_INFO, "address of tcp_prot is %p", &tcp_prot);
	ref_tcp_v4_connect = (void *)tcp_prot.connect;
	ref_tcp_disconnect = (void *)tcp_prot.disconnect;
	ref_tcp_close = (void *)tcp_prot.close;
	ref_tcp_sendmsg = (void *)tcp_prot.sendmsg;
	ref_tcp_recvmsg = (void *)tcp_prot.recvmsg;
	ref_inet_csk_accept = (void *)tcp_prot.accept;
	tcp_prot.connect = new_tcp_v4_connect;
	tcp_prot.disconnect = new_tcp_disconnect;
	tcp_prot.close = new_tcp_close;
	tcp_prot.sendmsg = new_tcp_sendmsg;
	tcp_prot.recvmsg = new_tcp_recvmsg;
	//tcp_prot.accept = new_inet_csk_accept;
	if ((tcpv6_prot_ptr = (void *)kallsyms_lookup_name("tcpv6_prot")) == 0) {
		ktblog(LOG_WARNING, "tcpv6_prot lookup failed, not intercepting IPv6 traffic");
	}
	else {
		ktblog(LOG_INFO, "tcpv6_prot lookup succeeded, address is %p", tcpv6_prot_ptr);
		ref_tcp_v6_connect = (void *)tcpv6_prot_ptr->connect;
		tcpv6_prot_ptr->connect = new_tcp_v6_connect;
		tcpv6_prot_ptr->disconnect = new_tcp_disconnect;
		tcpv6_prot_ptr->close = new_tcp_close;
		tcpv6_prot_ptr->sendmsg = new_tcp_sendmsg;
		tcpv6_prot_ptr->recvmsg = new_tcp_recvmsg;
		//tcpv6_prot_ptr->accept = new_inet_csk_accept;
	}
	return 0;
}

/**
 * Restores the original TCP functions and frees the connection state.
 * @post The tcp_prot functions point to the original TCP functions.
 * @return 0
 */
int proxy_unregister(void) {
	// Restore original TCP functions
	tcp_prot.connect = ref_tcp_v4_connect;
	tcp_prot.disconnect = ref_tcp_disconnect;
	tcp_prot.close = ref_tcp_close;
	tcp_prot.sendmsg = ref_tcp_sendmsg;
	tcp_prot.recvmsg = ref_tcp_recvmsg;
	//tcp_prot.accept = ref_inet_csk_accept;
	if (tcpv6_prot_ptr != 0) {
		tcpv6_prot_ptr->connect = ref_tcp_v6_connect;
		tcpv6_prot_ptr->disconnect = ref_tcp_disconnect;
		tcpv6_prot_ptr->close = ref_tcp_close;
		tcpv6_prot_ptr->sendmsg = ref_tcp_sendmsg;
		tcpv6_prot_ptr->recvmsg = ref_tcp_recvmsg;
		//tcpv6_prot_ptr->accept = ref_inet_csk_accept;
	}

	// Free up conn state memory
	conn_state_delete_all();
	return 0;
}

int nat_ops_register(void) {
	int err;
	INIT_LIST_HEAD(&proxy_accept_list.list);
	err = nf_register_sockopt(&nat_ops);
	if (err != 0) {
		ktblog(LOG_ERROR, "Failed to register new sock opts with kernel, locally proxied connections will fail");
	}
	nat_ops_registered = 1;
	return 0;
}

int nat_ops_unregister(void) {
	struct list_head* cur;
	struct list_head* q;
	proxy_accept_list_t* tmp;
	nat_ops_registered = 0;
	nf_unregister_sockopt(&nat_ops);
	list_for_each_safe(cur, q, &proxy_accept_list.list) {
		tmp = list_entry(cur, proxy_accept_list_t, list);
		list_del(cur);
		kfree(tmp);
	}
	return 0;
}

int add_to_proxy_accept_list(__be16 src_port, struct sockaddr* addr, int is_ipv6) {
	struct proxy_accept_list_t* tmp;
	if (nat_ops_registered != 1) {
		return 1;
	}
	if (addr == NULL) {
		ktblog(LOG_ERROR, "addr in proxy original destination info is NULL");
		return 1;
	}
	tmp = (proxy_accept_list_t*)kmalloc(sizeof(proxy_accept_list_t), GFP_KERNEL);
	if (tmp == NULL) {
		ktblog(LOG_ERROR, "Failed to allocate space for proxy original destination info");
		return 1;
	}
	if (is_ipv6) {
		tmp->addr_v6 = *(struct sockaddr_in6*)addr;
	}
	else {
		tmp->addr_v4 = *(struct sockaddr_in*)addr;
	}
	tmp->src_port = src_port;
	list_add(&(tmp->list), &(proxy_accept_list.list));
	return 0;
}

/**
 * Creates a new connection state.
 * @see handshake-handler/handshake_handler.c:tb_state_init
 * @see interceptor/connection_state.c:conn_state_create
 * @param pid The process id for the connection.
 * @param uaddr A pointer to the stuct for the userspace address for the task.
 * @param is_ipv6 0 if the connecion is not using IPv6.
 * @param addr_len The length of the address.
 * @param sock A pointer to the struct for the socket.
 * @return The pointer to a new connection state
 */
conn_state_t* start_conn_state(pid_t pid, pid_t tgid, struct sockaddr *uaddr, int is_ipv6, int addr_len, struct socket* sock) {
	conn_state_t* ret;
	ret = conn_state_create(pid, sock);
	if (ret != NULL) {
		ret->state = ops->state_init(ret->pid, tgid, sock, uaddr, is_ipv6, addr_len);
		if (ret->state == NULL) {
			stop_conn_state(ret);
			return NULL;
		}
	}
	return ret;
}

/**
 * Stops and frees a connection state.
 * @see handshake-handler/handshake_handler.c:tb_state_free
 * @see interceptor/connection_state.c:conn_state_delete
 * @param conn_state A pointer to a connection to be freed.
 * @return 1 if found and freed, 0 if not
 */
int stop_conn_state(conn_state_t* conn_state) {
	if (conn_state->state != NULL) {
		ops->state_free(conn_state->state);
	}
	return conn_state_delete(conn_state->pid, conn_state->sock);
}

// Wrapper definitions

/**
 * Runs a normal TCP connect, but stores the connection for monitoring.
 * @param sk A pointer to a sock struct for the connection.
 * @param uaddr A pointer to a sockaddr structure for the userspace address.
 * @param addr_len The length of the address.
 * @return tcp connect error code
 */
int new_tcp_v4_connect(struct sock *sk, struct sockaddr *uaddr, int addr_len) {
	int ret;
	struct socket* sock;
	sock = sk->sk_socket;
	ret = ref_tcp_v4_connect(sk, uaddr, addr_len);
	//ktblog(LOG_DEBUG, "We got a tcp_v4_connect");
	if (uaddr->sa_family == AF_INET) {
		//print_call_info("Calling connect (v4) to addres %pI4:%d", 
		//	&((struct sockaddr_in*)uaddr)->sin_addr,
		//	ntohs(((struct sockaddr_in*)uaddr)->sin_port));
	}
	start_conn_state(current->pid, current->tgid, uaddr, 0, addr_len, sock);
	return ret;
}

/**
 * Runs a normal TCPv6 connections, but stores the connection for monitoring.
 * @param sk A pointer to a sock struct for the connection.
 * @param uaddr A pointer to a sockaddr structure for the userspace address.
 * @param addr_len The length of the address.
 * @return tcp connect error code
 */
int new_tcp_v6_connect(struct sock *sk, struct sockaddr *uaddr, int addr_len) {
	int ret;
	struct socket* sock;
	sock = sk->sk_socket;
	ret = ref_tcp_v6_connect(sk, uaddr, addr_len);
	//ktblog(LOG_DEBUG, "We got a tcp_v6_connect");
	if (start_conn_state(current->pid, current->tgid, uaddr, 1, addr_len, sock)) {
	}
	return ret;
}

/**
 * Runs a TCP diconnect
 * @param sk A pointer to a sock struct for the connection.
 * @param flags TCP disconnect flags.
 * @return tcp disconnect error code
 */
int new_tcp_disconnect(struct sock *sk, int flags) {
	int ret;
	ret = ref_tcp_disconnect(sk, flags);
	return ret;
}

/**
 * Runs a TCP close, and closes Trustbase conneciton monitoring for that connection.
 * @see interceptor/connection_state.c:stop_conn_state
 * @param sk A pointer to a sock struct for the connection.
 * @param timeout TCP timeout time.
 * @return tcp disconnect error code
 */
void new_tcp_close(struct sock *sk, long timeout) {
	struct socket* sock;
	conn_state_t* conn_state;
	sock = sk->sk_socket;
	if ((conn_state = conn_state_get(current->pid, sock)) != NULL) {
		stop_conn_state(conn_state);
	}
	ref_tcp_close(sk, timeout);
	return;
}

/**
 * Manages TCP sending through the connection handler, according to the connection's handler.
 * @see handshaker-handler/handshake_handler.c:tb_fill_send_buffer 
 * @return the amount of bytes the user wanted to send, or an error code
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
int new_tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size) {
#else
int new_tcp_sendmsg(struct kiocb *iocb, struct sock *sk, struct msghdr *msg, size_t size) {
#endif
	conn_state_t* conn_state;
	struct socket* sock;
	int real_ret;
	struct iovec iov;
	struct msghdr kmsg;
	void* new_data;
	mm_segment_t oldfs;

	sock = sk->sk_socket;

	// Adopt default kernel behavior if we're not monitoring this connection
	if ((conn_state = conn_state_get(current->pid, sock)) == NULL) {
		#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
		return ref_tcp_sendmsg(sk, msg, size);
		#else
		return ref_tcp_sendmsg(iocb, sk, msg, size);
		#endif
	}

	// Copy attributes of existing message into our custom one
	kmsg = *msg;
	iov.iov_len = 0; // will be set later
	iov.iov_base = NULL; // will be set later
	#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
	kmsg.msg_iter.iov = &iov;
	// Pointer to data being sent by user.
	new_data = msg->msg_iter.iov->iov_base;
	BUG_ON(msg->msg_iter.nr_segs > 1);
	#else
	kmsg.msg_iov = &iov;
	// Pointer to data being sent by user.
	new_data = msg->msg_iov->iov_base;
	#endif
	BUG_ON(new_data == NULL);
	oldfs = get_fs();
	set_fs(KERNEL_DS);
	//printk(KERN_ALERT "tes: %c", ((char*)new_data)[0]);

	// XXX Enum this later
	if (ops->get_state(conn_state->state) == 2) {
		set_fs(oldfs);
		#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
		return ref_tcp_sendmsg(sk, msg, size);
		#else
		return ref_tcp_sendmsg(iocb, sk, msg, size);
		#endif
	}

	// 0) If last send attempt was an error, don't copy or update state
	if (conn_state->queued_send_ret > 0) {
		// 1) Copy data from user to our connection state buffer
		if (ops->give_to_handler_send(conn_state->state, new_data, size) != 0) {
			ktblog(LOG_ERROR, "Traffic interceptor failed to copy to send state");
			// XXX delete this connection, we can't handle it
			// Do we try to send existing buffer data?
			// Abort by calling original functionality
			set_fs(oldfs);
			#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
			return ref_tcp_sendmsg(sk, msg, size);
			#else
			return ref_tcp_sendmsg(iocb, sk, msg, size);
			#endif
		}
		// 2) Update handler's state now that it has new data
		if (ops->update_send_state(conn_state->state) != 0) {
			ktblog(LOG_ERROR, "Handler failed to update send state");
			// XXX delete this connection, we can't handle it
			// Do we try to send existing buffer data?
			// Abort by calling original functionality
			set_fs(oldfs);
			#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
			return ref_tcp_sendmsg(sk, msg, size);
			#else
			return ref_tcp_sendmsg(iocb, sk, msg, size);
			#endif
		}
	}
	else {
		// This branch indicates that the last send operation resulted in an
		// error.
		//
		// By skipping the copy and update phases in this branch we
		// effectively assume that the data being sent after an error
		// is the same as the previous time.
		//
		// XXX We could set up something here to verify that the 
		// data sent by the client this time around is the same as last time
		// but I'm not sure we have to.  Only a dumb programmer would alter
		// the contents of his buffer in between send attempts. While I
		// acknowledge the existence of dumb programmers, it seems like they
		// would get what they deserve in this case.
	}
	
	// 3) Have handler tell us what we should forward
	//    This will be the same as last time if an error occurred
	ops->fill_send_buffer(conn_state->state, &iov.iov_base, &iov.iov_len);

	// 4) Forward what handler told us to forward, if anything
	if (iov.iov_len <= 0) { //should never really be negative
		if (ops->num_send_bytes_to_forward(conn_state->state) == 0 && ops->get_state(conn_state->state) == 0) {
			stop_conn_state(conn_state); 
	        }
		// Tell the user we sent everything he wanted
		set_fs(oldfs);
		return size;
	}
	// Use real tcp_sendmsg call to transmit
	// but do it via the persona of the kernel
	#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
	real_ret = ref_tcp_sendmsg(sk, &kmsg, iov.iov_len);
	#else
	real_ret = ref_tcp_sendmsg(iocb, sk, &kmsg, iov.iov_len);
	#endif
	set_fs(oldfs);
	// Record result
	conn_state->queued_send_ret = real_ret;
	if (real_ret > 0) {
		ops->inc_send_bytes_forwarded(conn_state->state, real_ret);
	}
	if (real_ret != iov.iov_len) {
		ktblog(LOG_WARNING, "Traffic interceptor couldn't forward all the bytes desired to destination");
		if (msg->msg_flags & MSG_DONTWAIT) { // nonblocking IO
			// This forces a resend (dont need to delete here because we're
			// still interested in socket, clearly)
			conn_state->queued_send_ret = -EAGAIN;
			return -EAGAIN;
		}
		else { // blocking IO
			// loop here to retry because this might be the last time we're ever called
			while (ops->num_send_bytes_to_forward(conn_state->state) > 0) {
				// Ask handler to update our pointer and length again
				ops->fill_send_buffer(conn_state->state, &iov.iov_base, &iov.iov_len);
				// Attempt send again
				#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
				real_ret = ref_tcp_sendmsg(sk, &kmsg, iov.iov_len);
				#else
				real_ret = ref_tcp_sendmsg(iocb, sk, &kmsg, iov.iov_len);
				#endif
				// Record bytes sent
				ops->inc_send_bytes_forwarded(conn_state->state, real_ret);
			}
		}
	}
	// If handler doesn't care about connection anymore then delete it
	if (ops->num_send_bytes_to_forward(conn_state->state) == 0 && ops->get_state(conn_state->state) == 0) {
		//ktblog(LOG_DEBUG, "No longer interested in socket, ceasing monitoring");
		stop_conn_state(conn_state); 
        }
	// Just tell the user we sent everything he wanted
	// or an error code, if an error occurred
	return real_ret > 0 ? size : real_ret;
}

/**
 * Manages TCP receiving through the connection handler, according to the connection's handler, and data marked to be forwarded.
 * @see handshaker-handler/handshake_handler.c:tb_copy_to_user_buffer
 * @return the amount of bytes the user wanted to send, or an error code
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
int new_tcp_recvmsg(struct sock *sk, struct msghdr *msg, size_t len, int nonblock, int flags, int *addr_len) {
#else
int new_tcp_recvmsg(struct kiocb *iocb, struct sock *sk, struct msghdr *msg, size_t len, int nonblock, int flags, int *addr_len) {
#endif
	int ret;
	mm_segment_t oldfs;
	#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
	struct kvec iov;
	#else
	struct iovec iov;
	#endif
	struct msghdr kmsg = {};
	void* buffer;
	struct socket* sock;
	conn_state_t* conn_state;
	int bytes_to_copy;
	int bytes_sent;
	int b_to_forward;
	int b_to_read;
	void __user* user_buffer;
	sock = sk->sk_socket;

	// Early breakout if we aren't monitoring this connection
	if ((conn_state = conn_state_get(current->pid, sock)) == NULL) {
		#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
		ret = ref_tcp_recvmsg(sk, msg, len, nonblock, flags, addr_len);
		#else
		ret = ref_tcp_recvmsg(iocb, sk, msg, len, nonblock, flags, addr_len);
		#endif
		return ret;
	}

	#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
	user_buffer = (void __user*)msg->msg_iter.iov->iov_base;
	#else
	user_buffer = (void __user *)msg->msg_iov->iov_base;
	#endif

	// XXX Enum this later
	if (ops->get_state(conn_state->state) == 2) {
		#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
		return ref_tcp_recvmsg(sk, msg, len, nonblock, flags, addr_len);
		#else
		return ref_tcp_recvmsg(iocb, sk, msg, len, nonblock, flags, addr_len);
		#endif
	}

	bytes_sent = 0;
	// 1) Place into user's buffer any data already marked for fowarding
	//    up to maxiumum user is requesting (len)
	b_to_forward = ops->num_recv_bytes_to_forward(conn_state->state);
	if (b_to_forward > 0) {
		bytes_to_copy = b_to_forward > len ? len : b_to_forward;
		if (ops->copy_to_user(conn_state->state, user_buffer, bytes_to_copy) != 0) {
			ktblog(LOG_ERROR, "Traffic interceptor copy_to_user() failed");
			// XXX how do we fail here?
		}
		bytes_sent += bytes_to_copy;
		ops->inc_recv_bytes_forwarded(conn_state->state, bytes_sent);
	}

	// 2) If we've already given the user everything he wants, end
	if (bytes_sent == len) {
		return len;
	}

	// If we've not sent anything yet and the socket was closed last time
	// we actually read, then delete state and return
	if (bytes_sent == 0 && conn_state->queued_recv_ret == 0) {
		//ktblog(LOG_DEBUG, "tcp_rcv: reading then deleting");
		stop_conn_state(conn_state);
		return 0;
	}
	if (bytes_sent == 0 && conn_state->queued_recv_ret < 0) {
		//ktblog(LOG_DEBUG, "tcp_rcv: received an error, but gonna pass for a round");
		ret = conn_state->queued_recv_ret;
		conn_state->queued_recv_ret = 1; // pretend no error for next time
		return ret;
	}

	// If we don't care to read any more bytes for this socket, stop now
	if (ops->get_state(conn_state->state) == 0 && ops->bytes_to_read_recv(conn_state->state) == 0) {
		if (bytes_sent > 0) {
			return bytes_sent;
		}
		else {
			stop_conn_state(conn_state);
			#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
			return ref_tcp_recvmsg(sk, msg, len, nonblock, flags, addr_len);
			#else
			return ref_tcp_recvmsg(iocb, sk, msg, len, nonblock, flags, addr_len);
			#endif
		}
	}

	// At this point bytes_to_forward should be zero,
	BUG_ON(ops->num_recv_bytes_to_forward(conn_state->state) != 0);
	// queued_recv_ret should be positive, and bytes_to_read_recv
	// should be positive
	
	
	// 3) Attempt to get more data from external sources
	//ktblog(LOG_DEBUG, "tcp_rcv: going to get more data");
	while (ops->num_recv_bytes_to_forward(conn_state->state) == 0) {
		kmsg = *msg;
		#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 19, 0)
		kmsg.msg_iov = &iov;
		#endif
		#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0)
		kmsg.msg_iter.iov = &iov;
		#endif
		b_to_read = ops->bytes_to_read_recv(conn_state->state);
	        buffer = kmalloc(b_to_read, GFP_KERNEL | __GFP_NOFAIL);
		iov.iov_len = b_to_read;
		iov.iov_base = buffer;

		oldfs = get_fs();
		set_fs(KERNEL_DS);
		#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0)
		iov_iter_init(&kmsg.msg_iter, READ, &iov, 1, iov.iov_len);
		#endif
		#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0)
		ret = ref_tcp_recvmsg(iocb, sk, &kmsg, iov.iov_len, nonblock, flags, addr_len);
		if (ret == -EIOCBQUEUED) {
			ret = wait_on_sync_kiocb(iocb);
		}
		#else
		iov_iter_kvec(&kmsg.msg_iter, READ | ITER_KVEC, &iov, 1, iov.iov_len);
		ret = ref_tcp_recvmsg(sk, &kmsg, iov.iov_len, nonblock, flags, addr_len);
		#endif
		
		// 4) if operation failed then just return what we've sent so far
		//    or the error code
		conn_state->queued_recv_ret = ret;
		if (ret <= 0) {
			//ktblog(LOG_DEBUG, "tcp_rcv: failed on reading");
			if (bytes_sent > 0) {
				// error code is cached for next time
				set_fs(oldfs);
				return bytes_sent; 
			}
			else {
				// Pretend no error for next time since we're
				// sending it now
				conn_state->queued_recv_ret = 1;
				set_fs(oldfs);
				return ret;
			}
		}

		// 5) If operation succeeded then copy to state and update state
		if (ops->give_to_handler_recv(conn_state->state, buffer, ret) != 0) {
			//ktblog(LOG_ERROR, "tcp_rcv: Traffic interceptor failed to copy to recv state");
			// XXX how do we fail here?
		}
		kfree(buffer);
		if (ops->update_recv_state(conn_state->state) != 0) {
			//ktblog(LOG_ERROR, "tcp_rcv: Handler failed to update recv state");
			// XXX how do we fail here?
		}

		set_fs(oldfs);
		// XXX Enum this	
		if (ops->get_state(conn_state->state) == 2) {
			//ktblog(LOG_DEBUG, "tcp_rcv: gonna proxy connection");
			#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
			return ref_tcp_recvmsg(sk, msg, len, nonblock, flags, addr_len);
			#else
			return ref_tcp_recvmsg(iocb, sk, msg, len, nonblock, flags, addr_len);
			#endif
		}

		// 6) If this was a nonblocking call and we still don't have any
		//    additional bytes to forward, break out early
		if (nonblock && ops->num_recv_bytes_to_forward(conn_state->state) == 0) {
			//ktblog(LOG_DEBUG, "tcp_rcv: breakout early");
			return bytes_sent > 0 ? bytes_sent : -EAGAIN;
		}

		// 7) Otherwise if this was a blocking call keep trying until we have
		//    at least something to send back
	}

	// 8) copy to user what we received. return total number bytes sent
	b_to_forward = ops->num_recv_bytes_to_forward(conn_state->state);
	bytes_to_copy = b_to_forward > len - bytes_sent ? len - bytes_sent : b_to_forward;
	if (ops->copy_to_user(conn_state->state, user_buffer + bytes_sent, bytes_to_copy) != 0) {
		ktblog(LOG_ERROR, "tcp_rcv: Traffic interceptor copy_to_user() failed");
		// XXX how do we fail here?
	}
	ops->inc_recv_bytes_forwarded(conn_state->state, bytes_to_copy);
	bytes_sent += bytes_to_copy;
	return bytes_sent;

}

struct sock* new_inet_csk_accept(struct sock *sk, int flags, int *err, bool kern) {
	return ref_inet_csk_accept(sk, flags, err, kern);
}

int set_orig_dst(struct sock *sk, int cmd, void __user *user, unsigned int len) {
	return 0;
}

int get_orig_dst(struct sock *sk, int cmd, void __user *user, int *len) {
	int ret;
	__be16 src_port;
	struct list_head* cur;
	struct list_head* q;
	proxy_accept_list_t* tmp;

	if (cmd != NAT_SOCKOPT_GET) {
		return 0;
	}

	src_port = inet_sk(sk)->inet_dport;
	list_for_each_safe(cur, q, &proxy_accept_list.list) {
		tmp = list_entry(cur, proxy_accept_list_t, list);
		if (tmp->src_port == src_port) {
			ktblog(LOG_INFO, "Identified original destination info for local proxy");
			if (tmp->addr.sa_family == AF_INET) {
				*len = sizeof(struct sockaddr_in);
				ret = copy_to_user(user, &tmp->addr, sizeof(struct sockaddr_in));
			}
			else {
				*len = sizeof(struct sockaddr_in6);
				ret = copy_to_user(user, &tmp->addr, sizeof(struct sockaddr_in6));
			}
			list_del(cur);
			kfree(tmp);
		}
	}
	return 0;
}

