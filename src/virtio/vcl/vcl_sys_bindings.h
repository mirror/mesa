#include <sys/mman.h>
#include <sys/socket.h>

enum map_result {
    FAILED = (long)MAP_FAILED,
};

static inline size_t cmsg_space(size_t size) {
    return CMSG_SPACE(size);
}

static inline struct cmsghdr *cmsg_firsthdr(struct msghdr *msgh) {
    return CMSG_FIRSTHDR(msgh);
}

static inline unsigned char *cmsg_data(struct cmsghdr *cmsgh) {
    return CMSG_DATA(cmsgh);
}
