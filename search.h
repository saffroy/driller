/* Declarations for System V style searching functions.
   Copyright (C) 1995-1999, 2000 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA.  */

#ifndef _MY_SEARCH_H
#define	_MY_SEARCH_H 1

#define __USE_GNU
#define __set_errno(_v) (errno = _v)
#define __THROW

/* Action which shall be performed in the call the hsearch.  */
#if 0
typedef enum
  {
    FIND,
    ENTER
  }
ACTION;

typedef struct entry
  {
    char *key;
    void *data;
  }
ENTRY;
#endif

/* Opaque type for internal use.  */
struct _ENTRY;

#ifdef __USE_GNU
/* Data type for reentrant functions.  */
struct hsearch_data
  {
    struct _ENTRY *table;
    unsigned int size;
    unsigned int filled;
  };

/* Reentrant versions which can handle multiple hashing tables at the
   same time.  */
extern int hsearch_r (ENTRY __item, ACTION __action, ENTRY **__retval,
		      struct hsearch_data *__htab) __THROW;
extern int hcreate_r (size_t __nel, struct hsearch_data *__htab) __THROW;
extern void hdestroy_r (struct hsearch_data *__htab) __THROW;
#endif


#endif /* search.h */
