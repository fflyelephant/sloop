#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/sysinfo.h>
#include "dlist.h"
#include "sloop.h"
#include "dtrace.h"

/********************************************************************/
#if DEBUG_SLOOP
#define SLOOPDBG(x)	x
#else
#define SLOOPDBG(x)
#endif

/* internal structure for sloop module */
#define SLOOP_TYPE_MASK		0x00ff
#define SLOOP_TYPE_SOCKET	1
#define SLOOP_TYPE_TIMEOUT	2
#define SLOOP_TYPE_SIGNAL	3
#define SLOOP_INUSED		0x0100

//记录一个待监听(读 or 写)套接字
struct sloop_socket {
	struct dlist_head list;//双链表挂载点(不用时挂在free_socket,使用时挂在readers or writers)
	unsigned int flags;
	int sock;//套接字描述符
	void * param;
	sloop_socket_handler handler;//状态就绪回调函数
};

//记录一个定时器
struct sloop_timeout {
	struct dlist_head list;//双链表挂载点(不用时挂在free_timeout,使用时挂在timeout)
	unsigned int flags;
	struct timeval time;//超时时间
	void * param;
	sloop_timeout_handler handler;//超时回调函数
};

//记录一个信号
struct sloop_signal {
	struct dlist_head list;//双链表挂载点(不用时挂在free_signal,使用时挂在signals)
	unsigned int flags;
	int sig;//信号值
	void * param;
	sloop_signal_handler handler;//信号回调函数
};

struct sloop_data {
	int terminate;//退出标志
	int signal_pipe[2];//信号监听会使用到的管道
	void * sloop_data;
	struct dlist_head free_sockets;
	struct dlist_head free_timeout;
	struct dlist_head free_signals;
	struct dlist_head readers;
	struct dlist_head writers;
	struct dlist_head signals;
	struct dlist_head timeout;
};

//初始化静态存储区给sloop_***结构体
static struct sloop_socket  _sloop_sockets[MAX_SLOOP_SOCKET];
static struct sloop_timeout _sloop_timeout[MAX_SLOOP_TIMEOUT];
static struct sloop_signal  _sloop_signals[MAX_SLOOP_SIGNAL];
static struct sloop_data sloop;

/* initialize list pools */
static void init_list_pools(void)
{
	int i;

	memset(_sloop_sockets, 0, sizeof(_sloop_sockets));
	memset(_sloop_timeout, 0, sizeof(_sloop_timeout));
	memset(_sloop_signals, 0, sizeof(_sloop_signals));
	for (i = 0; i < MAX_SLOOP_SOCKET; i++) dlist_add(&_sloop_sockets[i].list, &sloop.free_sockets);
	for (i = 0; i < MAX_SLOOP_TIMEOUT; i++) dlist_add(&_sloop_timeout[i].list, &sloop.free_timeout);
	for (i = 0; i < MAX_SLOOP_SIGNAL; i++) dlist_add(&_sloop_signals[i].list, &sloop.free_signals);
}

/* get socket from pool */
static struct sloop_socket * get_socket(void)
{
	struct dlist_head * entry;
	struct sloop_socket * target;

	if (dlist_empty(&sloop.free_sockets)) {
		d_error("sloop: no sloop_socket available !!!\n");
		return NULL;
	}
	entry = sloop.free_sockets.next;
	SLOOPDBG(daig_printf("%s: get socket sd=[%x],\n", __FILE__, entry));
	dlist_del(entry);
	target = dlist_entry(entry, struct sloop_socket, list);
	target->flags = SLOOP_INUSED | SLOOP_TYPE_SOCKET;
	return target;
}

/* get timeout from pool */
static struct sloop_timeout * get_timeout(void)
{
	struct dlist_head * entry;
	struct sloop_timeout * target;

	if (dlist_empty(&sloop.free_timeout)) {
		d_error("sloop: no sloop_timeout available !!!\n");
		return NULL;
	}
	entry = sloop.free_timeout.next;
	dlist_del(entry);
	target = dlist_entry(entry, struct sloop_timeout, list);
	target->flags = SLOOP_INUSED | SLOOP_TYPE_TIMEOUT;
	return target;
}

