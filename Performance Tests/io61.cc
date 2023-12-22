#include "io61.hh"
#include <climits>
#include <cerrno>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <sys/types.h>
#include <sys/stat.h>
#include <iostream>
#include <map>
#include <thread>
#include <sys/mman.h> 
#include <fcntl.h>

// io61.cc
//    YOUR CODE HERE!


// io61_file
//    Data structure for io61 file wrappers.

struct io61_file {
    int fd = -1;                                // file descriptor
    int mode;                                   // O_RDONLY, O_WRONLY, or O_RDWR
    bool seekable;                              // is this file seekable?
    size_t size;

    // Single-slot cache
    static constexpr off_t cbufsz = 8192;
    unsigned char cbuf[cbufsz];
    off_t tag;                                  // offset of first character in `cbuf`
    off_t pos_tag;                              // next offset to read or write (non-positioned mode)
    off_t end_tag;                              // offset one past last valid character in `cbuf`

    // Memory-mapped IO
    char* map = nullptr;                        // memory-mapped IO
    bool is_seq = true;                         // if access pattern is sequential

    // Positioned mode
    std::atomic<bool> dirty = false;            // has cache been written?
    bool positioned = false;                    // is cache in positioned mode?

    // Synchronisation
    static const size_t nchunks = 1 << 18;      // number of chunks the file is divided into
    off_t chunk_sz;                             // how big is each lockable block?
    std::mutex m;                               // for accessing array containing locked regions
    std::condition_variable_any cv;             // for blocking
    int nlocked[nchunks];                       // number of times each chunk is locked
    std::thread::id owner[nchunks];             // pid of the process that owns the lock
};

void io61_check_assertions(io61_file* f)
{
    assert(f->tag <= f->pos_tag && f->pos_tag <= f->end_tag);
    if (f->map == MAP_FAILED) assert(f->end_tag - f->pos_tag <= f->cbufsz);
    if (f->mode == O_WRONLY) assert(f->pos_tag == f->end_tag); 
}


// io61_fdopen(fd, mode)
//    Returns a new io61_file for file descriptor `fd`. `mode` is either
//    O_RDONLY for a read-only file, O_WRONLY for a write-only file,
//    or O_RDWR for a read/write file.

io61_file* io61_fdopen(int fd, int mode) {
    assert(fd >= 0);
    assert((mode & O_APPEND) == 0);
    io61_file* f = new io61_file;
    f->fd = fd;
    f->mode = mode & O_ACCMODE;
    off_t off = lseek(fd, 0, SEEK_CUR);
    if (off != -1) {
        f->seekable = true;
        f->tag = f->pos_tag = f->end_tag = off;
    } else {
        f->seekable = false;
        f->tag = f->pos_tag = f->end_tag = 0;
    }
    f->size = io61_filesize(f); 
    f->dirty = f->positioned = false;

    // calculate chunk size
    f->chunk_sz = (off_t) (io61_filesize(f) / f->nchunks); 
    if (f->chunk_sz < 16) f->chunk_sz = 16;

    // initialise nlocked to 0 since initially nothing is locked
    for (int i = 0; i < (int) f->nchunks; i++) f->nlocked[i] = 0; 
    return f;
}


// io61_close(f)
//    Closes the io61_file `f` and releases all its resources.

int io61_close(io61_file* f) {
    io61_flush(f);
    int r = close(f->fd);
    munmap(f->map, (f->size > 0) ? f->size : f->cbufsz);
    delete f;
    return r;
}


// NORMAL READING AND WRITING FUNCTIONS

// io61_readc(f)
//    Reads a single (unsigned) byte from `f` and returns it. Returns EOF,
//    which equals -1, on end of file or error.

int io61_fill(io61_file* f);

