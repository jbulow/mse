/**
 * @file muse.c
 * @author Srikumar K. S. (mailto:kumar@muvee.com)
 *
 * Copyright (c) 2006 Jointly owned by Srikumar K. S. and muvee Technologies Pte. Ltd. 
 *
 * All rights reserved. See LICENSE.txt distributed with this source code
 * or http://muvee-symbolic-expressions.googlecode.com/svn/trunk/LICENSE.txt
 * for terms and conditions under which this software is provided to you.
 */

#include "muse_opcodes.h"
#include "muse_builtins.h"
#include "muse_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#ifdef MUSE_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <sys/time.h>
#endif

/**
 * The muse environment contains the heap, the various stacks and symbols, and 
 * basically everything that happens in the interpreter is w.r.t. the environment.
 * There is only one global muse environment that you can change using the
 * muse_get_current_env() and muse_set_current_env() API calls. Having the muse
 * environment global simplifies the API calls. 
 * 
 * As of this implementation, the limitation is that only one muse environment
 * can be the current environment in a single process. It is possible to improve
 * this situation to allow multiple environments to be active in different
 * threads by changing the global to a thread-local variable. Then the
 * conbstraint would be that a single muse environment can be the current
 * environment in only one thread at any given time.
 */
muse_env *g_muse_env = NULL;

/**
 * String names for the various cell types, intended for
 * debugging and reporting use.
 * 
 * @see _typename()
 */
const char *g_muse_typenames[] =
{
	"MUSE_CONS_CELL",
	"MUSE_LAMBDA_CELL",
	"MUSE_SYMBOL_CELL",
	"MUSE_NATIVEFN_CELL",
	"MUSE_INT_CELL",
	"MUSE_FLOAT_CELL",
	"MUSE_TEXT_CELL"
};

static void init_stack( muse_stack *s, int size )
{
	s->size = size;
	s->top = s->bottom = (muse_cell*)calloc( size, sizeof(muse_cell) );
}

static void destroy_stack( muse_stack *s )
{
	if ( s->bottom )
	{
		free( s->bottom );
		s->bottom = s->top = NULL;
		s->size = 0;
	}
}

static muse_boolean realloc_stack( muse_stack *s, int new_size )
{
	if ( new_size != s->size )
	{
		muse_cell *p = (muse_cell*)realloc( s->bottom, new_size * sizeof(muse_cell) );
		if ( p )
		{
			s->top = p + (s->top - s->bottom);
			s->bottom = p;
			s->size = new_size;
			return MUSE_TRUE;
		}
		else
			return MUSE_FALSE;
	}
	else	
		return MUSE_TRUE;
}

/**	
 * Makes sure that there is space enough on the stack
 * for "items" cells to be placed. If not, grows
 * the stack as much as necessary. 
 */
static muse_boolean ensure_stack( muse_stack *s, int items )
{
	if ( s->top - s->bottom + items >= s->size )
	{
		/* Need to realloc */
		int new_size = s->size + items;
		if ( new_size < s->size * 2 )
			new_size = s->size * 2;
		
		return realloc_stack( s, new_size );
	}
	else
		return MUSE_FALSE;
}

static void init_heap( muse_heap *heap, int heap_size )
{
	heap_size				= (heap_size + 7) & ~7;
	heap->size_cells		= heap_size;
	heap->cells				= (muse_cell_data*)calloc( heap_size, sizeof(muse_cell_data) );
	heap->free_cells		= _cellati(1); /* 0 is not in free list as its a fixed cell. */
	heap->free_cell_count	= heap_size - 1;
	heap->marks				= (unsigned char *)calloc( heap_size >> 3, 1 );

	/* Initialize free list */
	{
		int i, i_end;
		muse_cell_data *c = _ptr(_cellati(1));
		for ( i = 1, i_end = heap_size-1; i < i_end; ++i, ++c )
		{
			c->cons.head = MUSE_NIL;
			c->cons.tail = _cellati(i+1);
		}
	}
}

static void destroy_heap( muse_heap *heap )
{
	if ( heap->cells )
	{
		free(heap->cells);
		heap->cells = NULL;
		heap->size_cells = 0;
		free(heap->marks);
		heap->free_cells = 0;
		heap->free_cell_count = 0;
	}
}

static muse_boolean grow_heap( muse_heap *heap, int new_size )
{
	new_size = (new_size + 7) & ~7;
	
	fprintf(stderr, "\n(growing heap to %d)\n", new_size);
	if ( new_size <= heap->size_cells )
		return MUSE_TRUE;

	{
		muse_cell_data *p = (muse_cell_data*)realloc( heap->cells, new_size * sizeof(muse_cell_data) );
		if ( p )
		{
			unsigned char *m = (unsigned char *)realloc( heap->marks, new_size >> 3 );
			if ( m )
			{
				heap->cells = p;
				heap->marks = m;

				/* Collect the newly allocated cells into the free list. */				
				{
					int i = heap->size_cells;
					int i_end = new_size - 1;
					muse_cell_data *c = p + i;
					for ( ; i < i_end; ++i, ++c )
					{
						c->cons.head = MUSE_NIL;
						c->cons.tail = _cellati(i+1);
					}
					heap->cells[i_end].cons.head = MUSE_NIL;
					heap->cells[i_end].cons.tail = heap->free_cells;
					heap->free_cells = _cellati( heap->size_cells );
					heap->free_cell_count += (new_size - heap->size_cells);
					heap->size_cells = new_size;
				}
				
				return MUSE_TRUE;
			}
			else
			{
				heap->cells = (muse_cell_data*)realloc( p, heap->size_cells * sizeof(muse_cell_data) );;
				return MUSE_FALSE;
			}
		}
		else
			return MUSE_FALSE;
	}
}

