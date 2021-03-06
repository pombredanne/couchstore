/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include <assert.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>

#include "internal.h"

#undef LOG_IO
#ifdef LOG_IO
#include <stdio.h>
#endif

static void save_errno(couchstore_error_info_t *errinfo) {
    if (errinfo) {
        errinfo->error = errno;
    }
}

static int handle_to_fd(couch_file_handle handle)
{
    return (int)(intptr_t)handle;
}

static couch_file_handle fd_to_handle(int fd)
{
    return (couch_file_handle)(intptr_t)fd;
}

static ssize_t couch_pread(couchstore_error_info_t *errinfo,
                           couch_file_handle handle,
                           void *buf,
                           size_t nbyte,
                           cs_off_t offset)
{
#ifdef LOG_IO
    fprintf(stderr, "PREAD  %8llx -- %8llx  (%6.1f kbytes)\n", offset, offset+nbyte, nbyte/1024.0);
#endif
    int fd = handle_to_fd(handle);
    ssize_t rv;
    do {
        rv = pread(fd, buf, nbyte, offset);
    } while (rv == -1 && errno == EINTR);

    if (rv < 0) {
        save_errno(errinfo);
        return (ssize_t) COUCHSTORE_ERROR_READ;
    }
    return rv;
}

static ssize_t couch_pwrite(couchstore_error_info_t *errinfo,
                            couch_file_handle handle,
                            const void *buf,
                            size_t nbyte,
                            cs_off_t offset)
{
#ifdef LOG_IO
    fprintf(stderr, "PWRITE %8llx -- %8llx  (%6.1f kbytes)\n", offset, offset+nbyte, nbyte/1024.0);
#endif
    int fd = handle_to_fd(handle);
    ssize_t rv;
    do {
        rv = pwrite(fd, buf, nbyte, offset);
    } while (rv == -1 && errno == EINTR);

    if (rv < 0) {
        save_errno(errinfo);
        return (ssize_t) COUCHSTORE_ERROR_WRITE;
    }
    return rv;
}

static couchstore_error_t couch_open(couchstore_error_info_t *errinfo,
                                     couch_file_handle* handle,
                                     const char *path,
                                     int oflag)
{
    int fd;
    do {
        fd = open(path, oflag | O_LARGEFILE, 0666);
    } while (fd == -1 && errno == EINTR);

    if (fd < 0) {
        save_errno(errinfo);
        if (errno == ENOENT) {
            return COUCHSTORE_ERROR_NO_SUCH_FILE;
        } else {
            return COUCHSTORE_ERROR_OPEN_FILE;
        }
    }
    /* Tell the caller about the new handle (file descriptor) */
    *handle = fd_to_handle(fd);
    return COUCHSTORE_SUCCESS;
}

static void couch_close(couchstore_error_info_t *errinfo,
                        couch_file_handle handle)
{
    int fd = handle_to_fd(handle);
    int rv = 0;

    if (fd != -1) {
        do {
            assert(fd >= 3);
            rv = close(fd);
        } while (rv == -1 && errno == EINTR);
    }
    if (rv < 0) {
        save_errno(errinfo);
    }
}

static cs_off_t couch_goto_eof(couchstore_error_info_t *errinfo,
                               couch_file_handle handle)
{
    int fd = handle_to_fd(handle);
    cs_off_t rv = lseek(fd, 0, SEEK_END);
    if (rv < 0) {
        save_errno(errinfo);
    }
    return rv;
}


static couchstore_error_t couch_sync(couchstore_error_info_t *errinfo,
                                     couch_file_handle handle)
{
    int fd = handle_to_fd(handle);
    int rv;
    do {
        rv = fdatasync(fd);
    } while (rv == -1 && errno == EINTR);

    if (rv == -1) {
        save_errno(errinfo);
        return COUCHSTORE_ERROR_WRITE;
    }

    return COUCHSTORE_SUCCESS;
}

static couch_file_handle couch_constructor(couchstore_error_info_t *errinfo,
                                           void* cookie)
{
    (void) cookie;
    (void)errinfo;
    /*
    ** We don't have a file descriptor till couch_open runs, so return
    ** an invalid value for now.
    */
    return fd_to_handle(-1);
}

static void couch_destructor(couchstore_error_info_t *errinfo,
                             couch_file_handle handle)
{
    /* nothing to do here */
    (void)handle;
    (void)errinfo;
}

static couchstore_error_t couch_advise(couchstore_error_info_t *errinfo,
                                       couch_file_handle handle,
                                       cs_off_t offset,
                                       cs_off_t len,
                                       couchstore_file_advice_t advice)
{
#ifdef POSIX_FADV_NORMAL
    int fd = handle_to_fd(handle);
    int error = posix_fadvise(fd, offset, len, (int) advice);
    if (error != 0) {
        save_errno(errinfo);
    }
    switch(error) {
        case EINVAL:
        case ESPIPE:
            return COUCHSTORE_ERROR_INVALID_ARGUMENTS;
            break;
        case EBADF:
            return COUCHSTORE_ERROR_OPEN_FILE;
            break;
    }
#else
    (void) handle; (void)offset; (void)len; (void)advice;
    (void)errinfo;
#endif
    return COUCHSTORE_SUCCESS;
}

static const couch_file_ops default_file_ops = {
    (uint64_t)5,
    couch_constructor,
    couch_open,
    couch_close,
    couch_pread,
    couch_pwrite,
    couch_goto_eof,
    couch_sync,
    couch_advise,
    couch_destructor,
    NULL
};

LIBCOUCHSTORE_API
const couch_file_ops *couchstore_get_default_file_ops(void)
{
    return &default_file_ops;
}
