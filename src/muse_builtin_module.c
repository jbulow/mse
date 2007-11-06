/**
 * @file muse_builtin_module.c
 * @author Srikumar K. S. (mailto:kumar@muvee.com)
 *
 * Copyright (c) 2006 Jointly owned by Srikumar K. S. and muvee Technologies Pte. Ltd. 
 *
 * All rights reserved. See LICENSE.txt distributed with this source code
 * or http://muvee-symbolic-expressions.googlecode.com/svn/trunk/LICENSE.txt
 * for terms and conditions under which this software is provided to you.
 *
 * A very simple module system for muSE.
 */

#include "muse_builtins.h"
#include <stdlib.h>
#include <memory.h>

/** @addtogroup FunctionalObjects */
/*@{*/
/**
 * @defgroup Modules
 *
 * A module is a value that contains a set of variable bindings
 * you can refer to. You define a module using the \ref syntax_module "module"
 * syntax which creates a module value. Symbols exported from a module
 * are accessed using the long form notation @code ModuleName.exportSymbol @endcode.
 * If you want to use a module's exports just by their short names,
 * you can import its definitions using \ref fn_import "import".
 */
/*@{*/

enum { MODULE_SEP = '.' };

typedef struct {
	muse_cell short_name;
	muse_cell full_name;
	muse_cell value;
} module_binding_t;
	
typedef struct _module_t
{
	muse_functional_object_t base;
	int length;
	module_binding_t *bindings;
} module_t;

static muse_cell qualified_name( muse_env *env, int prefix_length, const muse_char *prefix, muse_cell sym )
{
	muse_cell name = _symname(sym);
	int len = 0;
	const muse_char *text = _text_contents( name, &len );
	muse_char *mem = (muse_char*)calloc( len + prefix_length + 1, sizeof(muse_char) );
	memcpy( mem, prefix, prefix_length * sizeof(muse_char) );
	memcpy( mem + prefix_length, text, len * sizeof(muse_char) );
	mem[ prefix_length + len ] = 0;
	
	{
		muse_cell qname = muse_symbol( env, mem, mem + prefix_length + len );
		free(mem);
		return qname;
	}
}

static void module_init( muse_env *env, void *ptr, muse_cell args )
{
	int bsp = _bspos();
	module_t *m = (module_t*)ptr;
	muse_cell mname = _next(&args);
	muse_cell exports = _next(&args);
	m->length = muse_list_length( env, exports );
	m->bindings = (module_binding_t*)calloc( m->length, sizeof(module_binding_t) );
	
	/* Reset the definitions of exported values. */
	{
		int i = 0;
		int prefix_length = 0;
		muse_char *prefix = (muse_char*)_text_contents( _symname(mname), &prefix_length );
		muse_cell e = exports;
		
		/* HACK: We mofiy the last NULL character in the text contents temporarily
		to get the prefix string. This saves an allocation. */
		prefix[prefix_length] = MODULE_SEP;
		for ( ; i < m->length; ++i ) {
			muse_cell sym = _next(&e);
			module_binding_t *b = m->bindings + i;
			muse_assert( _cellt(sym) == MUSE_SYMBOL_CELL );
			
			b->short_name = sym;
			b->full_name = qualified_name( env, prefix_length+1, prefix, sym );
			b->value = b->full_name;
			_pushdef( b->short_name, b->short_name );
			_pushdef( b->full_name, b->full_name );
		}
		prefix[prefix_length] = '\0';
	}

	/* Evaluate the body of the module. */
	_force( muse_do( env, args ) );
	
	/* Capture new definitions of the exported symbols. */
	{
		int i = 0;
		for ( ; i < m->length; ++i ) {
			module_binding_t *b = m->bindings + i;
			b->value = _symval(b->short_name);
		}
	}
	
	/* Restore old definitions. */
	_unwind_bindings(bsp);
	
	/* Introduce new global/local bindings. */
	{
		int i = 0;
		for ( ; i < m->length; ++i ) {
			module_binding_t *b = m->bindings + i;
			if ( bsp == 0 )
				_define( b->full_name, b->value );
			else
				_pushdef( b->full_name, b->value );
		}
	}
}

static void module_mark( muse_env *env, void *ptr )
{
	module_t *m = (module_t*)ptr;
	int i;
	for ( i = 0; i < m->length; ++i ) {
		// The symbols are automatically marked.
		muse_mark( env, m->bindings[i].value );
	}
}