static const struct _bs { int builtin; const muse_char *symbol; } k_builtin_symbol_table[] =
{
	{ MUSE_NIL,					NULL		},
	{ MUSE_T,					L"T"		},
	{ MUSE_QUOTE,				L"quote"	},
	{ MUSE_RETURN,				L"return"	},
	{ MUSE_BREAK,				L"break"	},
	{ MUSE_CLASS,				L"class"	},
	{ MUSE_SUPER,				L"super"	},
	{ MUSE_DOC,					L"doc"		},
	{ MUSE_CODE,				L"code"		},
	{ MUSE_SIGNATURE,			L"signature"},
	{ MUSE_USAGE,				L"usage"	},
	{ MUSE_BRIEF,				L"brief"	},
	{ MUSE_DESCR,				L"descr"	},
	{ MUSE_TIMEOUT,				L"timeout"	},
	{ MUSE_DEFINE,				L"define"	},
	{ -1,						NULL		},
};

static void init_builtin_symbols( muse_cell *s )
{
	const struct _bs *bs = k_builtin_symbol_table;
	while ( bs->builtin >= 0 )
	{
		if ( bs->symbol == NULL )
			s[bs->builtin]	= bs->builtin;
		else
			s[bs->builtin] = muse_csymbol( bs->symbol );
		
		++bs;
	}
}

static void init_parameters( muse_env *env, const int *parameters )
{
	static int k_default_parameter_values[MUSE_NUM_PARAMETER_NAMES] =
	{
		0,		/* Ignored */
		65536,	/* MUSE_HEAP_SIZE */
		80,		/* MUSE_GROW_HEAP_THRESHOLD */
		4096,	/* MUSE_STACK_SIZE */
		4096,	/* MUSE_MAX_SYMBOLS */
		0,		/* MUSE_DISCARD_DOC */
		1,		/* MUSE_PRETTY_PRINT */
		4,		/* MUSE_TAB_SIZE */
		10		/* MUSE_DEFAULT_ATTENTION */
	};

	/* Initialize default values. */
	env->parameters = (int*)calloc( MUSE_NUM_PARAMETER_NAMES, sizeof(int) );
	memcpy( env->parameters, k_default_parameter_values, sizeof(k_default_parameter_values) );
	
	/* Set overridden parameters. */
	if ( parameters )
	{
		while ( parameters[0] )
		{
			if ( parameters[0] > 0 && parameters[0] < MUSE_NUM_PARAMETER_NAMES )
			{
				env->parameters[parameters[0]] = parameters[1];
				parameters += 2;
			}
			else
			{
				fprintf( stderr, "muse: Invalid muse_init_env parameter %d!\n", *parameters );
				break;
			}
		}
	}
}

/**
 * Creates a new muse environment.
 * The new environment is made current.
 *
 * @param parameters is an int array of param-value pairs. The last entry 
 * should be MUSE_END_OF_LIST which need not be given a value.
 *
 * @see muse_env_parameter_name_t
 */
muse_env *muse_init_env( const int *parameters )
{
	muse_env *env = (muse_env*)calloc( 1, sizeof(muse_env) );
	muse_set_current_env(env);
	
	env->stack_base = (void*)&parameters;
	init_parameters( env, parameters );
	
	init_heap( &env->heap, env->parameters[MUSE_HEAP_SIZE] );
	init_stack( &env->symbol_stack, env->parameters[MUSE_MAX_SYMBOLS] );
	
	/* The symbol stack is not really a stack. Its an array of buckets 
	containing lists of symbols and is of fixed size. We use a hashing
	algorithm to uniquify a symbol. */
	env->symbol_stack.top = env->symbol_stack.bottom + env->symbol_stack.size;

	/* Start a time reference point. */
	env->timer = muse_tick();

	/* Create the main process. */
	{
		void *saved_sp = NULL;
		__asm
		{
			mov saved_sp, esp
		};

		{
			muse_process_frame_t *p = create_process( env, env->parameters[MUSE_DEFAULT_ATTENTION], MUSE_NIL, saved_sp );
			env->current_process = p;
			init_process_mailbox(p);
			prime_process( env, p );

			/* Immediately switch to running state. */
			p->state_bits = MUSE_PROCESS_RUNNING;
		}
	}

	/* Make sure the built-in symbol initialization doesn't use any net stack space. */
	{
		int sp = _spos();
		env->builtin_symbols = (muse_cell*)calloc( MUSE_NUM_BUILTIN_SYMBOLS, sizeof(muse_cell) );
		init_builtin_symbols( env->builtin_symbols );
		
		muse_load_builtin_fns();
		_unwind(sp);
	}

	return env;
}

void muse_network_shutdown();

/**
 * Destroys the given environment. If the given
 * environment is the current environment, then
 * the current environment is set to NULL and
 * no muse calls can subsequently be made.
 */
