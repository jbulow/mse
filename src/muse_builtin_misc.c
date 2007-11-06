/**
 * @file muse_builtin_misc.c
 * @author Srikumar K. S. (mailto:kumar@muvee.com)
 *
 * Copyright (c) 2006 Jointly owned by Srikumar K. S. and muvee Technologies Pte. Ltd. 
 *
 * All rights reserved. See LICENSE.txt distributed with this source code
 * or http://muvee-symbolic-expressions.googlecode.com/svn/trunk/LICENSE.txt
 * for terms and conditions under which this software is provided to you.
 *
 * A collection of OS-specific functions exposed in the language.
 */

#include "muse_builtins.h"
#include <string.h>

#ifdef MUSE_PLATFORM_WINDOWS
#include <windows.h>

typedef struct 
{
	muse_cell path;
	DWORD attrmask, attrcomp;
	HANDLE search;
	WIN32_FIND_DATAW data;
} filesearchinfo_t;

static muse_cell generate_files( muse_env *env, filesearchinfo_t *info, int i, muse_boolean *eol )
{
	if ( i == 0 )
	{
		info->search = FindFirstFileW( _text_contents( info->path, NULL ), &info->data );
		if ( info->search == INVALID_HANDLE_VALUE )
		{
			(*eol) = MUSE_TRUE;
			return MUSE_NIL;
		}
	}
	else
	{
		if ( !FindNextFileW( info->search, &info->data ) )
		{
			FindClose(info->search);
			(*eol) = MUSE_TRUE;
			return MUSE_NIL;
		}
	}

	while ( (info->data.dwFileAttributes & info->attrmask) != info->attrcomp 
				|| wcscmp(info->data.cFileName, L".") == 0
				|| wcscmp(info->data.cFileName, L"..") == 0 )
	{
		if ( !FindNextFileW( info->search, &info->data ) )
		{
			FindClose(info->search);
			(*eol) = MUSE_TRUE;
			return MUSE_NIL;
		}
	}

	(*eol) = MUSE_FALSE;
	return muse_mk_ctext( env, info->data.cFileName );
}

/**
 * (list-files [pattern]).
 * Returns a list of files that patch the given pattern. 
 * For example:
 * @code
 * (list-files "*.jpg")
 * @endcode
 * will list the JPG files in the current folder.
 * Note that the returned list only has the file names
 * and not the full paths to the files.
 */
muse_cell fn_list_files( muse_env *env, void *context, muse_cell args )
{
	filesearchinfo_t info;
	info.path		= _evalnext(&args);
	info.attrmask	= FILE_ATTRIBUTE_DIRECTORY;
	info.attrcomp	= 0;

	return muse_generate_list( env, (muse_list_generator_t)generate_files, &info );
}

/**
 * (list-folders parent-folder).
 * Returns a list of folder names for all the folders that exist under
 * the given parent folder.
 * For example:
 * @code
 * (list-folders "../*")
 * @endcode
 * will list the folders above the current folder.
 * Note that the returned list only has the folder names and not
 * the full paths to the folders. Also, the folder names don't end
 * with '/' or any such path separator character.
 */
muse_cell fn_list_folders( muse_env *env, void *context, muse_cell args )
{
	filesearchinfo_t info;
	info.path		= _evalnext(&args);
	info.attrmask	= FILE_ATTRIBUTE_DIRECTORY;
	info.attrcomp	= FILE_ATTRIBUTE_DIRECTORY;

	return muse_generate_list( env, (muse_list_generator_t)generate_files, &info );
}

#else // POSIX


static muse_cell generate_files( muse_env *env, FILE *info, int i, muse_boolean *eol )
{
	char line[4096];
	if ( fgets( line, 4096, info ) )
	{
		int len = strlen(line);

		// Erase the newline character at the end.
		if ( line[len-1] == '\n' || line[len-1] == '\r' )
		{
			line[--len] = '\0';
		}

		// Erase any "/" ending present for directory names.
		if ( line[len-1] == '/' )
		{
			line[--len] = '\0';
		}

		(*eol) == MUSE_FALSE;
		return muse_mk_text_utf8(env, line, line+len);
	}
	else
	{
		(*eol) = MUSE_TRUE;
		pclose(info);
		return MUSE_NIL;
	}
}

static int copy_with_space_escapes( int N, char *buffer, const muse_char *str )
{
	int count = 0;
	
	while ( count < N && (*str) ) {
		if ( isspace(*str) ) {
			buffer[count++] = '\\';
		}
		buffer[count++] = (char)(*str++);
	}
	
	buffer[count] = '\0';
	return count;
}

