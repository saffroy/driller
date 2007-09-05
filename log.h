#ifndef LOG_H
#define LOG_H

/*
 * traces
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#define err_noabort(fmt,arg...) \
	do {								\
		fprintf(stderr, "error(%s:%d:%s): " fmt "\n",		\
			__FILE__, __LINE__, __FUNCTION__, ##arg);	\
	} while(0)

#define err(fmt,arg...) \
	do {					\
		err_noabort(fmt, ##arg);	\
		abort();			\
	} while(0)

#define perr(str) err(str ":%s\n", strerror(errno))

#define warn(fmt,arg...) \
	fprintf(stderr, "warning(%s:%d:%s): " fmt "\n",		\
		__FILE__, __LINE__, __FUNCTION__, ##arg)

#ifdef DEBUG
#define dbg(fmt,arg...) \
	fprintf(stderr, "debug(%s:%d:%s): " fmt "\n",		\
		__FILE__, __LINE__, __FUNCTION__, ##arg)
#else
#define dbg(fmt,arg...) do { } while(0)
#endif

/*
 * utilities
 */

#define min(x,y) (x < y ? x : y)
#define max(x,y) (x > y ? x : y)

#endif /* LOG_H */