void muse_destroy_env( muse_env *env )
{
	/* Mark all processes as dead. */
	{
		muse_process_frame_t *cp = env->current_process;
		muse_process_frame_t *p = cp;
		do
		{
			p->state_bits = MUSE_PROCESS_DEAD;
			p = p->next;
		}
		while ( p != cp );
	}

	muse_gc(0);
	muse_network_shutdown();
	muse_tock(env->timer);
	free(env->builtin_symbols);
	env->builtin_symbols = NULL;
//GONE!	destroy_stack( &env->bindings_stack );
	destroy_stack( &env->symbol_stack );
//GONE!	destroy_stack( &env->stack );
	destroy_heap( &env->heap );
	free( env->parameters );
	
	if ( env == _env() )
		muse_set_current_env(NULL);
		
	free( env );
}

/**
 * Returns the current muse environment or NULL
 * if none is set.
 */
muse_env *muse_get_current_env()
{
	return _env();
}

/**
 * Changes the current muse environment to the
 * given one.
 */
muse_env *muse_set_current_env( muse_env *env )
{
	muse_env *prev = g_muse_env;
	g_muse_env = env;
	return prev;
}

/**
 * Allocates a new cons cell with the given head 
 * and tail cells. This is the primary constructor
 * in the whole of muse out of which all muse objects
 * are built. A new cell for the cons operation is
 * taken from the heap's free list. If no cell is 
 * available the garbage collector is invoked to
 * collect the unreferenced cells into the free list.
 * If absolutely no cells are available, then the heap
 * is grown in order to get a free cell to allocate
 * to the cons. The newly allocated cell is placed on
 * the stack so that it won't be inadvertently garbage
 * collected. This way, you can safely write expressions
 * of the form @code muse_cons( muse_cons(a,b), muse_cons(c,d) ) @endcode 
 * 
 * The cons operation does not fail unless there is
 * absolutely no system memory available for the new cell.
 */
muse_cell muse_cons( muse_cell head, muse_cell tail )
{
	muse_env *env = _env();
	
	if ( env->heap.free_cells == MUSE_NIL )
	{
		/* Make sure that the given head and tail
		 * will not be accidentally freed by the gc
		 * operation,m by pushing them onto the stack. 
		 */
		int sp = _spos();
		_spush(head);
		_spush(tail);
		muse_gc(1);
		_unwind(sp);
		
		if ( env->heap.free_cells == MUSE_NIL )
		{
			fprintf( stderr, "\t\t\tNo free cells!\n" );
			grow_heap( &env->heap, env->heap.size_cells * 2 );
		}
	}

	{
		muse_cell c = _takefreecell();
		_setht( c, head, tail );
		_spush(c);
		return c;
	}
}

/**
 * Allocates a new integer cell to hold the given integer. 
 * The newly allocated cell is placed on the stack.
 */
muse_cell muse_mk_int( muse_int i )
{
	muse_cell c = _setcellt( muse_cons( 0, 0 ), MUSE_INT_CELL );
	_ptr(c)->i = i;
	return c;
}

/**
 * Allocates a new float cell to hold the given integer.
 * The newly allocated cell is placed on the stack.
 */
muse_cell muse_mk_float( muse_float f )
{
	muse_cell c = _setcellt( muse_cons( 0, 0 ), MUSE_FLOAT_CELL );
	_ptr(c)->f = f;
	return c;
}

/**
 * Returns the current stack position. You can
 * subsequently unwind to the position by calling
 * \c muse_stack_unwind(), passing the position
 * as its argument.
 * 
 * A muse enviroment maintains a stack of cell references
 * which will not be garbage collected the next time the gc
 * is invoked. Every API function that uses \ref muse_cons
 * takes at least one new cell from the free list and
 * places it on the stack. Such functions include \ref muse_mk_int,
 * \ref muse_mk_float and family, the \ref muse_list and
 * related functions and so on - anything that will need to
 * allocate a new cell.
 * 
 * Once a reference to a cell is placed into another cell,
 * either by using \ref muse_define on a named symbol, or
 * by placing it in the property list of a named symbol or
 * by adding it to a list that is itself protected by being
 * on the stack, the stack space occupied by the cell can be
 * released. This is done using \ref muse_stack_unwind.
 * 
 * If a function does not allocate an unreferenced cell, but
 * creates temporary cells, it should reset the stack position
 * before it returns. This is typically done as follows -
 * @code
 * void my_func()
 * {
 *     int sp = muse_stack_pos();
 *     / * ... code that allocates new cells ... * /
 *     muse_stack_unwind(sp);
 * }
 * @endcode
 * 
 * If a function returns as its result a newly constructed compound
 * data structure referenced by a single muse_cell reference, it is
 * advisable for it to place it on the stack, but release all other
 * stack references to the constituent elements. For example -
 * @code
 * muse_cell my_make_blooper( int arg )
 * {
 *     int sp = muse_stack_pos();
 *     muse_cell result = MUSE_NIL;
 *     / * .. compute the data structure and store the reference in "result". * /
 *	   muse_unwind_stack(sp);
 *     muse_stack_push(result);
 *     return result;
 * }
 * @endcode
 * 
 * @see muse_stack_unwind, muse_stack_push
 */
int	muse_stack_pos()
{
	return _spos();
}

