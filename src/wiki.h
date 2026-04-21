#ifndef WIKI_H
#define WIKI_H

#include <stddef.h>

void handle_api_wiki_list(int client_fd);
void handle_api_wiki_read(int client_fd, const char *path_qs);
void handle_api_wiki_save(int client_fd, const char *body);
void handle_api_wiki_delete(int client_fd, const char *body);
void handle_api_wiki_search(int client_fd, const char *path_qs);
void handle_api_wiki_rebuild_html(int client_fd);
void handle_api_wiki_export_md_zip(int client_fd, const char *path_qs);
void handle_api_wiki_rename_article(int client_fd, const char *body);
void handle_api_wiki_rename_cat(int client_fd, const char *body);
void handle_api_wiki_delete_cat(int client_fd, const char *body);
void handle_api_wiki_move_article(int client_fd, const char *body);
void handle_api_wiki_mkdir(int client_fd, const char *body);
void handle_api_wiki_cleanup_uploads(int client_fd);
void handle_api_wiki_upload(int client_fd, const char *req_headers,
                             const char *body, size_t body_len);

#endif /* WIKI_H */