int io61_readc(io61_file* f) {
    // coarse-grained locking to ensure only one process can read from/write to the file at a given time
    std::unique_lock<std::mutex> lg(f->m); 

    io61_check_assertions(f); 

    if (f->map == nullptr)
    {
        f->map = (char*) mmap(nullptr, f->size, PROT_READ, MAP_PRIVATE, f->fd, 0); 
        if (f->map != MAP_FAILED)
        {
            f->tag = 0; 
            f->end_tag = f->size; 
            if (f->is_seq) posix_fadvise(f->fd, 0, f->size, POSIX_FADV_SEQUENTIAL);
        }
    }

    if (f->map == MAP_FAILED)
    {
        // non-mappable file
        if (f->pos_tag == f->end_tag)
        {
            if(io61_fill(f) < 0 || f->pos_tag == f->end_tag) return -1; 
        }

        unsigned char ch = f->cbuf[f->pos_tag - f->tag]; 
        f->pos_tag++; 
        io61_check_assertions(f); 
        return ch; 
    }

    // mappable file
    if (f->pos_tag < f->end_tag)
    {
        f->pos_tag++; 
        io61_check_assertions(f); 
        return (unsigned char) f->map[f->pos_tag-1]; 
    }else return -1; 
}


// io61_read(f, buf, sz)
//    Reads up to `sz` bytes from `f` into `buf`. Returns the number of
//    bytes read on success. Returns 0 if end-of-file is encountered before
//    any bytes are read, and -1 if an error is encountered before any
//    bytes are read.
//
//    Note that the return value might be positive, but less than `sz`,
//    if end-of-file or error is encountered before all `sz` bytes are read.
//    This is called a Ã¢â‚¬Å“short read.Ã¢â‚¬Â

ssize_t io61_read_cache(io61_file* f, unsigned char* buf, size_t sz) {
    io61_check_assertions(f); 

    size_t nread = 0; 
    while (nread < sz)
    {
        if (f->pos_tag == f->end_tag)
        {
            //buffer refill
            if (io61_fill(f) == -1 || f->pos_tag == f->end_tag) break; 
        }
        size_t curr_read = std::min((size_t) (f->end_tag - f->pos_tag), (size_t) (sz - nread)); 
        memcpy(&buf[nread], &f->cbuf[f->pos_tag - f->tag], curr_read); 
        f->pos_tag += curr_read; 
        nread += curr_read; 
    }

    if (nread != 0 || sz == 0 || errno == 0) {
        return nread;
    } else {
        return -1;
    }
}

ssize_t io61_read(io61_file* f, unsigned char* buf, size_t sz) {
    // coarse-grained locking to ensure only one process can read from/write to the file at a given time
    std::unique_lock<std::mutex> lg(f->m); 

    if (f->map == nullptr)
    {
        f->map = (char*) mmap(nullptr, f->size, PROT_READ, MAP_PRIVATE, f->fd, 0); 
        if (f->map != MAP_FAILED)
        {
            f->tag = 0; 
            f->end_tag = f->size; 
            if (f->is_seq) posix_fadvise(f->fd, 0, f->size, POSIX_FADV_SEQUENTIAL);
        }
    }

    if (f->map == MAP_FAILED) return io61_read_cache(f, buf, sz); 

    // Copy `sz` number of bytes from `f` into `buf`
    size_t nread = std::min(sz, (size_t) (f->size - f->pos_tag)); 
    memcpy(buf, &f->map[f->pos_tag], nread); 
    f->pos_tag += nread; 
    return nread; 
}


// io61_writec(f)
//    Write a single character `c` to `f` (converted to unsigned char).
//    Returns 0 on success and -1 on error.

int io61_writec(io61_file* f, int c) {
    // coarse-grained locking to ensure only one process can read from/write to the file at a given time
    std::unique_lock<std::mutex> lg(f->m); 

    io61_check_assertions(f); 

    assert(!f->positioned);
    if (f->end_tag == f->tag + f->cbufsz)
    {
        if (io61_flush(f) < 0) return -1; 
    }

    f->cbuf[f->pos_tag - f->tag] = c; 

    f->pos_tag++; 
    f->end_tag++; 
    f->dirty = true; 

    io61_check_assertions(f); 
    return 0; 
}


// io61_write(f, buf, sz)
//    Writes `sz` characters from `buf` to `f`. Returns `sz` on success.
//    Can write fewer than `sz` characters when there is an error, such as
//    a drive running out of space. In this case io61_write returns the
//    number of characters written, or -1 if no characters were written
//    before the error occurred.