/**
 * Unwinds the stack to the given position.
 * This discards all references to cells that
 * have been kept on the stack. Temporary
 * objects which were placed on the stack
 * that aren't referred to elsewhere will be
 * available for garbage collection.
 * 
 * @see muse_stack_pos
 */
void muse_stack_unwind( int stack_pos )
{
	_unwind(stack_pos);
}

/**
 * Saves the given object on the stack so that
 * it will not be garbage collected when gc
 * is next invoked.
 * 
 * @see muse_stack_pos
 */
muse_cell muse_stack_push( muse_cell obj )
{
	_spush(obj);
	return obj;
}

static void add_special( muse_cell special )
{
	_lpush( muse_cons( special, 0 ), &_env()->specials );
}

/**
 * Copies the given text and creates a new text cell to
 * store it. The newly allocated cell is placed on the stack.
 * If you pass NULL for \p start, then this creates a new
 * text cell of the needed length (= end-start), but
 * stores null characters in it. You can subsequently
 * change the contents of the text cell.
 * 
 * @internal The cell is also placed on the specials list
 * so that the memory required to hold the cell can be
 * destroyed when the text cell is no longer needed and is
 * garbage collected.
 */ 
muse_cell muse_mk_text( const muse_char *start, const muse_char *end )
{
	muse_cell c			= _setcellt( muse_cons( 0, 0 ), MUSE_TEXT_CELL );
	muse_cell_data *d	= _ptr(c);
	
	d->text.start		= (muse_char *)malloc( (end - start + 1) * sizeof(muse_char) );
	d->text.end			= d->text.start + (end - start);
	*(d->text.end)		= 0;

	/* If start is NULL, it means we only know the length
	and we're supposed to create a blank string of that length. */
	if ( start )
	{
		memcpy( d->text.start, start, sizeof(muse_char) * (end - start) );
	}

	add_special(c);
		
	return c;
}

/**
 * Same as \c muse_mk_text() except that it takes a UTF8 string
 * and converts it into a unicode string and stores it.
 */
muse_cell muse_mk_text_utf8( const char *start, const char *end )
{
	muse_cell c			= _setcellt( muse_cons( 0, 0 ), MUSE_TEXT_CELL );
	muse_text_cell *t	= &_ptr(c)->text;
	int len				= (int)(end - start);

	t->start			= (muse_char*)calloc( muse_unicode_size(start, len), 1 );
	t->end				= t->start + muse_utf8_to_unicode( t->start, len, start, len );

	add_special(c);
	
	return c;
}

/**
 * Same as \c muse_mk_text() except that it assumes a null
 * terminated string.
 */
muse_cell muse_mk_ctext( const muse_char *start )
{
	return muse_mk_text( start, start + wcslen(start) );
}

/**
 * Same as \c muse_mk_text_utf8() except that it assumes
 * a null terminated string as input.
 */
muse_cell muse_mk_ctext_utf8( const char *start )
{
	return muse_mk_text_utf8( start, start + strlen(start) );
}

/**
 * Creates a new cell that stores a C function. The 
 * C function can be passed its own context data that
 * is not managed by muSE. The function pointer is given
 * in \p fn and the context data is given in \p context.
 * Using the context pointer, it is possible to encode
 * a call to a C++ member function, by writing a static 
 * wrapper function.
 */
muse_cell muse_mk_nativefn( muse_nativefn_t fn, void *context )
{
	muse_cell c			= _setcellt( muse_cons( 0, 0 ), MUSE_NATIVEFN_CELL );
	muse_cell_data *p	= _ptr(c);
	
	p->fn.fn			= fn;
	p->fn.context		= context;
	
	return c;
}

/**
 * A destructor is a native function that also gets
 * called with no arguments when the function is 
 * garbage collected.
 */
muse_cell muse_mk_destructor( muse_nativefn_t fn, void *context )
{
	muse_cell f = muse_mk_nativefn( fn, context );
	add_special(f);
	return f;
}

static muse_cell lookup_symbol( const muse_char *start, const muse_char *end, muse_int *out_hash )
{
	muse_int hash = muse_hash_text( start, end, MUSE_SYMBOL_CELL );
	
	muse_stack *ss = _symstack();
	
	if ( out_hash )
		*out_hash = hash;

	/* The symbols are allocated over a set of buckets according to 
		their hashes. All symbols with the same hash are assigned to the
		same bucket, which essentially contains a linear linked list of
		symbols. After finding the bucket, we just do a linear search
		through the symbols in the list to find the one that matches,
		if there is one. */
	{
		int bucket = (int)(((hash % ss->size) + ss->size) % ss->size);
		muse_cell slist = ss->bottom[bucket];
		
		while ( slist )
		{
			muse_cell considering = _next(&slist);
			muse_cell symdef = _head(_tail(considering));
			
			if ( _ptr(_head(symdef))->i == hash )
			{
				/* Maybe found. */
				muse_text_cell t = _ptr(_tail(symdef))->text;
				if ( (t.end - t.start) == (end - start) && wcscmp( t.start, start ) == 0 )
				{
					/* Found. */
					return considering;
				}
			}
		}
		
		return MUSE_NIL;
	}
}

