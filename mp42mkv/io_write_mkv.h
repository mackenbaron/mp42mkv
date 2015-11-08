#ifndef __6D231C8973C94E6AADEE61151118FA56_H
#define __6D231C8973C94E6AADEE61151118FA56_H

#include "io_spec.h"
#include "mkv/AkaMatroskaIOCallback.h"
//#include <malloc.h>
//#include <memory.h>

class OutputFile : public AkaMatroska::Core::IOCallback
{
	FILE* _file;

	unsigned char* _buf;
	unsigned _buf_size;
	unsigned _offset;

public:
	OutputFile(unsigned buf_size = 524288) throw() : _file(nullptr), _buf(nullptr), _buf_size(buf_size), _offset(0) {}
	virtual ~OutputFile() throw() { FlushBuffer(); if (_file) fclose(_file); free(_buf); }

public:
	bool Open(const char* file_path) throw()
	{
		_file = fopen(file_path, "wb");
		return _file != nullptr;
	}

	unsigned Write(const void* buf, unsigned size)
	{
		if (size == 0)
			return 0;
		if (!InitBuffer())
			return 0;

		unsigned result = 0;
		if (size + _offset > _buf_size)
		{
			FlushBuffer();
			result = fwrite(buf, 1, size, _file);
		}else{
			if (size > 8)
				memcpy(_buf + _offset, buf, size);
			else
				CopyBytesValue(_buf + _offset, (unsigned char*)buf, size);
			_offset += size;
			result = size;
		}
		return result;
	}

	int64_t SetPosition(int64_t offset)
	{
		FlushBuffer();
		if (__fseek64(_file, offset, SEEK_SET) == -1)
			return -1;
		return __ftell64(_file);
	}

	int64_t GetPosition()
	{ return __ftell64(_file) + _offset; }

private:
	bool InitBuffer()
	{
		if (_buf != nullptr)
			return true;
		_buf = (decltype(_buf))malloc(_buf_size / 4 * 4 + 8);
		if (_buf == nullptr)
			return false;
		memset(_buf, 0, _buf_size);
		return true;
	}

	void FlushBuffer()
	{
		if (_file != nullptr && _offset > 0)
			fwrite(_buf, 1, _offset, _file);
		_offset = 0;
	}

	void CopyBytesValue(unsigned char* dst, unsigned char* src, unsigned size)
	{
		if (size == 1) {
			*dst = *src;
		}else{
			for (unsigned i = 0; i < size; i++)
				dst[i] = src[i];
		}
	}
};

#endif //__6D231C8973C94E6AADEE61151118FA56_H