#include "http_handler.h"
#include "http_utils.h"
#include "log.h"
#include "ssh_exec.h"
#include "ssh_api.h"
#include "svn_api.h"
#include "monitor.h"
#include "report_api.h"
#include "register_api.h"
#include "wiki.h"

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

/* ── 主处理入口 ──────────────────────────────────────────────── */

void handle_client(http_sock_t client_fd, struct sockaddr_in *addr)
{
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));
    int  client_port = ntohs(addr->sin_port);

    stats_req_start(client_ip);
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

    if (total <= 0) { stats_req_end(); http_sock_close(client_fd); return; }

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

    int is_poll_api = (strncmp(path, "/api/monitor", 12) == 0 ||
                       strncmp(path, "/api/procs",   10) == 0 ||
                       strcmp(path, "/api/client-info") == 0);
    if (!is_poll_api)
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
        if (strcmp(path, "/api/save-report") == 0 ||
            strcmp(path, "/api/save-config") == 0 ||
            strcmp(path, "/api/save-register-file") == 0 ||
            strcmp(path, "/api/wiki-save") == 0 ||
            strcmp(path, "/api/wiki-upload") == 0)
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

        if (strcmp(path, "/api/ssh-exec") == 0) {
            if (body) handle_api_ssh_exec(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"error\":\"empty body\"}", 21);
        } else if (strcmp(path, "/api/ssh-exec-stream") == 0) {
            if (body) handle_api_ssh_exec_stream(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"error\":\"empty body\"}", 21);
        } else if (strcmp(path, "/api/ssh-exec-one") == 0) {
            if (body) handle_api_ssh_exec_one(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"error\":\"empty body\"}", 21);
        } else if (strcmp(path, "/api/cancel") == 0) {
            ssh_cancel_current();
            send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
        } else if (strcmp(path, "/api/kill") == 0) {
            if (body) {
                int pid = json_get_int(body, "pid", -1);
                if (pid > 1) {
                    if (platform_process_kill(pid) == 0) {
                        char resp[64];
                        int rlen = snprintf(resp, sizeof(resp),
                                            "{\"ok\":true,\"pid\":%d}", pid);
                        send_json(client_fd, 200, "OK", resp, (size_t)rlen);
                    } else {
                        char resp[128];
                        int rlen = snprintf(resp, sizeof(resp),
                                            "{\"ok\":false,\"error\":\"%s\",\"pid\":%d}",
                                            strerror(errno), pid);
                        send_json(client_fd, 200, "OK", resp, (size_t)rlen);
                    }
                } else {
                    send_json(client_fd, 400, "Bad Request",
                              "{\"ok\":false,\"error\":\"invalid pid\"}", 35);
                }
            } else {
                send_json(client_fd, 400, "Bad Request",
                          "{\"error\":\"empty body\"}", 21);
            }
        } else if (strcmp(path, "/api/save-report") == 0) {
            if (body)
                handle_api_save_report(client_fd, req_buf, body,
                                       (size_t)content_length);
            else if (content_length > SAVE_REPORT_MAX_BODY) {
                send_json(client_fd, 413, "Payload Too Large",
                          "{\"ok\":false,\"error\":\"body too large\"}", 38);
            } else {
                send_json(client_fd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"empty body\"}", 35);
            }
        } else if (strcmp(path, "/api/save-config") == 0) {
            if (body)
                handle_api_save_config(client_fd, req_buf, body,
                                       (size_t)content_length);
            else {
                send_json(client_fd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"empty body\"}", 35);
            }
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
        } else if (strcmp(path, "/api/delete-report") == 0) {
            if (body)
                handle_api_delete_report(client_fd, body);
            else
                send_json(client_fd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/svn-log") == 0) {
            if (body) handle_api_svn_log(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-save") == 0) {
            if (body) handle_api_wiki_save(client_fd, body);
            else send_json(client_fd, 400, "Bad Request",
                           "{\"ok\":false,\"error\":\"empty body\"}", 35);
        } else if (strcmp(path, "/api/wiki-delete") == 0) {
            if (body) handle_api_wiki_delete(client_fd, body);
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
        } else if (strcmp(path, "/api/wiki-cleanup-uploads") == 0) {
            handle_api_wiki_cleanup_uploads(client_fd);
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

    if (strcmp(path, "/api/monitor") == 0) {
        handle_api_monitor(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/reports") == 0) {
        handle_api_reports(client_fd);
        goto done;
    }

    if (strncmp(path, "/api/list-ssh-configs", 21) == 0) {
        const char *rest = path + 21;
        if (*rest == '\0' || *rest == '?') {
            handle_api_list_ssh_configs(client_fd, path_qs);
            goto done;
        }
    }

    if (strcmp(path, "/api/list-all-configs") == 0) {
        handle_api_list_all_configs(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/wiki-list") == 0) {
        handle_api_wiki_list(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/wiki-refresh-index") == 0) {
        handle_api_wiki_refresh_index(client_fd);
        goto done;
    }

    if (strcmp(path, "/api/wiki-rebuild-html") == 0) {
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

    if (strcmp(path, "/api/client-info") == 0) {
        handle_api_client_info(client_fd, client_ip);
        goto done;
    }

    if (strncmp(path, "/api/procs", 10) == 0 &&
        (path[10] == '\0' || path[10] == '?')) {
        const char *q = "";
        const char *qs = strchr(path_qs, '?');
        char query_buf[128] = "";
        int include_ports = 0;
        if (qs && strstr(qs, "ports=1")) include_ports = 1;
        if (qs) {
            const char *qp = strstr(qs, "q=");
            if (qp) {
                qp += 2;
                size_t qi = 0;
                while (*qp && *qp != '&' && qi < sizeof(query_buf) - 1) {
                    if (*qp == '+') { query_buf[qi++] = ' '; qp++; }
                    else if (*qp == '%' && isxdigit((unsigned char)qp[1]) && isxdigit((unsigned char)qp[2])) {
                        char hex[3] = { qp[1], qp[2], '\0' };
                        query_buf[qi++] = (char)strtol(hex, NULL, 16);
                        qp += 3;
                    } else {
                        query_buf[qi++] = *qp++;
                    }
                }
                query_buf[qi] = '\0';
                q = query_buf;
            }
        }
        handle_api_procs(client_fd, q, include_ports);
        goto done;
    }

    if (strncmp(path, "/api/port", 9) == 0 &&
        (path[9] == '\0' || path[9] == '?')) {
        int port_num = 0;
        const char *qs = strchr(path_qs, '?');
        if (qs) {
            const char *pp = strstr(qs, "port=");
            if (pp) port_num = atoi(pp + 5);
        }
        handle_api_port(client_fd, port_num);
        goto done;
    }

    if (strcmp(path, "/api/log-files") == 0) {
        strbuf_t sb = {0};
        SB_LIT(&sb, "{\"ok\":true,\"files\":[");
        DIR *ld = opendir("logs");
        if (ld) {
            char names[LOG_MAX_FILES + 2][64];
            int  nc = 0;
            struct dirent *de;
            while ((de = readdir(ld)) != NULL && nc < LOG_MAX_FILES) {
                const char *nm  = de->d_name;
                size_t      nml = strlen(nm);
                if (nml > 4 && strcmp(nm + nml - 4, ".log") == 0) {
                    strncpy(names[nc], nm, 63);
                    names[nc][63] = '\0';
                    nc++;
                }
            }
            closedir(ld);
            for (int i = 0; i < nc - 1; i++)
                for (int j = 0; j < nc - i - 1; j++)
                    if (strcmp(names[j], names[j + 1]) > 0) {
                        char tmp[64];
                        memcpy(tmp,        names[j],     64);
                        memcpy(names[j],   names[j + 1], 64);
                        memcpy(names[j+1], tmp,          64);
                    }
            for (int i = 0; i < nc; i++) {
                if (i) SB_LIT(&sb, ",");
                sb_json_str(&sb, names[i]);
            }
        }
        SB_LIT(&sb, "]}");
        if (sb.data) { send_json(client_fd, 200, "OK", sb.data, sb.len); free(sb.data); }
        else send_json(client_fd, 500, "Internal Server Error", "{\"ok\":false}", 12);
        goto done;
    }

    if (strncmp(path, "/logs/", 6) == 0 && path[6] != '\0') {
        char filepath[2048];
        char logs_seg[2048];
        strncpy(logs_seg, path + 6, sizeof(logs_seg) - 1);
        logs_seg[sizeof(logs_seg) - 1] = '\0';
        url_decode_report_fn(logs_seg);
        snprintf(filepath, sizeof(filepath), "logs/%s", logs_seg);
        if (send_file(client_fd, filepath) < 0)
            send_response(client_fd, 404, "Not Found",
                          "<h1>404 Not Found</h1>");
        goto done;
    }

    {
        char filepath[2048];
        char decoded_path[2048];
        strncpy(decoded_path, path, sizeof(decoded_path) - 1);
        decoded_path[sizeof(decoded_path) - 1] = '\0';
        url_decode_report_fn(decoded_path);
        if (strcmp(path, "/") == 0)
            snprintf(filepath, sizeof(filepath), "%s/index.html", WEB_ROOT);
        else
            snprintf(filepath, sizeof(filepath), "%s%s", WEB_ROOT, decoded_path);

        if (send_file(client_fd, filepath) < 0) {
            char body[256];
            snprintf(body, sizeof(body),
                     "<h1>404 Not Found</h1><p>%s</p>", path);
            send_response(client_fd, 404, "Not Found", body);
            LOG_WARN("not_found  %s:%d \"%s\"", client_ip, client_port, path);
        }
    }

done:;
    stats_req_end();
    double elapsed = (double)(clock() - t_start) / CLOCKS_PER_SEC * 1000.0;
    if (!is_poll_api)
        LOG_INFO("response %s:%d \"%s\" done in %.2fms",
                 client_ip, client_port, path, elapsed);

    http_sock_close(client_fd);
}
