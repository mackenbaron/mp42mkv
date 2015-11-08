#ifndef __F7B3BFBCB9044A6698B16F8EA224FE15_H
#define __F7B3BFBCB9044A6698B16F8EA224FE15_H

#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>

#if defined(_MSC_VER)
#define __fseek64 _fseeki64
#define __ftell64  _ftelli64
#elif defined(__MINGW32__)
#define __fseek64 fseeko64
#define __ftell64  ftello64
#else
#define _POSIX_C_SOURCE 200809L
#define __fseek64 fseeko
#define __ftell64  ftello
#endif

#endif //__F7B3BFBCB9044A6698B16F8EA224FE15_H