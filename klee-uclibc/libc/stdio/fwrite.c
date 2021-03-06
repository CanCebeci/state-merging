/* Copyright (C) 2004       Manuel Novoa III    <mjn3@codepoet.org>
 *
 * GNU Library General Public License (LGPL) version 2 or later.
 *
 * Dedicated to Toni.  See uClibc/DEDICATION.mjn3 for details.
 */

#include "_stdio.h"

libc_hidden_proto(fwrite_unlocked)

#ifdef __DO_UNLOCKED

size_t fwrite_unlocked(const void * __restrict ptr, size_t size,
						 size_t nmemb, register FILE * __restrict stream)
{
	__STDIO_STREAM_VALIDATE(stream);
        
        //klee_merge_disable(1);
        if (stream == stdout || stream == stderr) {
            return nmemb;
            /*
            if (klee_is_symbolic(size) || klee_is_symbolic(nmemb)) {
                //__stdio_fwrite("#", 2, stream);
                klee_merge_disable(0);
                return nmemb;
            }
            */
        }

	/* Note: If nmbem * size > SIZE_MAX then there is an application
	 * bug since no array can be larger than SIZE_MAX in size. */

	if ((__STDIO_STREAM_IS_NARROW_WRITING(stream)
		 || !__STDIO_STREAM_TRANS_TO_WRITE(stream, __FLAG_NARROW))
		&& size && nmemb
		) {

		if (nmemb <= (SIZE_MAX / size)) {
			size_t res =  __stdio_fwrite((const unsigned char *) ptr,
								  size*nmemb, stream) / size;
                        //klee_merge_disable(0);
                        return res;
		}

		__STDIO_STREAM_SET_ERROR(stream);
		__set_errno(EINVAL);
	}

        //klee_merge_disable(0);
	return 0;
}
libc_hidden_def(fwrite_unlocked)

#ifndef __UCLIBC_HAS_THREADS__
libc_hidden_proto(fwrite)
strong_alias(fwrite_unlocked,fwrite)
libc_hidden_def(fwrite)
#endif

#elif defined __UCLIBC_HAS_THREADS__

libc_hidden_proto(fwrite)
size_t fwrite(const void * __restrict ptr, size_t size,
			  size_t nmemb, register FILE * __restrict stream)
{
	size_t retval;
	__STDIO_AUTO_THREADLOCK_VAR;

	__STDIO_AUTO_THREADLOCK(stream);

	retval = fwrite_unlocked(ptr, size, nmemb, stream);

	__STDIO_AUTO_THREADUNLOCK(stream);

	return retval;
}
libc_hidden_def(fwrite)

#endif
