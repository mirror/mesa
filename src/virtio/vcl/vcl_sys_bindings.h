#include <sys/mman.h>
#include <sys/socket.h>

enum MapResult : long {
   FAILED = (long)MAP_FAILED,
};

size_t CMSG_SPACE_SIZEOF_INT = CMSG_SPACE(sizeof(int));

struct cmsghdr *cmsg_firsthdr(struct msghdr *msgh);

unsigned char *cmsg_data(struct cmsghdr *cmsgh);