/**
 * Returns a reference to a named symbol with the given name.
 * All symbols with the same name have identical symbol references.
 * Therefore you can compare whether two symbols are the same
 * by comparing their cell references.
 * 
 * @internal
 * A symbol is a compound constructed out of cons cells.
 * 	- symbol = (value . ((hash . name) . plist))
 * 	- values = (symbol . () )
 * 	- plist = () or (( key . value ) . plist)
 * 
 * \c values is a stack of symbol value definitions
 * that you can push and pop using \c muse_pushdef() and
 * \c muse_popdef().
 */
muse_cell muse_symbol( const muse_char *start, const muse_char *end )
{
	int p = -1;
	muse_int hash = 0;
	muse_cell sym = lookup_symbol( start, end, &hash );
	
	if ( sym )
		return sym;
	else
	{
		muse_stack *ss = _symstack();
		int local_ix = _newlocal();
		
		/* sym -> ( . ) */
		p = _spos();
		sym = _setcellt( muse_cons( _localcell(local_ix), MUSE_NIL ), MUSE_SYMBOL_CELL );
		
		{
			muse_cell name = muse_mk_text( start, end );
			
			/* symplist -> ( ( hash . name ) . nil ) */
			muse_cell symplist = muse_cons( muse_cons( muse_mk_int( hash ),
													   name ),
											MUSE_NIL );
			
			/* sym -> ( sym . symplist ) */
			_sett( sym, symplist );

			/* Define the symbol to be itself in all processes. */
			{
				muse_process_frame_t *cp = _env()->current_process;
				muse_process_frame_t *p = cp;

				do
				{
					p->locals.bottom[local_ix] = sym;
					p->locals.top = p->locals.bottom + _env()->num_symbols;
					p = p->next;
				}
				while ( p != cp );
			}
		}
		
		/* Add the symbol to its hash bucket. */
		{
			int bucket = (int)(((hash % ss->size) + ss->size) % ss->size);
			ss->bottom[bucket] = muse_cons( sym, ss->bottom[bucket] );
		}

		_unwind(p);
		
		return sym;
	}
}

/**
 * Same as \c muse_symbol(), but takes a c-style null
 * terminated unicode character string.
 */
muse_cell muse_csymbol( const muse_char *sym )
{
	return muse_symbol( sym, sym + wcslen(sym) );
}

/**
 * Same as \c muse_symbol() except that it takes a
 * UTF8 string.
 */
muse_cell muse_symbol_utf8( const char *start, const char *end )
{
	int utf8_len = (int)(end - start);
	muse_char *s = (muse_char*)calloc( muse_unicode_size(start, utf8_len), 1 );
	int len = (int)muse_utf8_to_unicode( s, utf8_len, start, utf8_len );
	
	{
		muse_cell c = muse_symbol( s, s + len );
		free(s);
		return c;
	}
}

/**
 * Same as \c muse_symbol_utf8() except that it takes
 * a c-style null terminated utf8 string.
 */
muse_cell muse_csymbol_utf8( const char *sym )
{
	return muse_symbol_utf8( sym, sym + strlen(sym) );
}

/**
 * Returns a symbol reference corresponding to the given
 * symbol index. This a convenience function.
 * 
 * @see muse_builtin_symbol_t
 */
muse_cell muse_builtin_symbol( muse_builtin_symbol_t s )
{
	muse_assert( s >= 0 && s < MUSE_NUM_BUILTIN_SYMBOLS );
	return _env()->builtin_symbols[s];
}

/**
 * An anonymous symbol is similar to a named symbol, except
 * that it is not stored permanently on the symbol stack.
 * It has a property list as well and a value stack,
 * but the symbol name cell is nil. The hash code of 
 * an anonymous symbol is the integer value of cell
 * reference itself, so comparing using the hash is
 * the same as comparing the cell reference itself.
 * 
 * Anonymous symbols are used to represent objects
 * in muSE's object system. An object's properties are
 * stored in the plist of an anonymous symbol.
 */
muse_cell muse_mk_anon_symbol()
{
	muse_cell sym = _setcellt( muse_cons( 0, 0 ), MUSE_SYMBOL_CELL );
	
	int p = _spos();
	
	muse_cell symval	= sym;
	muse_cell symplist	= muse_cons( muse_cons( muse_mk_int(sym),
												MUSE_NIL ),
									 MUSE_NIL );
	
	_setht( sym, symval, symplist );	
	_unwind( p );
	
	return sym;
}

/**
 * Prior to garbage collection, muse_mark is called
 * on all cells which are referenced somewhere and
 * should not be garbage collected. The unmarked cells
 * are then swept into the free list.
 */
void muse_mark( muse_cell c )
{
	if ( c > 0 && !_ismarked(c) )
	{
		_mark(c);
		
		if ( _iscompound(c) )
		{
			/* We need to quick-unquote components of
			compound structures if they have been quick-quoted.
			For example, macros are specified as lambda expressions
			with a quick-quoted formals list. 
			
			Using _quq() here incurs some significant penalty
			at gc time, but we need it for correctness.

			TODO: Find out if there's a more optimal way to handle this. */

			muse_mark( _quq(_head(c)) );
			muse_mark( _quq(_tail(c)) );
		}
		else
		{
			/* If the cell is a functional object, mark all cells whose
			references it holds as well. */

			muse_functional_object_t *obj = _fnobjdata(c);
			if ( obj && obj->type_info->mark )
			{
				obj->type_info->mark(obj);
			}
		}
	}
}