/* get signal from pool */
static struct sloop_signal * get_signal(void)
{
	struct dlist_head * entry;
	struct sloop_signal * target;

	if (dlist_empty(&sloop.free_signals)) {
		d_error("sloop: no sloop_signal available !!!\n");
		return NULL;
	}
	entry = sloop.free_signals.next;
	dlist_del(entry);
	target = dlist_entry(entry, struct sloop_signal, list);
	target->flags = SLOOP_INUSED | SLOOP_TYPE_SIGNAL;
	return target;
}

/* return socket to pool */
static void free_socket(struct sloop_socket * target)
{
	dassert((target->flags & SLOOP_TYPE_MASK) == SLOOP_TYPE_SOCKET);
	target->flags &= (~SLOOP_INUSED);
	dlist_add(&target->list, &sloop.free_sockets);
}

/* return timeout to pool */
static void free_timeout(struct sloop_timeout * target)
{
	dassert((target->flags & SLOOP_TYPE_MASK) == SLOOP_TYPE_TIMEOUT);
	target->flags &= (~SLOOP_INUSED);
	dlist_add(&target->list, &sloop.free_timeout);
}

/* return signal to pool */
static void free_signal(struct sloop_signal * target)
{
	dassert((target->flags & SLOOP_TYPE_SIGNAL) == SLOOP_TYPE_SIGNAL);
	target->flags &= (~SLOOP_INUSED);
	dlist_add(&target->list, &sloop.free_signals);
}

/**********************************************************************/

static struct sloop_socket * register_socket(int sock,
        sloop_socket_handler handler, void * param, struct dlist_head * head)
{
	struct sloop_socket * entry;

	/* allocate a new structure sloop_socket */
	entry = get_socket();
	if (entry == NULL) return NULL;

	/* setup structure and insert into list. */
	entry->sock = sock;
	entry->param = param;
	entry->handler = handler;
	dlist_add(&entry->list, head);
	SLOOPDBG(d_dbg("sloop: new socket : 0x%x (fd=%d)\n", (unsigned int)entry, entry->sock));
	return entry;
}

static void cancel_socket(struct sloop_socket * target, struct dlist_head * head)
{
	struct dlist_head * entry;

	if (target) {
		dlist_del(&target->list);
		SLOOPDBG(d_dbg("sloop: free socket : 0x%x\n", (unsigned int)target));
		free_socket(target);
	} else {
		while (!dlist_empty(head)) {
			entry = head->next;
			dlist_del(entry);
			target = dlist_entry(entry, struct sloop_socket, list);
			SLOOPDBG(d_dbg("sloop: free socket : 0x%x\n", (unsigned int)target));
			free_socket(target);
		}
	}
}

/* signal handler */
static void sloop_signals_handler(int sig)
{
	d_info("sloop: sloop_signals_handler(%d)\n", sig);
	if (write(sloop.signal_pipe[1], &sig, sizeof(sig)) < 0) {
		d_error("sloop: sloop_signals_handler(): Cound not send signal: %s\n", strerror(errno));
	}
}

/***************************************************************************/
/* sloop APIs */

/* sloop module initialization */
void sloop_init(void * sloop_data)
{
	memset(&sloop, 0, sizeof(sloop));
	INIT_DLIST_HEAD(&sloop.readers);
	INIT_DLIST_HEAD(&sloop.writers);
	INIT_DLIST_HEAD(&sloop.signals);
	INIT_DLIST_HEAD(&sloop.timeout);
	INIT_DLIST_HEAD(&sloop.free_sockets);
	INIT_DLIST_HEAD(&sloop.free_timeout);
	INIT_DLIST_HEAD(&sloop.free_signals);
	init_list_pools();
	pipe(sloop.signal_pipe);
	sloop.sloop_data = sloop_data;
}

/* register a read socket */
sloop_handle sloop_register_read_sock(int sock, sloop_socket_handler handler, void * param)
{
	return register_socket(sock, handler, param, &sloop.readers);
}

