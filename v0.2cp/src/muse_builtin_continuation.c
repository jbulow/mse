/**
 * @file muse_builtin_continuation.c
 * @author Srikumar K. S. (mailto:kumar@muvee.com)
 *
 * Copyright (c) 2006 Jointly owned by Srikumar K. S. and muvee Technologies Pte. Ltd. 
 *
 * All rights reserved. See LICENSE.txt distributed with this source code
 * or http://muvee-symbolic-expressions.googlecode.com/svn/trunk/LICENSE.txt
 * for terms and conditions under which this software is provided to you.
 */

#include "muse_builtins.h"
#include "muse_opcodes.h"
#include <stdlib.h>
#include <setjmp.h>
#include <memory.h>

typedef struct _continuation_t
{
	muse_functional_object_t base;
	jmp_buf		state;
	muse_process_frame_t *process;
	int			process_atomicity;
	size_t		system_stack_size;
	void		*system_stack_from;
	void		*system_stack_copy;
	int			muse_stack_size;
	int			muse_stack_from;
	muse_cell	*muse_stack_copy;
	int			bindings_stack_size;
	int			bindings_stack_from;
	muse_cell	*bindings_stack_copy;
	int			bindings_size;
	muse_cell	*bindings_copy;
	muse_cell	this_cont;
	muse_cell	invoke_result;
} continuation_t;

static void continuation_init( void *p, muse_cell args )
{
}

static void mark_array( muse_cell *begin, muse_cell *end )
{
	while ( begin < end )
	{
		muse_mark( *begin++ );
	}
}

static void continuation_mark( void *p )
{
	continuation_t *c = (continuation_t*)p;
	
	muse_assert( c->process->state_bits != MUSE_PROCESS_DEAD );

	mark_array( c->muse_stack_copy, c->muse_stack_copy + c->muse_stack_size );
	mark_array( c->bindings_stack_copy, c->bindings_stack_copy + c->bindings_stack_size );
	mark_array( c->bindings_copy, c->bindings_copy + c->bindings_size );
}

static void continuation_destroy( void *p )
{
	continuation_t *c = (continuation_t*)p;

	free( c->system_stack_copy );	
	free( c->muse_stack_copy );
	free( c->bindings_stack_copy );
	free( c->bindings_copy );
	
	{
		muse_functional_object_t base = c->base;
		memset( p, 0, sizeof(continuation_t) );
		c->base = base;
	}
}

static muse_cell *copy_current_bindings( int *size )
{
	muse_stack *s = &(_env()->current_process->locals);
	muse_cell *copy = NULL;
	int i = 0;

	(*size) = _env()->num_symbols;
	copy = (muse_cell*)malloc( sizeof(muse_cell) * (*size) );
	memcpy( copy, s->bottom, sizeof(muse_cell) * (*size) );

	return copy;
}


static void restore_bindings( muse_cell *bindings, int size )
{
	muse_cell *end = bindings + size;
	
	muse_assert( size >= 0 );

	memcpy( _env()->current_process->locals.bottom, bindings, sizeof(muse_cell) * size );
}

static void *min3( void *p1, void *p2, void *p3 )
{
	char *c1 = (char*)p1, *c2 = (char*)p2, *c3 = (char*)p3;
	char *c = c1;
	
	if ( c2 < c ) c = c2;
	if ( c3 < c ) c = c3;
	return c;
}

static void *max3( void *p1, void *p2, void *p3 )
{
	char *c1 = (char*)p1, *c2 = (char*)p2, *c3 = (char*)p3;
	char *c = c1;
	
	if ( c2 > c ) c = c2;
	if ( c3 > c ) c = c3;
	return c;	
}