static void mark_stack( muse_stack *stack )
{
	muse_cell *bottom = stack->bottom;
	muse_cell *top = stack->top;
	
	while ( bottom < top )
		muse_mark( *bottom++ );
}

static void free_text( muse_cell t )
{
	if ( t )
	{
		muse_text_cell *c = &_ptr(t)->text;
		if ( c->start )
			free( c->start );
		c->start = c->end = NULL;
	}
}

void free_unused_specials( muse_cell *specials )
{
	muse_cell *cp = specials;
	muse_cell c = *cp;
	
	while ( c )
	{
		muse_cell s = _head(c);

		if ( _ismarked( s ) )
		{
			/* Leave special untouched. */
			_mark(c);
			cp = &_ptr(c)->cons.tail;
			c = *cp;
		}
		else
		{
			/* 
				Release the special. 
				For a text cell, we need to release the memory allocated for the string.
				If a nativefn is in the specials list, it means that it needs to be
				executed in this phase. This can help take care of running destructors
				for objects that are really allocated in native-c code.
			 */
			switch ( _cellt(s) )
			{
				case MUSE_TEXT_CELL			: free_text(s); break;
				case MUSE_NATIVEFN_CELL		: 
					{
						muse_functional_object_t *data = _fnobjdata(s);
						if ( data )
						{
							if ( data->type_info->destroy )
								data->type_info->destroy(data);

							free(data);
						}
						else
							muse_apply( s, MUSE_NIL, MUSE_FALSE );
					}
					break;
				
				default:;
			}
			
			_step(&c);
			*cp = c;
		}
	}
}

void collect_free_cells( muse_heap *heap )
{
	muse_cell f = MUSE_NIL;
	int marks_size = heap->size_cells;
	unsigned char *marks = heap->marks;
	int i, j, fcount;
	
	/* The nil cell is never freed. */
	_mark(MUSE_NIL);
	
	for ( i = 0, fcount = 0; i < marks_size; )
	{
		if ( !marks[i>>3] )
		{
			/* 
			   Grab 8 cells into the free list at one shot. 
			   This is a significant optimization since the
			   mark bits don't need to be checked in the
			   inner loop.
			*/
			muse_cell_data *p = _ptr(_cellati(i));
			muse_cell_data *p_end = p + 7;
			for ( j = i; p < p_end; ++p, ++i )
			{
				p->cons.head = MUSE_NIL;
				p->cons.tail = _cellati(i+1);
			}
			
			p_end->cons.head = MUSE_NIL;
			p_end->cons.tail = f;
			f = _cellati(j);
			fcount += 8;
			++i;
		}
		else if ( marks[i>>3] == 0xFF )
		{
			// Opportunity to skip 8 marked cells.
			i += 8;
		}
		else
		{
			/* Check each cell and add to free list if it is not marked. */
			for ( j = i + 8; i < j; ++i )
			{
				muse_cell c = _cellati(i);
				
				if ( !_ismarked(c) )
				{
					_ptr(c)->cons.head = MUSE_NIL;
					_ptr(c)->cons.tail = f;
					f = c;
					++fcount;
				}
			}
		}
	}
	
	heap->free_cell_count = fcount;
	heap->free_cells = f;
}


void muse_gc_impl( int free_cells_needed );

/**
 * Collects all the garbage cells - i.e. unreferenced
 * cells. Cells referenced on the various stacks 
 * including the symbol stack are not collected.
 * \c muse_cons() automaticallly invokes this function
 * when it cannot take a cell from the free list.
 * 
 * @param free_cells_needed Tells the garbage collector
 * to ensure that these many free cells are available 
 * after collection. If simply collecting unreferenced
 * cells is insufficient, the collector will grow the heap
 * by a sufficient amount. If this parameter is <= 0,
 * it means muse environment is being destroyed and 
 * the gc call simply destroys all the specials.
 */
void muse_gc( int free_cells_needed )
{
	fprintf(stderr, "Gc...");
	fflush(stderr);
	{
		muse_int time_taken;
		void *timer = muse_tick();
		muse_gc_impl( free_cells_needed );
		time_taken = muse_tock(timer);
		fprintf(stderr, "done. (free cells = %d)\n", _heap()->free_cell_count);		
		fprintf( stderr, "(time taken = " MUSE_FMT_INT " microseconds)\n", time_taken );
		fflush( stderr );
	}
}