/* register a write socket */
sloop_handle sloop_register_write_sock(int sock, sloop_socket_handler handler, void * param)
{
	return register_socket(sock, handler, param, &sloop.writers);
}

/* cancel a read socket */
void sloop_cancel_read_sock(sloop_handle handle)
{
	cancel_socket((struct sloop_socket *)handle, &sloop.readers);
}

/* cancel a write socket */
void sloop_cancel_write_sock(sloop_handle handle)
{
	cancel_socket((struct sloop_socket *)handle, &sloop.writers);
}

/* register a signal handler */
sloop_handle sloop_register_signal(int sig, sloop_signal_handler handler, void * param)
{
	struct sloop_signal * entry;
	struct sigaction sa;

	sa.sa_handler = sloop_signals_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;

	/* allocate a new structure sloop_signal */
	entry = get_signal();
	if (entry == NULL)
		return NULL;

	/* setup structure and insert into list. */
	entry->sig = sig;
	entry->param = param;
	entry->handler = handler;
	dlist_add(&entry->list, &sloop.signals);
	SLOOPDBG(d_dbg("sloop: sloop_register_signal(%d)\n", sig));
	if (sigaction(sig, &sa, NULL) < 0) {
		dlist_del(&entry->list);
		free_signal(entry);
		d_error("sigaction %d error %s\n", sig, strerror(errno));
		return NULL;
	}

	return entry;
}

/* cancel a signal handler */
void sloop_cancel_signal(sloop_handle handle)
{
	struct sloop_signal * entry = (struct sloop_signal *)handle;
	struct dlist_head * list;

	if (handle) {
		SLOOPDBG(d_dbg("sloop: sloop_cancel_signal(%d)\n", entry->sig));
		signal(entry->sig, SIG_DFL);
		dlist_del(&entry->list);
		free_signal(entry);
	} else {
		while (!dlist_empty(&sloop.signals)) {
			list = sloop.signals.next;
			entry = dlist_entry(list, struct sloop_signal, list);
			SLOOPDBG(d_dbg("sloop: sloop_cancel_signal(%d)\n", entry->sig));
			signal(entry->sig, SIG_DFL);
			dlist_del(list);
			free_signal(entry);
		}
	}
}
#endif

/* register a timer  */
sloop_handle sloop_register_timeout(unsigned int secs, unsigned int usecs, sloop_timeout_handler handler, void * param)
{
	struct sloop_timeout * timeout, * tmp;
	struct dlist_head * entry;

	/* allocate a new struct sloop_timeout. */
	timeout = get_timeout();
	if (timeout == NULL) return NULL;

	/* get current time */
	gettimeofday(&timeout->time, NULL);
	timeout->time.tv_sec += secs;
	timeout->time.tv_usec += usecs;

	while (timeout->time.tv_usec >= 1000000) {
		timeout->time.tv_sec++;
		timeout->time.tv_usec -= 1000000;
	}
	timeout->handler = handler;
	timeout->param = param;
	INIT_DLIST_HEAD(&timeout->list);

	/* put into the list */
	if (dlist_empty(&sloop.timeout)) {
		dlist_add(&timeout->list, &sloop.timeout);
		SLOOPDBG(d_dbg("sloop: timeout(0x%x) added !\n", timeout));
		return timeout;
	}

	entry = sloop.timeout.next;
	while (entry != &sloop.timeout) {
		tmp = dlist_entry(entry, struct sloop_timeout, list);
		if (timercmp(&timeout->time, &tmp->time, < )) break;
		entry = entry->next;
	}
	dlist_add_tail(&timeout->list, entry);
	SLOOPDBG(d_dbg("sloop: timeout(0x%x) added !!\n", timeout));
	return timeout;
}

/* cancel the timer */
void sloop_cancel_timeout(sloop_handle handle)
{
	struct sloop_timeout * entry = (struct sloop_timeout *)handle;
	struct dlist_head * list;

	if (handle) {
		dlist_del(&(entry->list));
		SLOOPDBG(d_dbg("sloop: sloop_cancel_timeout(0x%x)\n", handle));
		free_timeout(entry);
	} else {
		while (!dlist_empty(&sloop.timeout)) {
			list = sloop.timeout.next;
			dlist_del(list);
			entry = dlist_entry(list, struct sloop_timeout, list);
			SLOOPDBG(d_dbg("sloop: sloop_cancel_timeout(0x%x)\n", handle));
			free_timeout(entry);
		}
	}
}

