#ifndef SVN_API_H
#define SVN_API_H

#include "platform.h"
void handle_api_svn_log(http_sock_t client_fd, const char *body);

#endif /* SVN_API_H */