void muse_gc_impl( int free_cells_needed )
{
	muse_heap *heap = _heap();
	
	if ( free_cells_needed <= 0 || heap->free_cells == MUSE_NIL || heap->free_cell_count < free_cells_needed * 2 )
	{
		/* We need to gc. */
		
		/* 1. Unmark all cells. */
		memset( heap->marks, 0, heap->size_cells >> 3 );
		
		if ( free_cells_needed > 0 )
		{
			_mark(0);

			/* 2. Mark all symbols and their values and plists. */
			/*mark_stack( _symstack() );*/
			mark_stack( _symstack() );
			
			/* 3. Mark references held by every process. */
			{
				muse_process_frame_t *cp = _env()->current_process;
				muse_process_frame_t *p = cp;

				do 
				{
					mark_process(p);
					p = p->next;
				}
				while ( p != cp );
			}

			/* 4. Go through the specials list and release 
				  everything that isn't referenced. */
			free_unused_specials( &_env()->specials );
			
			/* 5. Collect whatever is unmarked into the free list. */
			collect_free_cells( _heap() );
			
			if ( heap->free_cell_count < (100 - _env()->parameters[MUSE_GROW_HEAP_THRESHOLD]) * heap->size_cells / 100 )
			{
				/* We're still too close to the edge here. Allocate 
				   enough memory. */
				int new_size = heap->size_cells;
				int opt_size = 2 * (heap->size_cells - heap->free_cell_count + free_cells_needed);
				while ( new_size < opt_size )
					new_size *= 2;
				
				grow_heap( heap, new_size );
			}
		}
		else
		{
			/* Go through the specials list and release 
			everything that isn't referenced. We have to release
			everythign when shutting down. free_cells_needed <= 0
			indicates that we're shutting down. */
			free_unused_specials( &_env()->specials );
		}
	}
}

/**
 * Creates a new functional object instance based on the given type
 * information. The type of the returned cell is a native function,
 * so that the object can be used in the function position.
 */
muse_cell muse_mk_functional_object( muse_functional_object_type_t *type_info, muse_cell init_args )
{
	muse_assert( type_info && type_info->magic_word == 'muSE' );
	muse_assert( type_info->size >= sizeof(muse_functional_object_t) );

	{
		muse_functional_object_t *obj = (muse_functional_object_t*)calloc( 1, type_info->size );
		obj->magic_word		= 'muSE';
		obj->type_info		= type_info;
		if ( obj->type_info->init )
			obj->type_info->init(obj, init_args);

		{
			muse_cell fn = muse_mk_nativefn( obj->type_info->fn, obj );
			add_special(fn);
			return fn;
		}
	}
}

/**
 * Returns the data pointer of the functional object, or
 * NULL if the object is not a functional object.
 */
muse_functional_object_t *muse_functional_object_data( muse_cell fobj, int type_word )
{
	muse_functional_object_t *obj = _fnobjdata(fobj);
	
	if ( obj && (!type_word || type_word == obj->type_info->type_word) )
		return obj;
	else
		return NULL;
}

muse_process_frame_t *create_process( muse_env *env, int attention, muse_cell thunk, void *sp )
{
	muse_process_frame_t *p = (muse_process_frame_t*)calloc( 1, sizeof(muse_process_frame_t) );

	muse_assert( attention > 0 );

	p->attention				= attention;
	p->remaining_attention		= attention;
	p->state_bits				= MUSE_PROCESS_PAUSED;
	p->thunk					= thunk;
	
	/* Create all the stacks. */
	init_stack( &p->stack,			env->parameters[MUSE_STACK_SIZE]		);
	init_stack( &p->bindings_stack, env->parameters[MUSE_STACK_SIZE] * 2	);
		/**< 
		 * We need a x2 in the size for the bindings stack 
		 * above because the bindings stack is an array of 
		 * symbol-value pairs.
		 */
	init_stack( &p->locals,			env->parameters[MUSE_MAX_SYMBOLS]		);

	if ( sp == NULL )
	{
		/* This is not the main process. Create a stack frame. 
		The cstack frame is different from the other stack frame
		in that it grows down. So top is the place the SP has to
		jump to when entering the process and it'll be decremented
		as more items gets pushed onto it. */
		init_stack( &p->cstack, env->parameters[MUSE_STACK_SIZE] );
		p->cstack.top = p->cstack.bottom + p->cstack.size - 1;
	}
	else
	{
		/* Its the main process. We just use the main stack frame as is. */
		p->cstack.top = (muse_cell*)sp;
	}

	/* Initialize the queue pointers. */
	p->next = p->prev = p;

	/* Copy all the currently defined symbols over to the new process. */
	if ( env->current_process )
		memcpy( p->locals.bottom, env->current_process->locals.bottom, sizeof(muse_cell) * env->num_symbols );

	return p;
}

muse_process_frame_t *init_process_mailbox( muse_process_frame_t *p )
{
	/* Messages get appended at the end of the mailbox
	and popped off at the beginning. */
	int sp = _spos();
	p->mailbox = muse_cons( muse_mk_destructor( (muse_nativefn_t)fn_pid, p ), MUSE_NIL );
	p->mailbox_end = p->mailbox;
	_unwind(sp);
	return p;
}

muse_cell process_id( muse_process_frame_t *p )
{
	return muse_head(p->mailbox);
}

/**
 * Should be called after create_process to get it rolling.
 */
muse_boolean prime_process( muse_env *env, muse_process_frame_t *process )
{
	if ( env->current_process && env->current_process != process )
	{
		muse_assert( env->current_process->next != process );

		/* Insert the process into the circular queue of processes,
		right after the current process. */
		{
			muse_process_frame_t *temp	= env->current_process->next;
			env->current_process->next	= process;
			process->next				= temp;
			process->prev				= env->current_process;
			temp->prev					= process;
		}
	}

	/* The process is being primed for first run.
	Just save the current state. */
	process->state_bits = MUSE_PROCESS_VIRGIN;

	return MUSE_TRUE;
}