void sloop_run(void)
{
	fd_set rfds;
	fd_set wfds;
	struct timeval tv, now;
	struct sloop_timeout * entry_timeout = NULL;
	struct sloop_socket * entry_socket;
	struct sloop_signal * entry_signal;
	struct dlist_head * entry;
	int max_sock;
	int res;
	int sig;
	// 开始循环
	while (!sloop.terminate) {
		/* 是否有定时器加入 */
		if (!dlist_empty(&sloop.timeout)) {
			entry = sloop.timeout.next;
			entry_timeout = dlist_entry(entry, struct sloop_timeout, list);
		} else {
			entry_timeout = NULL;
		}
		/* 有定时器 */
		if (entry_timeout) {
			/* 获取当前时间 */
			gettimeofday(&now, NULL);
			/* 当前时间>=定时器表示应该执行定时器的回调函数了 */
			if (timercmp(&now, &entry_timeout->time, >= ))
				tv.tv_sec = tv.tv_usec = 0;/* tv是select函数的timeout，直接置0表示不阻塞 */
			else
				timersub(&entry_timeout->time, &now, &tv);/* 否则阻塞 '当前时间-到期时间' */
		}

		/* 清空读写描述符集合 */
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		max_sock = 0;

		/* 添加信号可读转状态 */
		FD_SET(sloop.signal_pipe[0], &rfds);
		if (max_sock < sloop.signal_pipe[0]) max_sock = sloop.signal_pipe[0];

		/* 添加套接字可读转状态 */
		for (entry = sloop.readers.next; entry != &sloop.readers; entry = entry->next) {
			entry_socket = dlist_entry(entry, struct sloop_socket, list);
			FD_SET(entry_socket->sock, &rfds);
			if (max_sock < entry_socket->sock) max_sock = entry_socket->sock;
		}
		/* 添加套接字可写转状态 */
		for (entry = sloop.writers.next; entry != &sloop.writers; entry = entry->next) {
			entry_socket = dlist_entry(entry, struct sloop_socket, list);
			FD_SET(entry_socket->sock, &wfds);
			if (max_sock < entry_socket->sock) max_sock = entry_socket->sock;
		}

		d_dbg("sloop: >>> enter select sloop !!\n");
		res = select(max_sock + 1, &rfds, &wfds, NULL, entry_timeout ? &tv : NULL);

		if (res < 0) {
			/* 意外被中断 */
			if (errno == EINTR) {
				d_info("sloop: sloop_run(): EINTR!\n");
				continue;
			} else {
				d_error("sloop: sloop_run(): select error (%s)!\n", strerror(errno));
				break;
			}
		}

		/* 先检查信号 */
		if (res > 0 && FD_ISSET(sloop.signal_pipe[0], &rfds)) {
			if (read(sloop.signal_pipe[0], &sig, sizeof(sig)) < 0) {
				/* probabaly just EINTR */
				d_error("sloop: sloop_run(): Could not read signal: %s\n", strerror(errno));
			} else if (sig == 0) {
				d_info("sloop: get myself signal !!\n");
			} else if (!dlist_empty(&sloop.signals)) {
				for (entry = sloop.signals.next; entry != &sloop.signals; entry = entry->next) {
					entry_signal = dlist_entry(entry, struct sloop_signal, list);
					/* 通过信号值找到登记的信号结构体并执行回调函数 */
					if (entry_signal->sig == sig) {
						if (entry_signal->handler(entry_signal->sig, entry_signal->param, sloop.sloop_data) < 0) {
							dlist_del(entry);
							free_signal(entry_signal);
						}
						break;
					}
				}
				if (sloop.terminate) break;
			} else {
				SLOOPDBG(d_info("sloop: should not be here !!\n"));
			}
		}

		/* 检查定时器 */
		if (entry_timeout) {
			if (sloop.timeout.next == &entry_timeout->list) {
				gettimeofday(&now, NULL);
				if (res == 0 || timercmp(&now, &entry_timeout->time, >= )) {
					/* 当前时间>=到期时间就调用回调函数 */
					if (entry_timeout->handler)
						entry_timeout->handler(entry_timeout->param, sloop.sloop_data);
					dlist_del(&entry_timeout->list);//删除了定时器
					free_timeout(entry_timeout);//将此定时器又归还给free_timeout双链表
				}
			} else {
				SLOOPDBG(d_info("sloop: timeout (0x%x) is gone, should be canceled !!!\n", entry_timeout));
			}
		}

		/* 检查可读状态 */
		if (!dlist_empty(&sloop.readers)) {
			entry = sloop.readers.next;
			while (entry != &sloop.readers) {
				/* dlist_entry函数通过list指针获得指向list所在结构体的指针 */
				entry_socket = dlist_entry(entry, struct sloop_socket, list);
				if (FD_ISSET(entry_socket->sock, &rfds))/* 读状态就绪执行回调函数 */
					res = entry_socket->handler(entry_socket->sock, entry_socket->param, sloop.sloop_data);
				else
					res = 0;
				entry = entry->next;

				/* 不同于定时器，只有回调函数返回错误才将此结构归还给free_readers，否则一直会监听此描述符 */
				if (res < 0) {
					dlist_del(&entry_socket->list);
					free_socket(entry_socket);
				}
			}
		}

		/* 检查可写状态 */
		if (!dlist_empty(&sloop.writers)) {
			entry = sloop.writers.next;
			while (entry != &sloop.writers) {
				entry_socket = dlist_entry(entry, struct sloop_socket, list);
				if (FD_ISSET(entry_socket->sock, &wfds))
					res = entry_socket->handler(entry_socket->sock, entry_socket->param, sloop.sloop_data);
				else
					res = 0;
				entry = entry->next;

				if (res < 0) {
					dlist_del(&entry_socket->list);
					free_socket(entry_socket);
				}
			}
		}
	}
	/* 在退出循环时要将所有的都归还给free_***结构体 */
	sloop_cancel_signal(NULL);
	sloop_cancel_timeout(NULL);
	sloop_cancel_read_sock(NULL);
	sloop_cancel_write_sock(NULL);
}