static muse_cell capture_continuation( muse_env *env, muse_cell cont )
{
	continuation_t *c = (continuation_t*)muse_functional_object_data(cont,'cont');

	muse_cell result = setjmp( c->state );
	
	if ( result == MUSE_NIL )
	{
		/* We're capturing the continuation. Save all state. */
		
		/* First determine if the stack grows up or down. */
		muse_boolean stack_grows_down = ((char *)env->current_process->cstack.top > (char *)&cont) ? MUSE_TRUE : MUSE_FALSE;
		
		if ( stack_grows_down )
		{
			/* Save system state up to the c variable. Note that c's address is 
			less than result's, therefore result will also get saved. */
			SAVE_STACK_POINTER( saved_sp );
			
			c->system_stack_from = saved_sp;
			c->system_stack_size = (char*)env->current_process->cstack.top - (char*)c->system_stack_from;
			c->system_stack_copy = malloc( c->system_stack_size );
			muse_assert( is_main_process(env) || c->system_stack_size < env->current_process->cstack.size * sizeof(muse_cell) );

			memcpy( c->system_stack_copy, c->system_stack_from, c->system_stack_size );
		}
		else
		{
			muse_assert( MUSE_FALSE && "Unsupported stack growth direction!" );

			/* Save system state up to the result variable. Note that result's address is 
			greater than c's, therefore c will also get saved. */
			c->system_stack_from = env->current_process->cstack.top;
			c->system_stack_size = (char*)max3(&c, &result, &stack_grows_down) - (char*)_env()->stack_base;
			c->system_stack_copy = malloc( c->system_stack_size );
			memcpy( c->system_stack_copy, c->system_stack_from, c->system_stack_size );
		}

		/* Save the muse stack. */
		c->muse_stack_from = 0;
		c->muse_stack_size = _spos();
		c->muse_stack_copy = malloc( sizeof(muse_cell) * c->muse_stack_size );
		memcpy( c->muse_stack_copy, _stack()->bottom, sizeof(muse_cell) * c->muse_stack_size );
		
		/* Save the bindings stack. */
		c->bindings_stack_from = 0;
		c->bindings_stack_size = _bspos();
		c->bindings_stack_copy = malloc( sizeof(muse_cell) * c->bindings_stack_size );
		memcpy( c->bindings_stack_copy, env->current_process->bindings_stack.bottom, sizeof(muse_cell) * c->bindings_stack_size );

		/* Save all bindings. */
		c->bindings_copy = copy_current_bindings( &c->bindings_size );

		/* Save a pointer to the current process. */
		c->process = env->current_process;
		c->process_atomicity = env->current_process->atomicity;

		c->this_cont = cont;
		
		/* A -ve return value indicates a capture return. */
		return -1;
	}
	else
	{
		/* result-1 is the continuation object that was invoked, with
		the invoke_result field set to the argument supplied to the invocation. */
		c = (continuation_t*)muse_functional_object_data(result-1,'cont');
		muse_assert( c && c->base.type_info->type_word == 'cont' );
		
		/* Restore the process atomicity that was at capture time. 
		Also, continuation invocations cannot cross process boundaries, so
		the current process must be the one in which the continuation
		was captured. */
		muse_assert( _env()->current_process == c->process );
		c->process->atomicity = c->process_atomicity;

		/* Restore the evaluation stack. */
		memcpy( _stack()->bottom + c->muse_stack_from, c->muse_stack_copy, sizeof(muse_cell) * c->muse_stack_size );
		_unwind( c->muse_stack_from + c->muse_stack_size );

		/* Restore the bindings stack. */
		memcpy( c->process->bindings_stack.bottom + c->bindings_stack_from, c->bindings_stack_copy, sizeof(muse_cell) * c->bindings_stack_size );
		c->process->bindings_stack.top = c->process->bindings_stack.bottom + c->bindings_stack_from + c->bindings_stack_size;

		/* Restore the saved symbol values. */
		restore_bindings( c->bindings_copy, c->bindings_size );

		/* Restore the system stack. */
		memcpy( c->system_stack_from, c->system_stack_copy, c->system_stack_size );

		muse_assert( c->invoke_result >= 0 );
		muse_assert( (c->process->state_bits & MUSE_PROCESS_DEAD) == 0 );

		/* We return to fn_callcc after this. So to ensure we get into the
		"continuation invoked" branch, we have to make sure that the result
		value is non-zero. fn_callcc knows about this +1 and will decrement 
		the result and use it as a cell reference. */
		return c->invoke_result+1;
	}
}

static muse_cell fn_continuation( muse_env *env, continuation_t *c, muse_cell args )
{
	/* Continuation invocation cannot cross process boundaries. */
	muse_assert( c->process == env->current_process );

	c->invoke_result = muse_evalnext(&args);
	
	longjmp( c->state, c->this_cont+1 );

	return MUSE_NIL;
}

static muse_functional_object_type_t g_continuation_type =
{
	'muSE',
	'cont',
	sizeof(continuation_t),
	(muse_nativefn_t)fn_continuation,
	NULL,
	continuation_init,
	continuation_mark,
	continuation_destroy
};

