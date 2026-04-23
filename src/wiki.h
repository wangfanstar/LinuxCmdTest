#ifndef WIKI_H
#define WIKI_H

#include <stddef.h>

#include "platform.h"

void handle_api_wiki_list(http_sock_t client_fd);
void handle_api_wiki_refresh_index(http_sock_t client_fd);
void handle_api_wiki_read(http_sock_t client_fd, const char *path_qs);
void handle_api_wiki_save(http_sock_t client_fd, const char *body,
                          const char *actor, const char *ip);
void handle_api_wiki_delete(http_sock_t client_fd, const char *body);
void handle_api_wiki_search(http_sock_t client_fd, const char *path_qs);
void handle_api_wiki_rebuild_html(http_sock_t client_fd);
void handle_api_wiki_export_md_zip(http_sock_t client_fd, const char *path_qs);
void handle_api_wiki_export_pdf(http_sock_t client_fd, const char *body);
void handle_api_wiki_rename_article(http_sock_t client_fd, const char *body);
void handle_api_wiki_rename_cat(http_sock_t client_fd, const char *body);
void handle_api_wiki_delete_cat(http_sock_t client_fd, const char *body);
void handle_api_wiki_move_article(http_sock_t client_fd, const char *body);
void handle_api_wiki_mkdir(http_sock_t client_fd, const char *body);
void handle_api_wiki_cleanup_uploads(http_sock_t client_fd);
void handle_api_wiki_upload(http_sock_t client_fd, const char *req_headers,
                             const char *body, size_t body_len);

#endif /* WIKI_H */