static void module_destroy( muse_env *env, void *ptr )
{
	module_t *m = (module_t*)ptr;
	free( m->bindings );
	m->length = 0;
	m->bindings = NULL;
}

/**
 * Writes out the vector to the given port in such a
 * way that the expression written out is converted
 * to a vector by a trusted read operation.
 */
static void module_write( muse_env *env, void *ptr, void *port )
{
	module_t *m = (module_t*)ptr;
	muse_port_t p = (muse_port_t)port;
	
	port_putc( '{', p );
	port_write( "module ", 7, p );	
	
	port_putc( '(', p );
	{
		int i;
		for ( i = 0; i < m->length; ++i )
		{
			if ( i > 0 ) port_putc( ' ', p );
			muse_pwrite( p, m->bindings[i].short_name );
		}
	}
	port_putc( ')', p );
	port_write( " ...}", 5, p );
}

/**
 * (MyMod symbol)
 *
 * Looks up the value of the symbol in the module's context.
 */
muse_cell module_fn( muse_env *env, module_t *m, muse_cell args )
{
	muse_cell sym = _evalnext(&args);
	int i;
	for ( i = 0; i < m->length; ++i ) {
		module_binding_t *b = m->bindings + i;
		if ( sym == b->short_name || sym == b->full_name )
			return b->value;
	}
	return sym;
}

static muse_functional_object_type_t g_module_type =
{
	'muSE',
	'mmod',
	sizeof(module_t),
	(muse_nativefn_t)module_fn,
	NULL,
	module_init,
	module_mark,
	module_destroy,
	module_write
};

/**
 * @code
 * (module MyMod (exportA exportB ...)
 *   ...definitions...
 * )
 * @endcode
 *
 * Such a module expression introduces global symbols
 * MyMod.exportA, MyMod.exportB, etc. with the appropriate
 * bindings.
 *
 * Within a module, you use the usual \ref fn_define "define"
 * function to introduce definitions for whatever ie exported
 * from the module. If you introduce definitions of other symbols,
 * they will be visible only to the module block and will disappear
 * from all scope when the module completes loading. Such local 
 * symbols' values can be usefully captured in closures however.
 *
 * You can forward reference an exported symbol within a module
 * just by using its qualified name. This is valid only if the
 * forward reference will not be immediately evaluated at usage
 * point. Such a forward reference can be used within
 * function bodies.
 * 
 * Example:
 * @code
 * (module Numbers (even odd)
 *   (define (even N)
 *     (case N
 *       (0 T)
 *       (_ (Numbers.odd (- N 1)))))
 *   (define (odd N)
 *     (case N
 *       (0 ())
 *       (_ (even (- N 1)))))
 * )
 * @endcode   
 *
 * The \c module expression defines the value of the \c MyMod symbol
 * to be a function of one argument. This function takes a symbol
 * and evaluates to its value in the context of the module.
 */
muse_cell fn_module( muse_env *env, void *context, muse_cell args )
{
	muse_cell mname = _head(args);
	muse_cell mod = muse_mk_functional_object( env, &g_module_type, args );
	_define(mname,mod);
	return mod;
}

static void introduce_module( muse_env *env, module_t *m, muse_boolean global )
{
	int i = 0;
	for ( ; i < m->length; ++i ) {
		if ( global == MUSE_TRUE )
			_define( m->bindings[i].short_name, m->bindings[i].value );
		else
			_pushdef( m->bindings[i].short_name, m->bindings[i].value );
	}
}

/**
 * (import ModA ModB ..)
 *
 * Introduces symbols in all the named modules into the current
 * context, in their short forms. Useful in two places -
 *  - Inside a module definition to make the short form of symbols
 *    from aother module available for the scope of the module
 *    being defined.
 *  - To globally introduce short form names of the symbols in
 *    the given modules, when used at the REPL or file level.
 */
muse_cell fn_import( muse_env *env, void *context, muse_cell args )
{
	int bsp = _bspos();
	while ( args ) {
		muse_cell mod = _symval( _next(&args) );
		module_t *m = (module_t*)_functional_object_data( mod, 'mmod' );
		if ( m ) {
			introduce_module( env, m, bsp == 0 ? MUSE_TRUE : MUSE_FALSE );
		}
	}
	
	return MUSE_NIL;
}

/*@}*/
/*@}*/

void muse_define_builtin_type_module( muse_env *env )
{
	int sp = _spos();
	_define( _csymbol(L"module"), _mk_nativefn( fn_module, NULL ) );
	_define( _csymbol(L"import"), _mk_nativefn( fn_import, NULL ) );
	_unwind(sp);
}