/**
 * (call/cc (fn (k) --- (k result) ---)).
 *
 * Abbreviation for "call with current continuation", call/cc is an
 * implementation of the scheme recommendation for continuation support.
 * The first and only argument to call/cc is expected to be a function
 * that takes a single argument called the continuation. call/cc will
 * then call this function, supplying the current continuation as the
 * argument.
 *
 * A brief intro to continuations follows. When evaluating 
 * a sub-expression of any expression, the remainder of the computation may
 * be thought of as a function that expects the result of the sub-expression
 * evaluation. This "remainder of the computation" function w.r.t. the
 * specific sub-expression is called the "continuation" at that point
 * in the evaluation process. 
 *
 * The whole expression may be rewritten as a call to this continuation
 * function with the argument as the result of the sub-expression under
 * consideration. Note that the continuation function does
 * not return a value to the context in which it is called. Instead,
 * it "breaks out" of the context and pretends as though the result
 * of the sub-expression is the argument supplied to the 
 * continuation function at invocation time. 
 *
 * Its time for an example - what'll the following code print?
 * .. and then, what'll it print when \c bomb is defined to \c T instead?
 * @code
 * (define bomb ())
 * (print (+ 1 2 (call/cc (fn (k)
 *						     (print "before\n")
 *						     (if bomb (k 0))
 *						     (print "after\n")
 *							 3))
 *		     4 5))
 * @endcode
 *
 * When \c bomb is \c (), the @code (k 0) @endcode part is not
 * evaluated due to the if condition failing. Therefore the output 
 * will be -
 * @code
 * before
 * after
 * 15
 * @endcode
 *
 * When \c bomb is changed to \c T, the if block will kick in
 * and @code (k 3) @endcode will be evaluated. But since continuation
 * functions do not return to te point of invocation, but to the point
 * of the \c call/cc which captured them, the @code (print "after\n") @endcode
 * expression never gets evaluated and the result of the \c call/cc block
 * is not \c 3 as one would expect, but \c 0, because that's the argument
 * given to the continuation function when it is invoked! So you'll get
 * @code
 * before
 * 12
 * @endcode
 * as the result printed to the screen. Note that we know that the
 * continuation invocation did not return to its invocation point because
 * the expression @code (print "after\n") @endcode did not get evaluated.
 *
 * Continuations are rather powerful. The can be used to implement 
 * language constructs such as -
 *	-# try-catch style exception handling,
 *	-# breaking out of loops.
 *	-# suspend and resume mechanism.
 *
 * In general, it should be possible to store away the continuation
 * function for future invocation. Early on in muSE's development, only
 * a limited implementation of call/cc was put in in order to support
 * breaking out of loops. Later on a full implementation of continuations
 * was added. Now, you can store away the continuation function in a variable
 * and invoke it as many times as you need to, because the continuation
 * captures a complete snapshot of the execution environment at the time
 * it is created. 
 *
 * @todo The current implementation of call/cc seems to be working properly 
 * on Windows + Intel, but doesn't work correctly on PowerPC. 
 * Needs investigation.
 */
muse_cell fn_callcc( muse_env *env, void *context, muse_cell args )
{
	muse_cell proc = muse_evalnext(&args);
	
	muse_cell cont = muse_mk_functional_object( &g_continuation_type, MUSE_NIL );
	
	muse_cell result = capture_continuation(env,cont);
	
	if ( result < 0 )
	{
		/* We just captured the continuation. Invoke the proc with the 
		continuation as the argument. */
		
		return muse_apply( proc, muse_cons( cont, MUSE_NIL ), MUSE_TRUE );
	}
	else
	{
		/* The continuation was just invoked. Return the result
		without evaluating the proc. The actual result is not "result",
		but "result-1" because the fn_continuation function makes sure
		to invoke this branch by giving a result that is > 0. This is
		does by adding 1 to the actual result. */
		return result-1;
	}
}

/*------------------------------------------*/
/* Exceptions                               */
/*------------------------------------------*/

/**
 * Captures all the information required to jump
 * back to an execution point within stack scope.
 */
typedef struct _resume_point_t
{
	jmp_buf state;
	int spos;
	int bspos;
	int atomicity;
	muse_cell trapval;
	muse_cell result;
} resume_point_t;

/**
 * Usage: if ( resume_capture( env, rp, setjmp(rp->state) ) == 0 )
 *            ...
 *        else
 *            ...
 *            return rp->result;
 */
static int resume_capture( muse_env *env, resume_point_t *rp, int setjmp_result )
{
	if ( setjmp_result == 0 )
	{
		rp->spos = _spos();
		rp->bspos = _bspos();
		rp->atomicity = env->current_process->atomicity;
		rp->trapval = _symval( muse_builtin_symbol( MUSE_TRAP_POINT ) );
		rp->result = 0;
	}
	else
	{
		env->current_process->atomicity = rp->atomicity;
		_unwind( rp->spos );
		_unwind_bindings( rp->bspos );
		_def( muse_builtin_symbol( MUSE_TRAP_POINT ), rp->trapval );
		rp->result = setjmp_result-1;
	}

	return setjmp_result;
}

