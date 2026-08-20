#include "http_handler.h"
#include "http_utils.h"
#include "log.h"
#include "svn_api.h"
#include "register_api.h"
#include "wiki.h"
#include "auth_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#include <arpa/inet.h>
#endif
#include <errno.h>
#ifndef _WIN32
#include <signal.h>
#endif
#include <dirent.h>
#include <ctype.h>

#define MAX_BODY_SIZE        (64 * 1024)
#define SAVE_REPORT_MAX_BODY (50 * 1024 * 1024)

static int is_wiki_write_api(const char *path)
{
    return strcmp(path, "/api/wiki-save") == 0 ||
           strcmp(path, "/api/wiki-delete") == 0 ||
           strcmp(path, "/api/wiki-rename-article") == 0 ||
           strcmp(path, "/api/wiki-rename-cat") == 0 ||
           strcmp(path, "/api/wiki-delete-cat") == 0 ||
           strcmp(path, "/api/wiki-move-article") == 0 ||
           strcmp(path, "/api/wiki-mkdir") == 0 ||
           strcmp(path, "/api/wiki-upload") == 0 ||
           strcmp(path, "/api/wiki-rebuild-html") == 0 ||
           strcmp(path, "/api/wiki-cleanup-uploads") == 0 ||
           strcmp(path, "/api/wiki-trash-restore") == 0 ||
           strcmp(path, "/api/wiki-trash-empty") == 0 ||
           strcmp(path, "/api/wiki-restore-version") == 0;
}

/* ── 主处理入口 ──────────────────────────────────────────────── */