muse_boolean run_process( muse_env *env )
{
	if ( env->current_process->cstack.size > 0 && env->current_process->thunk )
	{
		/* Repeatedly evaluate the thunk in a loop until the
		thunk returns a non-NIL value. */
		while ( !muse_apply( env->current_process->thunk, env->current_process->mailbox, MUSE_TRUE ) );

		/* Process completed. Switch to the next one. */
		return kill_process( env, env->current_process );
	}
	else
		return MUSE_TRUE;
}

muse_boolean switch_to_process( muse_env *env, muse_process_frame_t *process )
{
	if ( env->current_process == process )
		return MUSE_TRUE;

	if ( process->state_bits & (MUSE_PROCESS_RUNNING | MUSE_PROCESS_VIRGIN) )
	{
		/* The process is running. Save current process state
		and switch to the given process. */

		if ( env->current_process->state_bits & MUSE_PROCESS_DEAD )
			env->current_process = process;

		if ( setjmp( env->current_process->jmp ) == 0 )
		{
			env->current_process = process;

			if ( env->current_process->state_bits & MUSE_PROCESS_VIRGIN )
			{
				env->current_process->state_bits = MUSE_PROCESS_RUNNING;

				/* Change the ESP for the virgin process. */
				{
					void *new_sp = (void*)(env->current_process->cstack.top);
					__asm
					{
						push new_sp
						pop esp
					};
				}

				return run_process( _env() );
			}
			else
				longjmp( env->current_process->jmp, 1 );
		} 

		return MUSE_TRUE;
	}

	if ( process->state_bits & MUSE_PROCESS_WAITING )
	{
		/* Process is waiting for a message. Check if there are any messages
		in the mailbox. If there is any, resume the process. If there aren't
		any messages, check the timeout value in order to resume the process. */

		if ( _tail(process->mailbox) )
		{
			/* There is a message. Resume it. */
			process->state_bits = MUSE_PROCESS_RUNNING;
		}
		else if ( process->state_bits & MUSE_PROCESS_HAS_TIMEOUT )
		{
			/* Check timeout. */
			muse_int elapsed_us = muse_elapsed_us(env->timer);
			if ( elapsed_us >= process->timeout_us )
				process->state_bits = MUSE_PROCESS_RUNNING;
		}

		if ( process->state_bits == MUSE_PROCESS_RUNNING )
			return switch_to_process( env, process );
	}

	/* Can't run this one. Try the next process. */
	return switch_to_process( env, process->next );
}

void yield_process( int spent_attention )
{
	muse_process_frame_t *p = _env()->current_process;

	if ( p->atomicity == 0 )
	{
		/* Not in an atomic block. So check remaining attention. */
		if ( p->remaining_attention <= 0 )
		{
			/* Give time to the next process. */
			p->remaining_attention = p->attention;
			switch_to_process( _env(), p->next );
		}
		else
			p->remaining_attention -= spent_attention;
	}
}

muse_boolean kill_process( muse_env *env, muse_process_frame_t *process )
{
	/* First take the process out of the ring. */
	muse_process_frame_t 
		*prev = process->prev,
		*next = process->next;

	if ( next && next != process )
	{
		muse_assert( prev != process );
		next->prev = prev;
	}

	if ( prev && prev != process )
	{
		muse_assert( next != process );
		prev->next = next;
	}

	process->state_bits = MUSE_PROCESS_DEAD;
	process->next = process->prev = process;

	if ( env->current_process == process )
		return switch_to_process( env, next );
	else
		return MUSE_TRUE;
}

/**
 * Used to send asynchronous messages to a process.
 * Also serves as its process ID.
 */
static muse_cell fn_pid( muse_env *env, muse_process_frame_t *p, muse_cell args )
{
	int sp = _spos();

	/* The PID of a process is stored in the head of the mailbox list.
	The tail of the mailbox list consists of the message queue. */
	if ( args && p->state_bits != MUSE_PROCESS_DEAD )
	{
		/* The list of arguments is the message in its full structure.
		To this, we prepend the pid of the sending process and append
		the result to the message queue of our process. */
		muse_cell msg = muse_cons( muse_head(env->current_process->mailbox), muse_eval_list(args) );

		muse_cell msg_entry = muse_cons( msg, MUSE_NIL );

		muse_set_tail( p->mailbox_end, msg_entry );
		p->mailbox_end = msg_entry;

		_unwind(sp);

		return muse_builtin_symbol( MUSE_T );
	}

	if ( p->state_bits == MUSE_PROCESS_DEAD )
	{
		/* Cleanup process memory. */
		destroy_stack( &p->locals );
		destroy_stack( &p->bindings_stack );
		destroy_stack( &p->stack );
		free(p);
	}

	return MUSE_NIL;
}

/**
 * Marks all references held by the given process.
 */
void mark_process( muse_process_frame_t *p )
{
	mark_stack( &p->stack );
	mark_stack( &p->bindings_stack );
	mark_stack( &p->locals );
	muse_mark( p->thunk );
	muse_mark( p->mailbox );
}

/**
 * Enters an atomic block that should be evaluated
 * without switching to another process.
 */
void enter_atomic()
{
	_env()->current_process->atomicity++;
}

/**
 * Leaves the current atomic block that should be evaluated
 * without switching to another process. Atomic blocks can be
 * nested, but must always be matched. Use the fn_atomic
 * muSE function to mark an atomic do block.
 */
void leave_atomic()
{
	_env()->current_process->atomicity--;
}
