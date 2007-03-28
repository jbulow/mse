/**
 * @file muse_builtin_memport.c
 * @author Srikumar K. S. (mailto:kumar@muvee.com)
 *
 * Copyright (c) 2006 Jointly owned by Srikumar K. S. and muvee Technologies Pte. Ltd. 
 *
 * All rights reserved. See LICENSE.txt distributed with this source code
 * or http://muvee-symbolic-expressions.googlecode.com/svn/trunk/LICENSE.txt
 * for terms and conditions under which this software is provided to you.
 *
 * Implements file ports - a wrapper for I/O using FILE*.
 */

#include "muse_port.h"
#include <stdlib.h>
#include <memory.h>

/** @addtogroup Ports */
/*@{*/

typedef struct _memchunk_t
{
	struct _memchunk_t *next;
	size_t size;
	unsigned char data[1];
} memchunk_t;

typedef struct
{
	muse_port_base_t base;
	memchunk_t *first, *last;
	size_t read_offset;
} memport_t;

typedef struct
{
	muse_port_type_t port;
} memport_type_t;

static void memport_init( muse_env *env, void *ptr, muse_cell args )
{
	memport_t *p = (memport_t*)ptr;

	p->base.mode |= MUSE_PORT_READ_WRITE;

	port_init( env, (muse_port_base_t*)p );
	
	p->first = p->last = NULL;
	p->read_offset = 0;
}

static void memport_destroy( muse_env *env, void *ptr )
{
	memport_t *p = (memport_t*)ptr;
	memport_type_t *t = (memport_type_t*)p->base.base.type_info;

	if ( t->port.close )
	{
		t->port.close(p);
	}

	port_destroy( (muse_port_base_t*)p );
}

static void memport_close( void *ptr )
{
	memport_t *p = (memport_t*)ptr;
	memchunk_t *ch = NULL;

	while ( p->first )
	{
		ch = p->first;
		p->first = ch->next;
		free(ch);		
	}

	p->first = p->last = NULL;
	p->read_offset = 0;
}

static size_t memport_read( void *buffer, size_t nbytes, void *port )
{
	memport_t *p = (memport_t*)port;
	unsigned char *b = (unsigned char *)buffer;

	/* Read the requested number of bytes into the given target buffer. */
	size_t bytes_read = 0;
	while ( p->first && bytes_read < nbytes )
	{
		size_t n = p->first->size - p->read_offset;

		if ( bytes_read + n > nbytes )
			n = nbytes - bytes_read;

		if ( n > 0 )
		{
			memcpy( b, p->first->data + p->read_offset, n );
			bytes_read += n;
			p->read_offset += n;
			b += n;
		}

		if ( p->read_offset >= p->first->size )
		{
			memchunk_t *ch = p->first;
			p->first = ch->next;
			free(ch);
			if ( p->first == NULL )
				p->last = NULL;
			p->read_offset = 0;
		}
	}

	return bytes_read;
}

static size_t memport_write(void *buffer, size_t nbytes, void *port )
{
	if ( nbytes > 0 ) 
	{
		memport_t *p = (memport_t*)port;
		
		memchunk_t *ch = (memchunk_t*)malloc( sizeof(memchunk_t) + nbytes );
		ch->next = NULL;
		ch->size = nbytes;
		memcpy( ch->data, buffer, nbytes );

		if ( !(p->last) ) 
			p->first = p->last = ch;
		else
		{
			p->last->next = ch;
			p->last = ch;
		}

		return nbytes;
	}

	return 0;
}

static int memport_flush( void *port )
{
	return 0;
}

/**
 * Dumps the data in the current memport to the given port.
 */
static void memport_dump( muse_env *env, void *obj, void *port )
{
	memport_t *mp = (memport_t*)obj;
	muse_port_base_t *p = (muse_port_base_t*)port;

	memchunk_t *ch = mp->first;
	while ( ch )
	{
		port_write( ch->data, ch->size, p );
		ch = ch->next;
	}
}

static memport_type_t g_memport_type =
{
	{
		{
			'muSE',
			'port',
			sizeof(memport_t),
			NULL,
			NULL,
			memport_init,
			NULL,
			memport_destroy,
			memport_dump
		},

		memport_close,
		memport_read,
		memport_write,
		memport_flush
	}
};


/**
 * (memport)
 *
 * Creates a new memory port for processing streams of data
 * in memory. You can write data to a memory port using the
 * standard \ref fn_print "print" and \ref fn_write "write" 
 * functions. All the data you write to a memport are kept
 * in memory and can be subsequently read back using
 * \ref fn_read "read" on the memport.
 *
 * @code
 * > (define p (memport))
 * > (print p "(+ 1 2)")
 * T
 * > (eval (read p))
 * 3
 * @endcode
 */
static muse_cell fn_memport( muse_env *env, void *context, muse_cell args )
{
	return _mk_functional_object( (muse_functional_object_type_t*)&g_memport_type, args );
}

void muse_define_builtin_memport(muse_env *env)
{	
	/* Define the "open-file" function. This is the only file specific function needed.
	After this the generic port functions take over. */
	_define( _csymbol(L"memport"), _mk_nativefn( fn_memport, NULL ) );
}