/**
 * (list-files [pattern]).
 * Returns a list of files that patch the given pattern. 
 * For example:
 * @code
 * (list-files "*.jpg")
 * @endcode
 * will list the JPG files in the current folder.
 * Note that the returned list only has the file names
 * and not the full paths to the files.
 */
muse_cell fn_list_files( muse_env *env, void *context, muse_cell args )
{
	FILE *f;
	char cmd[4096];
	int len = 0;
	
	const muse_char *path = _text_contents( _evalnext(&args), NULL );
	
	const muse_char *wild = wcschr( path, '*' );
	
	if ( wild )
	{
		len += snprintf( cmd, 4096, "ls -p " );
		len += copy_with_space_escapes( wild-path, cmd + len, path );
		len += snprintf( cmd+len, 4096-len, " | grep '%ls$'", wild+1 );
	}
	else
	{
		/* No wild card. We can directly get contents. */
		len = snprintf( cmd, 4096, "ls -p " );
		len += copy_with_space_escapes( 4096-len, cmd + len, path );
		len += snprintf( cmd+len, 4096-len, " | grep -v /" );
	}

	printf( "command = [%s]\n", cmd );
	
	f = popen( cmd, "r" );
	if ( f )
		return muse_generate_list( env, (muse_list_generator_t)generate_files, f );

	return MUSE_NIL;
}

/**
 * (list-folders parent-folder).
 * Returns a list of folder names for all the folders that exist under
 * the given parent folder.
 * For example:
 * @code
 * (list-folders "../")
 * @endcode
 * will list the folders above the current folder.
 * Note that the returned list only has the folder names and not
 * the full paths to the folders. Also, the folder names don't end
 * with '/' or any such path separator character.
 */
muse_cell fn_list_folders( muse_env *env, void *context, muse_cell args )
{
	FILE *f;
	char cmd[4096];
	int len = 0;
	const muse_char *path = _text_contents( _evalnext(&args), NULL );

	len = snprintf( cmd, 4096, "ls -p " );
	len += copy_with_space_escapes( 4096-len, cmd + len, path );
	len += snprintf( cmd+len, 4096-len, " | grep /" );

	f = popen( cmd, "r" );
	if ( f )
		return muse_generate_list( env, (muse_list_generator_t)generate_files, f );

	return MUSE_NIL;
}

#endif


/**
 * Finds the first position of the given character in the given string.
 * the return value will either point to the character or be 0 if the
 * end of string was reached without encountering the character.
 */
static const muse_char *findnext( const muse_char *str, muse_char c )
{
	while ( str[0] && str[0] != c ) ++str;
	return str;
}

struct separator_state_t {
	const muse_char *str;
	const muse_char *sep;
	const muse_char *currsep;
};

muse_cell fieldgen( muse_env *env, void *context, int i, muse_boolean *eol )
{
	struct separator_state_t *s = (struct separator_state_t*)context;
	
	if ( s->str[0] == 0 )
	{
		(*eol) = MUSE_TRUE;
		return MUSE_NIL;
	}
	
	(*eol) = MUSE_FALSE;
	
	{
		const muse_char *end = findnext( s->str, s->currsep[0] );
		muse_cell field = muse_mk_text(env, s->str, end );
		if ( end[0] ) s->str = end + 1; else s->str = end;
		
		/* Cycle through the seequence of separators given. */
		s->currsep++;
		if ( s->currsep[0] == 0 ) s->currsep = s->sep;
		
		return field;
	}
}


/**
 * (split "one;two;;three;" ";") -> ("one" "two" "" "three" "")
 * (split "one=1;two=2;three=3" "=;") -> ("one" "1" "two" "2" "three" "3")
 *
 * The first argument is the string to split and the second 
 * argument is a character separator. 
 *
 * You can define a recursive splitter like this -
 * @code
(define (split-rec str (sep . seps))
	(if seps
		(map (fn (s) (split-rec s seps)) (split str sep))
		(split str sep)))  
 * @endcode
 * so that @code (split-rec "a=1&b=2&c=3" '("&" "=")) @endcode
 * will give you @code (("a" "1") ("b" "2") ("c" "3")) @endcode.
 */
muse_cell fn_split( muse_env *env, void *context, muse_cell args )
{
	muse_cell strc = _evalnext(&args);
	muse_cell sep = _evalnext(&args);
	muse_cell result = MUSE_NIL;
	
	struct separator_state_t state;
	state.str = _text_contents(strc,NULL);
	state.sep = _text_contents(sep,NULL);
	state.currsep = state.sep;

	return muse_generate_list( env, fieldgen, &state );
}