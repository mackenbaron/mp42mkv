#ifndef __6D231C8973C94E6AADEE61151118FA55_H
#define __6D231C8973C94E6AADEE61151118FA55_H

#include "io_spec.h"
#include "mp4/Internal/IsomStreamSource.h"

class InputFile : public Isom::IStreamSource
{
	FILE* _file;
	int64_t _size;

public:
	InputFile() throw() : _file(nullptr), _size(0) {}
	virtual ~InputFile() throw() { if (_file) fclose(_file); }

public:
	bool Open(const char* file_path) throw()
	{
		if (file_path == nullptr)
			return false;
		_file = fopen(file_path, "rb");
		if (_file == nullptr)
			return false;

		__fseek64(_file, 0, SEEK_END);
		_size = __ftell64(_file);
		__fseek64(_file, 0, SEEK_SET);
		return _size > 0;
	}

	unsigned Read(void* buf, unsigned size)
	{ return fread(buf, 1, size, _file); }
	bool Seek(int64_t pos)
	{ return __fseek64(_file, pos, SEEK_SET) != -1; }
	int64_t Tell()
	{ return __ftell64(_file); }
	int64_t GetSize() { return _size; }
};

#endif //__6D231C8973C94E6AADEE61151118FA55_H