#include "vcl_sys_inline_bindings_wrapper.h"
#include <sys/mman.h>
#include <sys/socket.h>

size_t cmsg_space(size_t size) {
    return CMSG_SPACE(size);
}

struct cmsghdr *cmsg_firsthdr(struct msghdr *msgh) {
    return CMSG_FIRSTHDR(msgh);
}

unsigned char *cmsg_data(struct cmsghdr *cmsgh) {
    return CMSG_DATA(cmsgh);
}