ssize_t io61_write(io61_file* f, const unsigned char* buf, size_t sz) {
    // coarse-grained locking to ensure only one process can read from/write to the file at a given time
    std::unique_lock<std::mutex> lg(f->m); 

    io61_check_assertions(f);
    assert(!f->positioned);

    size_t nwritten = 0; 
    // nwritten = write(f->fd, buf, sz); 
    while (nwritten < sz)
    {
        if (f->end_tag == f->tag + f->cbufsz)
        {
            // flush buffer
            if (io61_flush(f) == -1) break; 
        }

        size_t curr_write = std::min((size_t) (f->cbufsz + f->tag - f->pos_tag), (size_t) (sz-nwritten)); 
        if (curr_write == 0) break; 
        memcpy(&f->cbuf[f->pos_tag - f->tag], &buf[nwritten], curr_write); 
        nwritten += curr_write; 
        f->pos_tag += curr_write; 
        f->end_tag += curr_write; 
        f->dirty = true; 
    }

    if (nwritten != 0 || sz == 0) {
        return nwritten;
    } else {
        return -1;
    }
}


// io61_flush(f)
//    If `f` was opened for writes, `io61_flush(f)` forces a write of any
//    cached data written to `f`. Returns 0 on success; returns -1 if an error
//    is encountered before all cached data was written.
//
//    If `f` was opened read-only and is seekable, `io61_flush(f)` drops any
//    data cached for reading and seeks to the logical file position.

static int io61_flush_dirty(io61_file* f);
static int io61_flush_dirty_positioned(io61_file* f);
static int io61_flush_clean(io61_file* f);

int io61_flush(io61_file* f) {
    if (f->dirty && f->positioned) {
        return io61_flush_dirty_positioned(f);
    } else if (f->dirty) {
        return io61_flush_dirty(f);
    } else {
        return io61_flush_clean(f);
    }
}


// io61_seek(f, off)
//    Changes the file pointer for file `f` to `off` bytes into the file.
//    Returns 0 on success and -1 on failure.

int io61_seek(io61_file* f, off_t off) {
    io61_check_assertions(f); 

    if (f->mode == O_RDONLY && f->tag <= off && f->end_tag > off)
    {
        f->pos_tag = off; 
        return 0; 
    }

    if (f->mode == O_WRONLY) if (io61_flush(f) < 0) return -1; 

    // update kernal and IO position
    off_t r = lseek(f->fd, (off_t) off, SEEK_SET);
    if (r == -1) return -1; 
    f->pos_tag = off; 
    f->positioned = false; 

    if (f->mode == O_WRONLY)
    {
        // don't consider mapping if write only
        f->tag = f->end_tag = off; 
        return 0; 
    }

    // see if file is mappable
    if (f->map == nullptr)
    {
        f->map = (char*) mmap(nullptr, f->size, PROT_READ, MAP_PRIVATE, f->fd, 0); 
        if (f->map != nullptr)
        {
            // mappable file
            f->tag = 0; 
            f->end_tag = f->size; 
        }
    }

    if (f->map == MAP_FAILED)
    {
        // file not mappable
        f->tag = f->end_tag = off; 
        return 0; 
    }
    
    if (f->is_seq) posix_fadvise(f->fd, 0, f->size, POSIX_FADV_NORMAL); 
    f->is_seq = false; 
    return 0; 
}


// Helper functions

// io61_fill(f)
//    Fill the cache by reading from the file. Returns 0 on success,
//    -1 on error. Used only for non-positioned files.

int io61_fill(io61_file* f) {
    f->tag = f->pos_tag = f->end_tag; 

    ssize_t nr;
    while (true) {
        nr = read(f->fd, f->cbuf, f->cbufsz);
        if (nr >= 0) {
            break;
        } else if (errno != EINTR && errno != EAGAIN) {
            return -1;
        }
    }
    f->end_tag += nr;
    return 0;
}


// io61_flush_*(f)
//    Helper functions for io61_flush.

static int io61_flush_dirty(io61_file* f) {
    // Called when `f`Ã¢â‚¬â„¢s cache is dirty and not positioned.
    // Uses `write`; assumes that the initial file position equals `f->tag`.
    off_t flush_tag = f->tag;
    while (flush_tag != f->end_tag) {
        ssize_t nw = write(f->fd, &f->cbuf[flush_tag - f->tag],
                           f->end_tag - flush_tag);
        if (nw >= 0) {
            flush_tag += nw;
        } else if (errno != EINTR && errno != EINVAL) {
            return -1;
        }
    }
    f->dirty = false;
    f->tag = f->pos_tag = f->end_tag;
    return 0;
}