void sloop_terminate(void)
{
	sloop.terminate = 1;
}

static void sloop_dump_socket(struct dlist_head * head)
{
	struct dlist_head * entry;
	struct sloop_socket * socket;

	entry = head->next;
	while (entry != head) {
		socket = dlist_entry(entry, struct sloop_socket, list);
		printf("socket(0x%p), fd(%d), param(0x%p), handler(0x%p)\n",
		       socket, socket->sock, socket->param,
		       socket->handler);
		entry = entry->next;
	}
}
void sloop_dump_readers(void)
{
	printf("=================================\n");
	printf("sloop readers\n");
	sloop_dump_socket(&sloop.readers);
	printf("---------------------------------\n");
}
void sloop_dump_writers(void)
{
	printf("=================================\n");
	printf("sloop writers\n");
	sloop_dump_socket(&sloop.writers);
	printf("---------------------------------\n");
}
void sloop_dump_timeout(void)
{
	struct dlist_head * entry;
	struct sloop_timeout * timeout;

	printf("=================================\n");
	printf("sloop timeout\n");
	entry = sloop.timeout.next;
	while (entry != &sloop.timeout) {
		timeout = dlist_entry(entry, struct sloop_timeout, list);
		printf("timeout(0x%p), time(%d:%d), param(0x%p), handler(0x%p)\n",
		       timeout, (int)timeout->time.tv_sec, (int)timeout->time.tv_usec,
		       timeout->param, timeout->handler);
		entry = entry->next;
	}
	printf("---------------------------------\n");
}

void sloop_dump_signals(void)
{
	struct dlist_head * entry;
	struct sloop_signal * signal;

	printf("=================================\n");
	printf("sloop signals\n");
	entry = sloop.signals.next;
	while (entry != &sloop.signals) {
		signal = dlist_entry(entry, struct sloop_signal, list);
		printf("signals(0x%p), sig(%d), param(0x%p), handler(0x%p)\n",
		       signal, signal->sig, signal->param,
		       signal->handler);
		entry = entry->next;
	}
	printf("---------------------------------\n");
}