void handle_client(http_sock_t client_fd, struct sockaddr_in *addr)
{
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));
    int  client_port = ntohs(addr->sin_port);

    clock_t t_start = clock();

    /* 读取请求头（最多 8KB） */
    char req_buf[8192];
    memset(req_buf, 0, sizeof(req_buf));

    ssize_t total = 0, n;
    while (total < (ssize_t)sizeof(req_buf) - 1) {
        n = http_sock_recv_buf(client_fd, req_buf + total, sizeof(req_buf) - 1 - total);
        if (n <= 0) break;
        total += n;
        if (strstr(req_buf, "\r\n\r\n")) break;
    }

    if (total <= 0) { http_sock_close(client_fd); return; }

    /* 解析请求行 */
    char method[16] = {0}, path[2048] = {0}, version[16] = {0};
    char path_qs[2048];
    sscanf(req_buf, "%15s %2047s %15s", method, path, version);
    strncpy(path_qs, path, sizeof(path_qs) - 1);
    path_qs[sizeof(path_qs) - 1] = '\0';
    {
        char *qm = strchr(path, '?');
        if (qm) *qm = '\0';
        qm = strchr(path, '#');
        if (qm) *qm = '\0';
    }

    LOG_INFO("request  %s:%d \"%s %s %s\"",
             client_ip, client_port, method, path, version);

    /* ── POST ──────────────────────────────────────────────────── */
    if (strcasecmp(method, "POST") == 0) {
        long content_length = 0;
        const char *cl = platform_strcasestr(req_buf, "\r\nContent-Length:");
        if (cl) {
            cl += strlen("\r\nContent-Length:");
            while (*cl == ' ') cl++;
            content_length = atol(cl);
        }

        long max_body_allowed = MAX_BODY_SIZE;
        if (strcmp(path, "/api/save-register-file") == 0 ||
            strcmp(path, "/api/wiki-save") == 0 ||
            strcmp(path, "/api/wiki-upload") == 0 ||
            strcmp(path, "/api/wiki-export-pdf") == 0)
            max_body_allowed = SAVE_REPORT_MAX_BODY;

        char *body = NULL;
        if (content_length > 0 && content_length <= max_body_allowed) {
            body = calloc((size_t)content_length + 1, 1);
            if (body) {
                const char *hdr_end = strstr(req_buf, "\r\n\r\n");
                size_t already = 0;
                if (hdr_end) {
                    hdr_end += 4;
                    already = (size_t)(total - (hdr_end - req_buf));
                    if (already > (size_t)content_length)
                        already = (size_t)content_length;
                    memcpy(body, hdr_end, already);
                }
                size_t rcvd = already;
                while (rcvd < (size_t)content_length) {
                    n = http_sock_recv_buf(client_fd, body + rcvd,
                             (size_t)content_length - rcvd);
                    if (n <= 0) break;
                    rcvd += (size_t)n;
                }
            }
        }

        auth_user_t req_user;
        memset(&req_user, 0, sizeof(req_user));
        if (is_wiki_write_api(path)) {
            if (auth_require_author(req_buf, client_fd, &req_user) != 0) {
                auth_audit(client_ip, "", "denied", path, "guest write blocked");
                free(body);
                goto done;
            }
        }
        if (strcmp(path, "/api/wiki-user-save") == 0 ||
            strcmp(path, "/api/wiki-user-delete") == 0) {
            if (auth_require_admin(req_buf, client_fd, &req_user) != 0) {
                auth_audit(client_ip, "", "denied", path, "admin required");
                free(body);
                goto done;
            }
        }

        if (strcmp(path, "/api/wiki-login") == 0) {
            if (body) handle_api_wiki_login(client_fd, req_buf, body, client_ip);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-logout") == 0) {
            handle_api_wiki_logout(client_fd, req_buf);
        } else if (strcmp(path, "/api/save-register-file") == 0) {
            if (body)
                handle_api_save_register_file(client_fd, req_buf, body,
                                              (size_t)content_length);
            else {
                send_json(client_fd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"empty body\"}", 35);
            }
        } else if (strcmp(path, "/api/rename-register-file") == 0) {
            if (body) handle_api_rename_register_file(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/delete-register-file") == 0) {
            if (body) handle_api_delete_register_file(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/rename-register-dir") == 0) {
            if (body) handle_api_rename_register_dir(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/delete-register-dir") == 0) {
            if (body) handle_api_delete_register_dir(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/svn-log") == 0) {
            if (body) handle_api_svn_log(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-save") == 0) {
            if (body) {
                handle_api_wiki_save(client_fd, body, req_user.username, client_ip);
            }
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-delete") == 0) {
            if (body) {
                auth_audit(client_ip, req_user.username, "wiki_delete", "", "");
                handle_api_wiki_delete(client_fd, body,
                                       req_user.username, client_ip);
            }
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-rename-article") == 0) {
            if (body) handle_api_wiki_rename_article(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-rename-cat") == 0) {
            if (body) handle_api_wiki_rename_cat(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-delete-cat") == 0) {
            if (body) handle_api_wiki_delete_cat(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-move-article") == 0) {
            if (body) handle_api_wiki_move_article(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-mkdir") == 0) {
            if (body) handle_api_wiki_mkdir(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-upload") == 0) {
            if (body) handle_api_wiki_upload(client_fd, req_buf, body,
                                             (size_t)content_length);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-export-pdf") == 0) {
            if (body) handle_api_wiki_export_pdf(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-cleanup-uploads") == 0) {
            auth_audit(client_ip, req_user.username, "wiki_cleanup_uploads", "", "");
            handle_api_wiki_cleanup_uploads(client_fd);
        } else if (strcmp(path, "/api/wiki-rebuild-html") == 0) {
            auth_audit(client_ip, req_user.username, "wiki_rebuild_html", "", "");
            handle_api_wiki_rebuild_html(client_fd);
        } else if (strcmp(path, "/api/wiki-trash-restore") == 0) {
            if (body) {
                auth_audit(client_ip, req_user.username,
                           "wiki_trash_restore", "", "");
                handle_api_wiki_trash_restore(client_fd, body);
            } else send_json(client_fd, 400, "Bad Request",
                             "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-trash-empty") == 0) {
            if (body) {
                auth_audit(client_ip, req_user.username,
                           "wiki_trash_empty", "", "");
                handle_api_wiki_trash_empty(client_fd, body);
            } else send_json(client_fd, 400, "Bad Request",
                             "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-restore-version") == 0) {
            if (body) {
                handle_api_wiki_restore_version(client_fd, body,
                                                 req_user.username, client_ip);
            } else send_json(client_fd, 400, "Bad Request",
                             "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-user-save") == 0) {
            if (body) {
                auth_audit(client_ip, req_user.username, "user_save", "", "");
                handle_api_wiki_user_save(client_fd, body);
            } else {
                send_json(client_fd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"empty body\"}", 35);
            }
        } else if (strcmp(path, "/api/wiki-user-delete") == 0) {
            if (body) {
                auth_audit(client_ip, req_user.username, "user_delete", "", "");
                handle_api_wiki_user_delete(client_fd, body);
            } else {
                send_json(client_fd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"empty body\"}", 35);
            }
        } else if (strcmp(path, "/api/wiki-notewiki-prefs") == 0) {
            if (body)
                handle_api_wiki_notewiki_prefs_post(client_fd, req_buf, body);
            else
                send_json(client_fd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else {
            send_response(client_fd, 404, "Not Found",
                          "<h1>404 Not Found</h1>");
        }

        free(body);
        goto done;
    }

    /* ── GET ───────────────────────────────────────────────────── */
    if (strcasecmp(method, "GET") != 0) {
        send_response(client_fd, 405, "Method Not Allowed",
                      "<h1>405 Method Not Allowed</h1>");
        goto done;
    }

    if (strstr(path, "..")) {
        send_response(client_fd, 403, "Forbidden", "<h1>403 Forbidden</h1>");
        goto done;
    }

    if (strcmp(path, "/api/gt-sdk-doc") == 0) {
        char cidir[512];
        snprintf(cidir, sizeof(cidir), "%s/wiki/ci_html", WEB_ROOT);
        DIR *d = opendir(cidir);
        if (!d) {
            send_response(client_fd, 404, "Not Found",
                          "<h1>404 Not Found</h1><p>GT SDK document not found</p>");
            goto done;
        }
        char best[256] = {0};
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            const char *nm = de->d_name;
            size_t nml = strlen(nm);
            if (nml < 11) continue;  /* GT_SDK_ + .html = 11 min */
            if (strncmp(nm, "GT_SDK_", 7) != 0) continue;
            if (nml < 5 || strcasecmp(nm + nml - 5, ".html") != 0) continue;
            if (!best[0] || strcmp(nm, best) < 0)
                strncpy(best, nm, sizeof(best) - 1);
        }
        closedir(d);
        if (!best[0]) {
            send_response(client_fd, 404, "Not Found",
                          "<h1>404 Not Found</h1><p>GT SDK document not found</p>");
            goto done;
        }
        char loc[640];
        snprintf(loc, sizeof(loc), "/wiki/ci_html/%s", best);
        send_redirect(client_fd, loc);
        goto done;
    }

    if (strcmp(path, "/api/wiki-list") == 0) {
        handle_api_wiki_list(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/wiki-trash-list") == 0) {
        handle_api_wiki_trash_list(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/wiki-notewiki-prefs") == 0) {
        handle_api_wiki_notewiki_prefs_get(client_fd, req_buf);
        goto done;
    }

    if (strcmp(path, "/api/wiki-auth-status") == 0) {
        handle_api_wiki_auth_status(client_fd, req_buf);
        goto done;
    }

    if (strcmp(path, "/api/wiki-users") == 0) {
        auth_user_t u;
        if (auth_require_admin(req_buf, client_fd, &u) != 0) goto done;
        handle_api_wiki_users_list(client_fd);
        goto done;
    }

    if (strncmp(path, "/api/wiki-audit-logs", 20) == 0 &&
        (path[20] == '\0' || path[20] == '?')) {
        auth_user_t u;
        if (auth_require_admin(req_buf, client_fd, &u) != 0) goto done;
        handle_api_wiki_audit_logs(client_fd, path_qs);
        goto done;
    }

    if (strncmp(path, "/api/wiki-md-history", 20) == 0 &&
        (path[20] == '\0' || path[20] == '?')) {
        char hist_id[256] = {0};
        query_param_get(path_qs, "id", hist_id, sizeof(hist_id));
        if (!hist_id[0]) {
            auth_user_t u;
            if (auth_require_author(req_buf, client_fd, &u) != 0) goto done;
        }
        handle_api_wiki_md_history(client_fd, path_qs);
        goto done;
    }

    if (strncmp(path, "/api/wiki-user-article-rank", 27) == 0 &&
        (path[27] == '\0' || path[27] == '?')) {
        auth_user_t u;
        if (auth_require_admin(req_buf, client_fd, &u) != 0) goto done;
        handle_api_wiki_user_article_rank(client_fd, path_qs);
        goto done;
    }

    if (strcmp(path, "/api/wiki-refresh-index") == 0) {
        auth_user_t u;
        if (auth_require_author(req_buf, client_fd, &u) != 0) goto done;
        auth_audit(client_ip, u.username, "wiki_refresh_index", "", "");
        handle_api_wiki_refresh_index(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/wiki-rebuild-html") == 0) {
        auth_user_t u;
        if (auth_require_author(req_buf, client_fd, &u) != 0) goto done;
        auth_audit(client_ip, u.username, "wiki_rebuild_html", "", "");
        handle_api_wiki_rebuild_html(client_fd);
        goto done;
    }

    if (strncmp(path, "/api/wiki-export-md-zip", 23) == 0 &&
        (path[23] == '\0' || path[23] == '?')) {
        handle_api_wiki_export_md_zip(client_fd, path_qs);
        goto done;
    }

    if (strncmp(path, "/api/wiki-read", 14) == 0 &&
        (path[14] == '\0' || path[14] == '?')) {
        handle_api_wiki_read(client_fd, path_qs);
        goto done;
    }

    if (strncmp(path, "/api/wiki-search", 16) == 0 &&
        (path[16] == '\0' || path[16] == '?')) {
        handle_api_wiki_search(client_fd, path_qs);
        goto done;
    }

    if (strcmp(path, "/api/list-register-files") == 0) {
        handle_api_list_register_files(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/list-register-dirs") == 0) {
        handle_api_list_register_dirs(client_fd);
        goto done;
    }

    /* 浏览器默认请求 /favicon.ico；无 .ico 时回退到 html/favicon.svg，避免 404 */
    if (strcmp(path, "/favicon.ico") == 0) {
        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s/favicon.ico", WEB_ROOT);
        if (send_file(client_fd, fpath, req_buf) < 0) {
            snprintf(fpath, sizeof(fpath), "%s/favicon.svg", WEB_ROOT);
            if (send_file(client_fd, fpath, req_buf) < 0) {
                static const char nofav[] =
                    "HTTP/1.1 204 No Content\r\n"
                    "Connection: close\r\n"
                    "\r\n";
                (void)http_sock_send_all(client_fd, nofav, sizeof(nofav) - 1);
            }
        }
        goto done;
    }

    {
        auth_user_t u;
        if (strncmp(path, "/wiki/", 6) == 0 && auth_resolve_user_from_headers(req_buf, &u) != 0) {
            auth_audit(client_ip, "guest", "guest_view", path, "");
        }
        char filepath[2048];
        char decoded_path[2048];
        strncpy(decoded_path, path, sizeof(decoded_path) - 1);
        decoded_path[sizeof(decoded_path) - 1] = '\0';
        url_decode_report_fn(decoded_path);
        if (strcmp(path, "/") == 0)
            snprintf(filepath, sizeof(filepath), "%s/index.html", WEB_ROOT);
        else
            snprintf(filepath, sizeof(filepath), "%s%s", WEB_ROOT, decoded_path);

        if (send_file(client_fd, filepath, req_buf) < 0) {
            char body[256];
            snprintf(body, sizeof(body),
                     "<h1>404 Not Found</h1><p>%s</p>", path);
            send_response(client_fd, 404, "Not Found", body);
            LOG_WARN("not_found  %s:%d \"%s\"", client_ip, client_port, path);
        }
    }

done:;
    double elapsed = (double)(clock() - t_start) / CLOCKS_PER_SEC * 1000.0;
    LOG_INFO("response %s:%d \"%s\" done in %.2fms",
             client_ip, client_port, path, elapsed);

    http_sock_close(client_fd);
}