static int io61_flush_dirty_positioned(io61_file* f) {
    // Called when `f`Ã¢â‚¬â„¢s cache is dirty and positioned.
    // Uses `pwrite`; does not change file position.
    off_t flush_tag = f->tag;
    while (flush_tag != f->end_tag) {
        ssize_t nw = pwrite(f->fd, &f->cbuf[flush_tag - f->tag],
                            f->end_tag - flush_tag, flush_tag);
        if (nw >= 0) {
            flush_tag += nw;
        } else if (errno != EINTR && errno != EINVAL) {
            return -1;
        }
    }
    f->dirty = false;
    return 0;
}

static int io61_flush_clean(io61_file* f) {
    // Called when `f`Ã¢â‚¬â„¢s cache is clean.
    if (!f->positioned && f->seekable) {
        if (lseek(f->fd, f->pos_tag, SEEK_SET) == -1) {
            return -1;
        }
        f->tag = f->end_tag = f->pos_tag;
    }
    return 0;
}



// POSITIONED I/O FUNCTIONS

// io61_pread(f, buf, sz, off)
//    Read up to `sz` bytes from `f` into `buf`, starting at offset `off`.
//    Returns the number of characters read or -1 on error.
//
//    This function can only be called when `f` was opened in read/write
//    more (O_RDWR).

static int io61_pfill(io61_file* f, off_t off);

ssize_t io61_pread(io61_file* f, unsigned char* buf, size_t sz,
                   off_t off) {
    // coarse-grained locking to ensure only one process can read from/write to the file at a given time
    std::unique_lock<std::mutex> lg(f->m); 

    if (!f->positioned || off < f->tag || off >= f->end_tag) {
        if (io61_pfill(f, off) == -1) {
            return -1;
        }
    }
    size_t nleft = f->end_tag - off;
    size_t ncopy = std::min(sz, nleft);
    memcpy(buf, &f->cbuf[off - f->tag], ncopy);
    return ncopy;
}


// io61_pwrite(f, buf, sz, off)
//    Write up to `sz` bytes from `buf` into `f`, starting at offset `off`.
//    Returns the number of characters written or -1 on error.
//
//    This function can only be called when `f` was opened in read/write
//    more (O_RDWR).

ssize_t io61_pwrite(io61_file* f, const unsigned char* buf, size_t sz,
                    off_t off) {
    // coarse-grained locking to ensure only one process can read from/write to the file at a given time
    std::unique_lock<std::mutex> lg(f->m); 

    if (!f->positioned || off < f->tag || off >= f->end_tag) {
        if (io61_pfill(f, off) == -1) {
            return -1;
        }
    }
    size_t nleft = f->end_tag - off;
    size_t ncopy = std::min(sz, nleft);
    memcpy(&f->cbuf[off - f->tag], buf, ncopy);
    f->dirty = true;
    return ncopy;
}


// io61_pfill(f, off)
//    Fill the single-slot cache with data including offset `off`.
//    The handout code rounds `off` down to a multiple of 8192.

static int io61_pfill(io61_file* f, off_t off) {
    assert(f->mode == O_RDWR);
    if (f->dirty && io61_flush(f) == -1) {
        return -1;
    }

    off = off - (off % 8192);
    ssize_t nr = pread(f->fd, f->cbuf, f->cbufsz, off);
    if (nr == -1) {
        return -1;
    }
    f->tag = off;
    f->end_tag = off + nr;
    f->positioned = true;
    return 0;
}

bool is_overlap(io61_file* f, off_t start, off_t len)
{
    for (int i = start / f->chunk_sz; i * f->chunk_sz < start + len; i++)
        if (f->nlocked[i] && f->owner[i] != std::this_thread::get_id()) return 1; 
    
    return 0; 
}