void sloop_dump(void)
{
	sloop_dump_readers();
	sloop_dump_writers();
	sloop_dump_timeout();
	sloop_dump_signals();
}

#if 0
static int SIGINT_handler(int sig, void * param, void * sloop_data)
{
	printf("SIGINT_handler(%d, 0x%x, 0x%x)\n", sig, (unsigned int)param, (unsigned int)sloop_data);
	sloop_terminate();
	return 0;
}

static int SIGTERM_handler(int sig, void * param, void * sloop_data)
{
	printf("SIGTERM_handler(%d, 0x%x, 0x%x)\n", sig, (unsigned int)param, (unsigned int)sloop_data);
	sloop_terminate();
	return 0;
}

static int SIGUSR1_handler(int sig, void * param, void * sloop_data)
{
	printf("SIGUSR1_handler(%d, 0x%x, 0x%x)\n", sig, (unsigned int)param, (unsigned int)sloop_data);
	sloop_dump();
	return 0;
}

static sloop_handle h_timeout1 = NULL;
static sloop_handle h_timeout2 = NULL;
static sloop_handle h_timeout3 = NULL;
static sloop_handle h_timeout4 = NULL;

static void timer1_handler(void * param, void * sloop_data)
{
	sloop_handle * handle = (sloop_handle *)param;
	printf("timer1_handler(0x%x, 0x%x)\n", (unsigned int)param, (unsigned int)sloop_data);
	*handle = NULL;
}

static int SIGUSR2_handler(int sig, void * param, void * sloop_data)
{
	printf("SIGUSR2_handler(%d, 0x%x, 0x%x)\n", sig, (unsigned int)param, (unsigned int)sloop_data);
	printf(" handle1 = 0x%x\n", (unsigned int)h_timeout1);
	printf(" handle2 = 0x%x\n", (unsigned int)h_timeout2);
	printf(" handle3 = 0x%x\n", (unsigned int)h_timeout3);
	printf(" handle4 = 0x%x\n", (unsigned int)h_timeout4);
	if (h_timeout1) {
		sloop_cancel_timeout(h_timeout1);
		h_timeout1 = NULL;
	} else if (h_timeout2) {
		sloop_cancel_timeout(h_timeout2);
		h_timeout2 = NULL;
	} else if (h_timeout3) {
		sloop_cancel_timeout(h_timeout3);
		h_timeout3 = NULL;
	} else if (h_timeout4) {
		sloop_cancel_timeout(h_timeout4);
		h_timeout4 = NULL;
	} else {
		h_timeout1 = sloop_register_timeout(50, 0, timer1_handler, &h_timeout1);
		h_timeout2 = sloop_register_timeout(10, 0, timer1_handler, &h_timeout2);
		h_timeout3 = sloop_register_timeout(30, 0, timer1_handler, &h_timeout3);
		h_timeout4 = sloop_register_timeout(40, 0, timer1_handler, &h_timeout4);
	}
	printf("SIGUSR2_handler !!!\n");
	return 0;
}

int main(int argc, char * argv[])
{
	printf("SIGINT =%d\n", SIGINT);
	printf("SIGTERM=%d\n", SIGTERM);
	printf("SIGUSR1=%d\n", SIGUSR1);
	printf("SIGUSR2=%d\n", SIGUSR2);

	sloop_init(NULL);

	sloop_register_signal(SIGINT,  SIGINT_handler,  NULL);
	sloop_register_signal(SIGTERM, SIGTERM_handler, NULL);
	sloop_register_signal(SIGUSR1, SIGUSR1_handler, NULL);
	sloop_register_signal(SIGUSR2, SIGUSR2_handler, NULL);

	sloop_run();


	return 0;
}
#endif

void sloop_main(unsigned long data)
{
	//diag_printf("sloop_main start\n");
	sloop_init(NULL);
	sloop_run();
}