/**
 * Invokes an already captured resume point with the given result.
 * The longjmp call is made with result+1 and rp->result will
 * be set to the longjmp return value minus 1.
 */
static void resume_invoke( muse_env *env, resume_point_t *p, muse_cell result )
{
	longjmp( p->state, result+1 );
}

/**
 * The function that gets called to resume a particular exception.
 * At exception raise time, a resume point is captured and passed
 * on to the handlers. A handler may choose to resume the computation
 * by calling the resume function with a particular result value.
 */
static muse_cell fn_resume( muse_env *env, void *context, muse_cell args )
{
	resume_point_t *rp = (resume_point_t*)context;

	resume_invoke( env, rp, muse_evalnext(&args) );

	return MUSE_NIL;
}

/**
 * A trap point is a marker for the beginning of a
 * (try...) block. When you return to a trap point,
 * you return with a value that is supposed to be the
 * value of the try block. Trap points are maintained as
 * a stack of values of the built-in symbol "{{trap}}",
 * which is available via MUSE_TRAP_POINT.
 */
typedef struct _trap_point_t
{
	muse_functional_object_t base;
	resume_point_t escape;	/**< The resume point to invoke to return from the try block. */
	muse_cell handlers;		/**< A list of unevaluated handlers. */
	muse_cell prev;			/**< The previous trap point. */
} trap_point_t;


static void trap_point_init( void *p, muse_cell args )
{
	trap_point_t *trap = (trap_point_t*)p;

	/* We're evaluating the list of handlers here. This is
	fairly expensive to simply enter a try block. We either accept
	this overhead or accept the overhead of capturing a full 
	continuation at the point at which the exception is raised
	in order to get resumable exceptions. */
	trap->handlers = muse_eval_list(args);

	trap->prev = muse_symbol_value( muse_builtin_symbol( MUSE_TRAP_POINT ) );
}

static void trap_point_mark( void *p )
{
	trap_point_t *trap = (trap_point_t*)p;
	muse_mark(trap->handlers);
	muse_mark(trap->prev);
}

static muse_cell fn_trap_point( muse_env *env, void *trap_point, muse_cell args )
{
	muse_assert( !"fn_trap_point should never be called!" );
	return MUSE_NIL;
}

static muse_functional_object_type_t g_trap_point_type =
{
	'muSE',
	'trap',
	sizeof(trap_point_t),
	fn_trap_point,
	NULL,
	trap_point_init,
	trap_point_mark,
	NULL
};


/**
 * Marks an expression that needs to be protected by
 * exception handlers. A try block has the following syntax -
 * @code
 *  (try
 *       expr
 *       handler1
 *       handler2
 *       ....
 *  )
 * @endcode
 *
 * First it tries to evaluate the given \c expr. If the expression
 * raised an exception using (raise...), then each of the handlers
 * is tried in turn until one matches. The handlers are evaluated
 * at the time the try block is entered, not when an exception is 
 * raised, so for efficiency reasons you should always use
 * in-place handlers (using the macro brace facility) which do not
 * refer to the lexical context of the try block if possible and
 * use closures for handlers only when you absolutely need them.
 * 
 * A handler can be a function expression - like {fn args expr} or
 * {fn: args expr}. If it is such an expression, each handler is
 * tried in turn until the argument pattern of one of the handlers
 * matches the raised exception. The body of the handler whose
 * arguments matched the exception is evaluated and the result
 * is returned as the result of the try block. Handlers may themselves
 * raise exceptions in order to jump up to the enclosing try block.
 *
 * The first argument to the handler is an exception object whose
 * sole purpose is to enable the handler to resume the computation
 * with a given value as the result of the (raise...) expression that
 * raised the exception. The rest of the arguments to a handler
 * are the same arguments that were passed to \c raise.
 *
 * If a handler is not a function object, then its value is
 * used as the value of the try block without further checks.
 * Such a "handler" always succeeds.
 *
 * If none of the handlers match, then the handlers of
 * the previous enclosing try block are examined.
 *
 * A handler may choose to resume the computation by invoking
 * the exception object with the desired value that should be
 * returned by the (raise ...) expression.
 *
 * If continuations are captured in the middle of try blocks,
 * they will automatically include the correct state of the
 * try block nesting because they will capture the correct
 * value of the "{{trap}}" symbol.
 *
 * It is convenient to use read-time evaluated dynamically
 * scoped function objects as handlers since they cause the
 * least overhead and are usually sufficiently general.
 * For example -
 * @code
 *  (try (if (> a b) (raise 'NotInOrder a b) (- b a))
 *       {fn: (e 'NotInOrder a b) (e (- a b))}
 *   )
 * @endcode
 */