void lock_region(io61_file* f, off_t start, off_t len)
{
    for (int i = start / f->chunk_sz; i * f->chunk_sz < start + len; i++)
    {
        assert(f->nlocked[i] == 0 || f->owner[i] == std::this_thread::get_id());
        f->nlocked[i]++; 
        f->owner[i] = std::this_thread::get_id(); 
    }
}

// FILE LOCKING FUNCTIONS

// io61_try_lock(f, start, len, locktype)
//    Attempts to acquire a lock on offsets `[start, len)` in file `f`.
//    `locktype` must be `LOCK_SH`, which requests a shared lock,
//    or `LOCK_EX`, which requests an exclusive lock.
//
//    Returns 0 if the lock was acquired and -1 if it was not. Does not
//    block: if the lock cannot be acquired, it returns -1 right away.

int io61_try_lock(io61_file* f, off_t start, off_t len, int locktype) {
    assert(start >= 0 && len >= 0);
    assert(locktype == LOCK_EX || locktype == LOCK_SH);
    if (len == 0) return 0;
    
    // try to grab the lock
    std::unique_lock<std::mutex> lg(f->m); 

    // lock acquired
    // first check if entire region can be locked
    if (is_overlap(f, start, len)) return -1; 

    // entire region is free
    lock_region(f, start, len); 
    return 0; 
}


// io61_lock(f, start, len, locktype)
//    Acquire a lock on offsets `[start, len)` in file `f`.
//    `locktype` must be `LOCK_SH`, which requests a shared lock,
//    or `LOCK_EX`, which requests an exclusive lock.
//
//    Returns 0 if the lock was acquired and -1 on error. Blocks until
//    the lock can be acquired; the -1 return value is reserved for true
//    error conditions, such as EDEADLK (a deadlock was detected).

int io61_lock(io61_file* f, off_t start, off_t len, int locktype) {
    assert(start >= 0 && len >= 0);
    assert(locktype == LOCK_EX || locktype == LOCK_SH);
    if (len == 0) return 0; 

    // try to grab the lock
    std::unique_lock<std::mutex> lg(f->m); 

    // repeated check if the entire region can be locked
    while (is_overlap(f, start, len)) f->cv.wait(lg); 

    // entire region is free
    lock_region(f, start, len); 
    return 0;
}


// io61_unlock(f, start, len)
//    Release the lock on offsets `[start,len)` in file `f`.
//    Returns 0 on success and -1 on error.

int io61_unlock(io61_file* f, off_t start, off_t len) {
    assert(start >= 0 && len >= 0);
    if (len == 0) return 0;
    
    // try to grab the lock
    std::unique_lock<std::mutex> lg(f->m); 

    // lock acquired; remove all locks
    for (int i = start / f->chunk_sz; i * f->chunk_sz < start + len; i++)
    {
        assert(f->nlocked[i] && f->owner[i] == std::this_thread::get_id()); 
        f->nlocked[i]--; 
    }

    // notify changes
    f->cv.notify_all(); 
    return 0; 
}



// HELPER FUNCTIONS
// You shouldn't need to change these functions.

// io61_open_check(filename, mode)
//    Opens the file corresponding to `filename` and returns its io61_file.
//    If `!filename`, returns either the standard input or the
//    standard output, depending on `mode`. Exits with an error message if
//    `filename != nullptr` and the named file cannot be opened.

io61_file* io61_open_check(const char* filename, int mode) {
    int fd;
    if (filename) {
        fd = open(filename, mode, 0666);
    } else if ((mode & O_ACCMODE) == O_RDONLY) {
        fd = STDIN_FILENO;
    } else {
        fd = STDOUT_FILENO;
    }
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", filename, strerror(errno));
        exit(1);
    }
    return io61_fdopen(fd, mode & O_ACCMODE);
}


// io61_fileno(f)
//    Returns the file descriptor associated with `f`.

int io61_fileno(io61_file* f) {
    return f->fd;
}


// io61_filesize(f)
//    Returns the size of `f` in bytes. Returns -1 if `f` does not have a
//    well-defined size (for instance, if it is a pipe).

off_t io61_filesize(io61_file* f) {
    struct stat s;
    int r = fstat(f->fd, &s);
    if (r >= 0 && S_ISREG(s.st_mode)) {
        return s.st_size;
    } else {
        return -1;
    }
}