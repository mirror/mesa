#include <sys/mman.h>
#include <sys/socket.h>

size_t cmsg_space(size_t size);

struct cmsghdr *cmsg_firsthdr(struct msghdr *msgh);

unsigned char *cmsg_data(struct cmsghdr *cmsgh);