muse_cell fn_try( muse_env *env, void *context, muse_cell args ) 
{
	muse_cell trapval = muse_mk_functional_object( &g_trap_point_type, _tail(args) );

	trap_point_t *tp = (trap_point_t*)muse_functional_object_data( trapval, 'trap' );

	muse_cell result = MUSE_NIL;

	_def( muse_builtin_symbol( MUSE_TRAP_POINT ), trapval );

	if ( resume_capture( env, &(tp->escape), setjmp(tp->escape.state) ) == 0 )
	{
		/* Evaluate the body of the try block. */
		result = muse_evalnext(&args);
	}
	else
	{
		/* Exception raised and a handler was invoked which 
		gave us this value. Return from the try block with the
		given result. */
		result = tp->escape.result;
	}

	_def( muse_builtin_symbol( MUSE_TRAP_POINT ), tp->prev );
	return result;
}

/**
 * Examines the handlers of the given scope first, then followed by
 * the handlers of the enclosing try scope, and so on until a successful
 * handler was invoked or execution reached the top level without a handler.
 * In the latter case, the process is terminated with an "unhandled exception"
 * error.
 */
static muse_cell try_handlers( muse_env *env, muse_cell handler_args )
{
	muse_cell sym_trap_point = muse_builtin_symbol( MUSE_TRAP_POINT );

	muse_cell trapval = muse_symbol_value( sym_trap_point );

	trap_point_t *trap = (trap_point_t*)muse_functional_object_data( trapval, 'trap' );

	muse_cell handlers = trap->handlers;

	/* The trap point state must be redefined to the previous
	one because if an exception is raised within a handler,
	it must evaluate it w.r.t. the try block that encloses
	the try block in which the first exception was raised. */
	_def( muse_builtin_symbol( MUSE_TRAP_POINT ), trap->prev );

	while ( handlers )
	{
		/* Note that handlers are expected to be in-place values,
		defined using macro braces. */
		muse_cell h = _next(&handlers);

		if ( _cellt(h) == MUSE_LAMBDA_CELL )
		{
			/* A handler needs to be examined. */
			muse_cell formals = _quq(_head(h));
			int bsp = _bspos();

			if ( muse_bind_formals( formals, handler_args ) )
			{
				muse_cell result;
				result = muse_do(_tail(h));
				_unwind_bindings(bsp);
				resume_invoke( env, &(trap->escape), result );
			}
		}
		else
		{
			/* The "handler" itself is the result of the try block. */
			resume_invoke( env, &(trap->escape), h );
		}

		/* Switch to handlers of shallower scopes if necessary. */
		while ( !handlers )
		{
			trapval = trap->prev;
			trap = (trap_point_t*)muse_functional_object_data( trapval, 'trap' );

			if ( trap == NULL )
			{
				handlers = MUSE_NIL;
				break;
			}

			handlers = trap->handlers;

			/* See note above on similar line. */
			_def( muse_builtin_symbol( MUSE_TRAP_POINT ), trap->prev );
		}
	}

	/* No handler succeeded in handling the exception. */
	muse_message( L"Unhandled exception!", L"%m\nin process %m", _tail(handler_args), process_id(env->current_process) );
	remove_process( env, env->current_process );
}

/**
 * (raise ...)
 * Raises an exception described by the given arguments. Handlers are
 * matched against the pattern of arguments to determine which handler
 * to use to handle the exception. It is useful to use a quoted symbol
 * as the first argument which describes the exception. A handler can
 * then specify the same quoted symbol as its second argument in order
 * to get to handle the exception.
 *
 * (raise..) evaluates the matching handler without unwinding the stack
 * to the point of the try block. This means any raised exception can be
 * resumed by invoking the exception object (passed as the first argument
 * to the handler) with the resume value as the argument.
 *
 * @see fn_try
 */ 
muse_cell fn_raise( muse_env *env, void *context, muse_cell args ) 
{
	resume_point_t *rp = (resume_point_t*)calloc( 1, sizeof(resume_point_t) );
	muse_cell resume_pt = muse_mk_destructor( fn_resume, rp );
	muse_cell handler_args = muse_cons( resume_pt, muse_eval_list(args) );

	if ( resume_capture( env, rp, setjmp(rp->state) ) == 0 )
	{
		return try_handlers( env, handler_args );
	}
	else
	{
		return rp->result;
	}
}