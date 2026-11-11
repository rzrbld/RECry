#include "StdAfx.h"
#include <ILog.h>
#include <Stream.h>
#include "System.h"
#include "zlib/zlib.h"
#if defined(LINUX)
#include <unistd.h>
#else
#include <io.h>
#endif

// gzclose owns its descriptor. Duplicate it before releasing the FILE so
// stdio never retains or closes a descriptor that gzip has already closed.
static gzFile OpenCompressedFile(const char* filename, bool write)
{
#if defined(LINUX)
    // fxopen is plain stdio in this SDK; compressed saves bypass CryPak.
    std::string normalized(filename);
    for (size_t i = 0; i < normalized.size(); ++i)
        if (normalized[i] == '\\') normalized[i] = '/';
    filename = normalized.c_str();
#endif
    FILE* file = fxopen(filename, write ? "wb" : "rb");
    if (!file) return NULL;
#if defined(LINUX)
    int fd = dup(fileno(file));
#else
    int fd = _dup(_fileno(file));
#endif
    fclose(file);
    if (fd < 0) return NULL;
    gzFile stream = gzdopen(fd, write ? "wb9" : "rb");
    if (!stream) {
#if defined(LINUX)
        close(fd);
#else
        _close(fd);
#endif
    }
    return stream;
}

bool CSystem::WriteCompressedFile(char* filename, void* data, unsigned int bitlen)
{
    gzFile stream = OpenCompressedFile(filename, true);
    if (!stream) return false;
    unsigned int bytes = bitlen / 8 + ((bitlen & 7) != 0);
    bool ok = gzwrite(stream, &bitlen, sizeof(bitlen)) == sizeof(bitlen);
    if (ok) ok = gzwrite(stream, data, bytes) == (int)bytes;
    int result = gzclose(stream);
    return ok && result == Z_OK;
}

unsigned int CSystem::GetCompressedFileSize(char* filename)
{
    gzFile stream = OpenCompressedFile(filename, false);
    if (!stream) return 0;
    unsigned int bitlen = 0;
    int read = gzread(stream, &bitlen, sizeof(bitlen));
    int result = gzclose(stream);
    // CStream uses signed bit offsets. Reject corrupt sizes before allocation.
    return read == sizeof(bitlen) && result == Z_OK && bitlen <= 0x7fffffffU ? bitlen : 0;
}

unsigned int CSystem::ReadCompressedFile(char* filename, void* data, unsigned int maxbitlen)
{
    gzFile stream = OpenCompressedFile(filename, false);
    if (!stream) return 0;
    unsigned int bitlen = 0;
    bool ok = gzread(stream, &bitlen, sizeof(bitlen)) == sizeof(bitlen);
    if (ok) ok = bitlen <= maxbitlen && bitlen <= 0x7fffffffU;
    unsigned int bytes = bitlen / 8 + ((bitlen & 7) != 0);
    if (ok) ok = gzread(stream, data, bytes) == (int)bytes;
    int result = gzclose(stream);
    return ok && result == Z_OK ? bitlen : 0;
}
