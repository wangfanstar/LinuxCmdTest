#ifndef AUTH_DB_H
#define AUTH_DB_H

#include <stddef.h>

#include "platform.h"

typedef struct {
    int  logged_in;
    int  user_id;
    char username[64];
    char display_name[128];
    char role[16];   /* admin | author */
    char group_name[64];
} auth_user_t;

int auth_db_init(void);
void auth_db_close(void);

int auth_resolve_user_from_headers(const char *req_headers, auth_user_t *out_user);
int auth_require_author(const char *req_headers, http_sock_t fd, auth_user_t *out_user);
int auth_require_admin(const char *req_headers, http_sock_t fd, auth_user_t *out_user);

void auth_audit(const char *ip, const char *username, const char *action,
                const char *target, const char *detail);

void auth_md_backup(const char *article_id, const char *title, const char *category,
                    const char *content, const char *html, const char *editor,
                    const char *ip);

void handle_api_wiki_login(http_sock_t fd, const char *req_headers, const char *body, const char *ip);
void handle_api_wiki_logout(http_sock_t fd, const char *req_headers);
void handle_api_wiki_auth_status(http_sock_t fd, const char *req_headers);
void handle_api_wiki_users_list(http_sock_t fd);
void handle_api_wiki_user_save(http_sock_t fd, const char *body);
void handle_api_wiki_user_delete(http_sock_t fd, const char *body);
void handle_api_wiki_audit_logs(http_sock_t fd, const char *path_qs);
void handle_api_wiki_md_history(http_sock_t fd, const char *path_qs);
void handle_api_wiki_user_article_rank(http_sock_t fd, const char *path_qs);

#endif
