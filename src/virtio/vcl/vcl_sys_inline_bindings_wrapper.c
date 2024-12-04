#include "vcl_sys_inline_bindings_wrapper.h"
#include <sys/mman.h>
#include <sys/socket.h>

struct cmsghdr *cmsg_firsthdr(struct msghdr *msgh) {
    return CMSG_FIRSTHDR(msgh);
}

unsigned char *cmsg_data(struct cmsghdr *cmsgh) {
    return CMSG_DATA(cmsgh);
}
