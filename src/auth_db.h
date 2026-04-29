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
void auth_audit_txn(const char *ip, const char *username, const char *action,
                    const char *target, const char *detail, const char *save_txn_id);

void auth_md_backup(const char *article_id, const char *title, const char *category,
                    const char *content, const char *html, const char *editor,
                    const char *ip);
void auth_md_backup_txn(const char *article_id, const char *title, const char *category,
                        const char *content, const char *html, const char *editor,
                        const char *ip, const char *save_txn_id);
void auth_gen_save_txn_id(char *out, size_t outsz);

void handle_api_wiki_login(http_sock_t fd, const char *req_headers, const char *body, const char *ip);
void handle_api_wiki_logout(http_sock_t fd, const char *req_headers);
void handle_api_wiki_auth_status(http_sock_t fd, const char *req_headers);
void handle_api_wiki_users_list(http_sock_t fd);
void handle_api_wiki_user_save(http_sock_t fd, const char *body);
void handle_api_wiki_user_delete(http_sock_t fd, const char *body);
void handle_api_wiki_audit_logs(http_sock_t fd, const char *path_qs);
void handle_api_wiki_md_history(http_sock_t fd, const char *path_qs);
void handle_api_wiki_user_article_rank(http_sock_t fd, const char *path_qs);

/* Markdown 文章元数据（无首行 META 的 md 依赖 SQLite 记录作者与时间） */
typedef struct {
    char title[512];
    char category[512];
    char created[80];
    char updated[80];
    char last_author[128];
    char authors_json[2048];
    int  found;
} wiki_md_meta_row_t;

int auth_wiki_md_meta_get(const char *article_id, wiki_md_meta_row_t *out);
int auth_wiki_md_meta_ensure_scan_plain(const char *article_id, const char *category,
                                        const char *title, const char *iso_now);
int auth_wiki_md_meta_upsert_scan_meta(const char *article_id, const char *title,
                                       const char *category, const char *created,
                                       const char *updated);
int auth_wiki_md_meta_on_editor_save(const char *article_id, const char *title,
                                     const char *category, const char *updated_iso,
                                     const char *editor_username);
int auth_wiki_md_meta_update_category(const char *article_id, const char *new_category,
                                      const char *updated_iso);
void auth_wiki_md_meta_delete(const char *article_id);

/* NoteWiki 侧栏「文件夹筛选」偏好（按用户名存 SQLite） */
int auth_notewiki_prefs_get(const char *username, char *folder_filter_json_out, size_t cap);
int auth_notewiki_prefs_set(const char *username, const char *folder_filter_json);

#endif
