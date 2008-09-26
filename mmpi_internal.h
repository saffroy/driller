/*
 * mmpi_internal.h
 *
 * Copyright 2007 Jean-Marc Saffroy <saffroy@gmail.com>
 * This file is part of the Driller library.
 * Driller is free software, distributed under the terms of the
 * GNU Lesser General Public License version 2.1.
 *
 */

#ifndef MMPI_INTERNAL_H
#define MMPI_INTERNAL_H

/*
 * lists for shared mem
 */

#define LIST_MAGIC 0xf001157
struct list_head {
#ifndef NDEBUG
	int magic;
#endif
	intptr_t off_next, off_prev;
};

/*
 * shared mem and messages
 */

#define SHMEM_KEY_MAGIC 0xf003333

#define __cacheline_aligned __attribute__((__aligned__(CACHELINE_ALIGN)))

enum msg_type {
	MSG_FREE          = -1,
	MSG_DATA          = 0,
	MSG_FRAG          = 1,
	MSG_DRILLER       = 2,
	MSG_DRILLER_INVAL = 3,
};

struct driller_payload {
	struct map_rec map;
	struct fdkey key;
	off_t offset;
	size_t length;
};

struct message {
	struct list_head m_list;
	enum msg_type m_type;
	int m_size;
	int m_src;
	union {
		char m_payload[MSG_PAYLOAD_SIZE_BYTES];
		struct driller_payload m_drill;
	};
};

struct message_queue {
	struct spinlock q_lock __cacheline_aligned;
	struct list_head q_list;
	int q_length;
};

struct shmem {
	volatile int barrier_box __cacheline_aligned;
	volatile int driller_send_running;
	struct message_queue free_q;
	struct message_queue recv_q;
	struct message msg_pool[MSG_POOL_SIZE];
};

/*
 * interaction with driller
 */

struct driller_udata {
	struct fdkey key;
	char references[];
};

#endif /* MMPI_INTERNAL_H */
