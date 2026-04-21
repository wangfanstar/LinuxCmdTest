#include "wiki.h"
#include "http_handler.h"
#include "http_utils.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>

/* ── 路径常量 ─────────────────────────────────────────────── */

#define WIKI_ROOT    WEB_ROOT "/wiki"
#define WIKI_MD_DB   WIKI_ROOT "/md_db"
#define WIKI_UPLOADS WIKI_ROOT "/uploads"

/* ── 前向声明 ─────────────────────────────────────────────── */

static void wiki_rewrite_html(const char *id, const char *title,
                               const char *cat, const char *updated);
static void rmdir_recursive(const char *path);
typedef struct _strnode { char *s; struct _strnode *next; } _strnode_t;
static void _strlist_add(_strnode_t **head, const char *s, size_t len);
static void _strlist_free(_strnode_t *head);

/* ── 内部工具 ─────────────────────────────────────────────── */

static char *json_get_str_alloc(const char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    size_t len = 0;
    const char *q = p;
    while (*q && *q != '"') { if (*q == '\\') { q++; if (!*q) break; } len++; q++; }
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++; if (!*p) break;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                default:  out[i++] = *p;   break;
            }
        } else { out[i++] = *p; }
        p++;
    }
    out[i] = '\0';
    return out;
}

static void wiki_gen_id(char *buf, size_t sz)
{
    time_t t = time(NULL); struct tm tm;
    localtime_r(&t, &tm);
    unsigned long tid = (unsigned long)pthread_self() & 0xFFFF;
    snprintf(buf, sz, "note_%04d%02d%02d_%02d%02d%02d_%04lx",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, tid);
}

static void wiki_now_iso(char *buf, size_t sz)
{
    time_t t = time(NULL); struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, sz, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static int wiki_md_find_r(char *buf, size_t sz, const char *dir, const char *target)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de; int found = -1;
    while ((de = readdir(d)) != NULL && found < 0) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) found = wiki_md_find_r(buf, sz, child, target);
        else if (strcmp(de->d_name, target) == 0) { snprintf(buf, sz, "%s", child); found = 0; }
    }
    closedir(d);
    return found;
}

static int wiki_md_find(char *buf, size_t sz, const char *id)
{
    char target[256]; snprintf(target, sizeof(target), "%s.md", id);
    return wiki_md_find_r(buf, sz, WIKI_MD_DB, target);
}

static void wiki_md_write_path(char *buf, size_t sz, const char *id, const char *cat)
{
    if (cat && cat[0]) {
        char catdir[768]; snprintf(catdir, sizeof(catdir), "%s/%s", WIKI_MD_DB, cat);
        mkdir_p(catdir);
        snprintf(buf, sz, "%s/%s/%s.md", WIKI_MD_DB, cat, id);
    } else {
        snprintf(buf, sz, "%s/%s.md", WIKI_MD_DB, id);
    }
}

static void wiki_scan_md_dir(strbuf_t *sb, int *pfirst, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { wiki_scan_md_dir(sb, pfirst, child); continue; }
        size_t nl = strlen(de->d_name);
        if (nl < 4 || strcmp(de->d_name + nl - 3, ".md") != 0) continue;
        FILE *fp = fopen(child, "r"); if (!fp) continue;
        char line[4096] = {0}; int ok = (fgets(line, sizeof(line), fp) != NULL); fclose(fp);
        if (!ok || strncmp(line, "<!--META ", 9) != 0) continue;
        char *end = strstr(line, "-->"); if (!end) continue; *end = '\0';
        const char *mj = line + 9;
        char id[128]={0},title[512]={0},cat[512]={0},cre[64]={0},upd[64]={0};
        json_get_str(mj,"id",id,sizeof(id)); json_get_str(mj,"title",title,sizeof(title));
        json_get_str(mj,"category",cat,sizeof(cat)); json_get_str(mj,"created",cre,sizeof(cre));
        json_get_str(mj,"updated",upd,sizeof(upd));
        if (!id[0]) continue;
        if (!*pfirst) SB_LIT(sb, ","); *pfirst = 0;
        SB_LIT(sb,"{\"id\":"); sb_json_str(sb,id);
        SB_LIT(sb,",\"title\":"); sb_json_str(sb,title);
        SB_LIT(sb,",\"category\":"); sb_json_str(sb,cat);
        SB_LIT(sb,",\"created\":"); sb_json_str(sb,cre);
        SB_LIT(sb,",\"updated\":"); sb_json_str(sb,upd);
        SB_LIT(sb,"}");
    }
    closedir(d);
}

static void wiki_rebuild_md_dir(int *pcount, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { wiki_rebuild_md_dir(pcount, child); continue; }
        size_t nl = strlen(de->d_name);
        if (nl < 4 || strcmp(de->d_name + nl - 3, ".md") != 0) continue;
        FILE *fp = fopen(child, "r"); if (!fp) continue;
        char ml[4096] = {0}; fgets(ml, sizeof(ml), fp); fclose(fp);
        if (strncmp(ml,"<!--META ",9)!=0) continue;
        char *mend = strstr(ml,"-->"); if (!mend) continue; *mend='\0';
        const char *mj = ml + 9;
        char id[128]={0},title[512]={0},cat[512]={0},upd[64]={0};
        json_get_str(mj,"id",id,sizeof(id)); json_get_str(mj,"title",title,sizeof(title));
        json_get_str(mj,"category",cat,sizeof(cat)); json_get_str(mj,"updated",upd,sizeof(upd));
        if (!id[0]) continue;
        wiki_rewrite_html(id, title, cat[0] ? cat : NULL, upd);
        (*pcount)++;
    }
    closedir(d);
}

static void wiki_update_cat_in_dir(const char *dir,
                                    const char *old_path, const char *new_path,
                                    size_t old_len)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            wiki_update_cat_in_dir(child, old_path, new_path, old_len); continue;
        }
        size_t nl = strlen(de->d_name);
        if (nl < 4 || strcmp(de->d_name + nl - 3, ".md") != 0) continue;
        FILE *fp = fopen(child, "r"); if (!fp) continue;
        char ml[4096]={0}; fgets(ml, sizeof(ml), fp);
        strbuf_t cbuf={0}; char tbuf[8192]; size_t nr;
        while ((nr=fread(tbuf,1,sizeof(tbuf),fp))>0) sb_append(&cbuf,tbuf,nr);
        fclose(fp);
        if (strncmp(ml,"<!--META ",9)!=0) { free(cbuf.data); continue; }
        char *end=strstr(ml,"-->"); if (!end) { free(cbuf.data); continue; }
        *end='\0'; const char *mj=ml+9;
        char aid[128]={0},atitle[512]={0},acat[512]={0},acre[64]={0},aupd[64]={0};
        json_get_str(mj,"id",aid,sizeof(aid)); json_get_str(mj,"title",atitle,sizeof(atitle));
        json_get_str(mj,"category",acat,sizeof(acat)); json_get_str(mj,"created",acre,sizeof(acre));
        json_get_str(mj,"updated",aupd,sizeof(aupd));
        char new_cat[512]={0};
        if (strcmp(acat,old_path)==0) snprintf(new_cat,sizeof(new_cat),"%s",new_path);
        else if (strncmp(acat,old_path,old_len)==0 && acat[old_len]=='/')
            snprintf(new_cat,sizeof(new_cat),"%s%s",new_path,acat+old_len);
        else { free(cbuf.data); continue; }
        fp = fopen(child,"wb"); if (!fp) { free(cbuf.data); continue; }
        strbuf_t mb={0};
        SB_LIT(&mb,"<!--META "); SB_LIT(&mb,"{\"id\":"); sb_json_str(&mb,aid);
        SB_LIT(&mb,",\"title\":"); sb_json_str(&mb,atitle);
        SB_LIT(&mb,",\"category\":"); sb_json_str(&mb,new_cat);
        SB_LIT(&mb,",\"created\":"); sb_json_str(&mb,acre);
        SB_LIT(&mb,",\"updated\":"); sb_json_str(&mb,aupd);
        SB_LIT(&mb,"}-->\n");
        if (mb.data) fwrite(mb.data,1,mb.len,fp);
        if (cbuf.data) fwrite(cbuf.data,1,cbuf.len,fp);
        fclose(fp); free(mb.data); free(cbuf.data);
        wiki_rewrite_html(aid, atitle, new_cat, aupd);
    }
    closedir(d);
}

static int wiki_md_dir_has_md(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de; int found = 0;
    while (!found && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) found = wiki_md_dir_has_md(child);
        else { size_t nl=strlen(de->d_name);
               if (nl>=4 && strcmp(de->d_name+nl-3,".md")==0) found=1; }
    }
    closedir(d);
    return found;
}

static int wiki_upload_safe(const char *fn)
{
    if (!fn || !fn[0] || fn[0] == '.') return 0;
    if (strchr(fn, '/') || strchr(fn, '\\') || strchr(fn, ':')) return 0;
    const char *dot = strrchr(fn, '.');
    if (!dot) return 0;
    static const char *exts[] = {
        ".jpg",".jpeg",".png",".gif",".svg",".webp",
        ".pdf",".txt",".md",".zip",".tar",".gz",NULL
    };
    for (int i = 0; exts[i]; i++)
        if (strcasecmp(dot, exts[i]) == 0) return 1;
    return 0;
}

static void wiki_ensure_dirs(void)
{
    mkdir_p(WIKI_MD_DB);
    mkdir_p(WIKI_UPLOADS);
}

static void wiki_fhtml(FILE *fp, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
            case '&': fputs("&amp;", fp); break;
            case '<': fputs("&lt;",  fp); break;
            case '>': fputs("&gt;",  fp); break;
            case '"': fputs("&quot;",fp); break;
            default:  fputc(*s, fp);      break;
        }
    }
}

static void wiki_fjs(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (; s && *s; s++) {
        if      (*s == '\\') fputs("\\\\", fp);
        else if (*s == '"')  fputs("\\\"", fp);
        else if (*s == '\n') fputs("\\n",  fp);
        else if (*s == '\r') {}
        else                 fputc(*s, fp);
    }
    fputc('"', fp);
}

/* 根据 category 深度计算相对路径前缀（供离线 file:// 访问用）
   ""          → ""
   "cat"       → "../"
   "cat1/cat2" → "../../"  */
static void wiki_rel_prefix(char *buf, size_t bufsz, const char *cat)
{
    buf[0] = '\0';
    if (!cat || !cat[0]) return;
    int depth = 1;
    for (const char *p = cat; *p; p++) if (*p == '/') depth++;
    size_t pos = 0;
    for (int i = 0; i < depth && pos + 3 < bufsz; i++) {
        buf[pos++] = '.'; buf[pos++] = '.'; buf[pos++] = '/';
    }
    buf[pos] = '\0';
}

/* 将 html_body 写入 fp，同时将 /wiki/uploads/ 替换为相对路径 */
static void wiki_fwrite_body(FILE *fp, const char *body, const char *prefix)
{
    static const char nd[] = "/wiki/uploads/";
    const size_t nl = sizeof(nd) - 1;
    const char *p = body;
    while (p && *p) {
        const char *f = strstr(p, nd);
        if (!f) { fputs(p, fp); break; }
        if (f > p) fwrite(p, 1, (size_t)(f - p), fp);
        fputs(prefix, fp);
        fputs("uploads/", fp);
        p = f + nl;
    }
}

static int wiki_copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[65536];
    size_t nr;
    while ((nr = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, nr, out) != nr) { fclose(in); fclose(out); return -1; }
    }
    fclose(in);
    fclose(out);
    return 0;
}

static int wiki_send_attachment_file(int fd, const char *filepath, const char *download_name)
{
    struct stat st;
    if (stat(filepath, &st) < 0 || S_ISDIR(st.st_mode)) return -1;
    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) return -1;

    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/zip\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Content-Length: %ld\r\n"
        "Cache-Control: no-store\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        download_name, (long)st.st_size);
    write(fd, header, hlen);

    char buf[65536];
    ssize_t n;
    while ((n = read(file_fd, buf, sizeof(buf))) > 0) write(fd, buf, (size_t)n);
    close(file_fd);
    return 0;
}

static void wiki_collect_upload_refs_from_md(const char *md, _strnode_t **refs)
{
    if (!md) return;
    const char *marker = "/wiki/uploads/";
    size_t mlen = strlen(marker);
    const char *p = md;
    while ((p = strstr(p, marker)) != NULL) {
        p += mlen;
        const char *end = p;
        while (*end && *end != ')' && *end != '"' && *end != '\'' &&
               !isspace((unsigned char)*end)) end++;
        size_t len = (size_t)(end - p);
        if (len > 0 && len < 768 && strstr(p, "..") == NULL && *p != '/') {
            _strlist_add(refs, p, len);
        }
        p = end;
    }
}

/* ZIP 导出：把 /wiki/uploads/... 写成相对路径 assets/... */
static void wiki_md_rewrite_uploads_to_assets(strbuf_t *sb)
{
    if (!sb || !sb->data || sb->len == 0) return;
    static const char nd[] = "/wiki/uploads/";
    const size_t nl = sizeof(nd) - 1;
    strbuf_t out = {0};
    const char *p = sb->data;
    for (;;) {
        const char *f = strstr(p, nd);
        if (!f) {
            sb_append(&out, p, strlen(p));
            break;
        }
        if (f > p) sb_append(&out, p, (size_t)(f - p));
        SB_LIT(&out, "assets/");
        p = f + nl;
    }
    free(sb->data);
    sb->data = out.data;
    sb->len = out.len;
}

/* ── wiki_write_html_file ─────────────────────────────────── */

static int wiki_write_html_file(const char *filepath,
    const char *id, const char *title, const char *category,
    const char *updated, const char *html_body)
{
    FILE *fp = fopen(filepath, "wb");
    if (!fp) return -1;

    /* 相对路径前缀，使 HTML 文件可离线用 file:// 打开 */
    char rp[64];
    wiki_rel_prefix(rp, sizeof(rp), category);

    fputs("<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
          "<meta charset=\"UTF-8\">\n"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
          "<title>", fp);
    wiki_fhtml(fp, title);
    fputs(" - NoteWiki</title>\n<style>\n"
          "*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}\n"
          "html,body{height:100%;overflow:hidden}\n"
          "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
               "background:#0d1117;color:#c9d1d9;line-height:1.7;"
               "display:flex;flex-direction:column}\n"
          "nav.topbar{background:#161b22;border-bottom:1px solid #30363d;"
                    "padding:8px 20px;font-size:.85rem;flex-shrink:0}\n"
          "nav.topbar a{color:#4a90e2;text-decoration:none}\n"
          ".layout{display:flex;flex:1;overflow:hidden}\n"
          ".sidebar{width:220px;background:#161b22;border-right:1px solid #30363d;"
                  "overflow:hidden;flex-shrink:0;transition:width .2s ease;"
                  "display:flex;flex-direction:column}\n"
          ".sidebar.collapsed{width:28px}\n"
          ".sidebar-body{overflow-y:auto;flex:1}\n"
          ".sidebar.collapsed .sidebar-body{display:none}\n"
          ".content{flex:1;overflow-y:auto;padding:32px 40px}\n"
          "article{width:100%}\n"
          ".st-top{font-size:.78rem;color:#8b949e;padding:6px 8px 6px 12px;"
                 "border-bottom:1px solid #21262d;font-weight:600;"
                 "letter-spacing:.04em;flex-shrink:0;"
                 "display:flex;align-items:center;justify-content:space-between}\n"
          ".sidebar.collapsed .st-top{justify-content:center;padding:6px 0}\n"
          ".sidebar.collapsed .st-top .panel-label{display:none}\n"
          ".st-cat{font-size:.8rem;font-weight:600;color:#8b949e;"
                 "padding:5px 12px 2px;letter-spacing:.04em;"
                 "user-select:none;white-space:nowrap;overflow:hidden;"
                 "text-overflow:ellipsis;cursor:pointer;display:flex;align-items:center;gap:4px}\n"
          ".st-cat:hover{color:#f0f6fc}\n"
          ".cat-arrow{font-size:.65rem;flex-shrink:0;width:10px;text-align:center}\n"
          ".cat-body{overflow:hidden}\n"
          ".st-art{font-size:.83rem;color:#c9d1d9;padding:3px 12px;"
                 "text-decoration:none;display:block;white-space:nowrap;"
                 "overflow:hidden;text-overflow:ellipsis}\n"
          ".st-art:hover{background:#21262d;color:#f0f6fc}\n"
          ".st-art.active{background:#1a3a5c;color:#7ab8ff;font-weight:500}\n"
          ".toc{width:200px;background:#161b22;border-left:1px solid #30363d;"
              "overflow:hidden;flex-shrink:0;transition:width .2s ease;"
              "display:flex;flex-direction:column}\n"
          ".toc.collapsed{width:28px}\n"
          ".toc-body{overflow-y:auto;flex:1}\n"
          ".toc.collapsed .toc-body{display:none}\n"
          ".toc-top{font-size:.78rem;color:#8b949e;padding:6px 4px 6px 12px;"
                  "border-bottom:1px solid #21262d;font-weight:600;"
                  "letter-spacing:.04em;flex-shrink:0;"
                  "display:flex;align-items:center;justify-content:space-between}\n"
          ".toc.collapsed .toc-top{justify-content:center;padding:6px 0}\n"
          ".toc.collapsed .toc-top .panel-label{display:none}\n"
          ".toc-node{}\n"
          ".toc-row{display:flex;align-items:center;gap:2px;cursor:pointer}\n"
          ".toc-tog{font-size:.65rem;color:#8b949e;cursor:pointer;width:14px;text-align:center;"
                  "flex-shrink:0;user-select:none;padding:2px 0}\n"
          ".toc-tog:hover{color:#f0f6fc}\n"
          ".toc-tog-sp{width:14px;flex-shrink:0}\n"
          ".toc-children{overflow:hidden}\n"
          ".toc-item{font-size:.8rem;color:#8b949e;padding:3px 4px 3px 0;"
                   "text-decoration:none;display:block;white-space:nowrap;"
                   "overflow:hidden;text-overflow:ellipsis;flex:1;min-width:0}\n"
          ".toc-item:hover{color:#f0f6fc}\n"
          ".toc-row:hover{background:#21262d;border-radius:3px}\n"
          ".toc-item.active{color:#7ab8ff;font-weight:500}\n"
          ".panel-toggle{background:none;border:none;cursor:pointer;"
                        "color:#8b949e;font-size:.8rem;padding:2px 5px;"
                        "border-radius:3px;flex-shrink:0;line-height:1}\n"
          ".panel-toggle:hover{background:#21262d;color:#f0f6fc}\n"
          ".edit-btn{font-size:.78rem;color:#4a90e2;text-decoration:none;"
                   "padding:3px 10px;border:1px solid #2d3a54;border-radius:5px;"
                   "margin-left:auto;white-space:nowrap}\n"
          ".edit-btn:hover{background:#1a3a5c;border-color:#4a90e2}\n"
          ".copy-btn{font-size:.78rem;color:#8b949e;background:none;"
                   "padding:3px 10px;border:1px solid #30363d;border-radius:5px;"
                   "cursor:pointer;white-space:nowrap;margin-left:6px}\n"
          ".copy-btn:hover{background:#21262d;color:#f0f6fc;border-color:#8b949e}\n"
          ".ab .code-block{position:relative;margin:1em 0}\n"
          ".ab .code-block pre{margin:0}\n"
          ".ab .code-block .copy-btn{position:absolute;top:7px;right:9px;"
              "padding:2px 9px;font-size:.7rem;line-height:1.5;"
              "background:#1e2740;color:#8b949e;border:1px solid #30363d;"
              "border-radius:4px;cursor:pointer;opacity:0;"
              "transition:opacity .15s,color .15s,border-color .15s;"
              "z-index:2;white-space:nowrap;margin-left:0}\n"
          ".ab .code-block:hover .copy-btn{opacity:1}\n"
          ".ab .code-block .copy-btn:hover{background:#1e2740;color:#f0f6fc;border-color:#8b949e}\n"
          ".ab .code-block .copy-btn.copied{color:#3fb950;border-color:#2a5a3a;opacity:1}\n"
          "h1.at{font-size:1.75rem;color:#f0f6fc;margin-bottom:6px}\n"
          ".am{font-size:.75rem;color:#8b949e;padding-bottom:14px;"
              "border-bottom:1px solid #30363d;margin-bottom:24px}\n"
          ".ab h1,.ab h2,.ab h3,.ab h4,.ab h5,.ab h6{margin:1.3em 0 .5em;line-height:1.35}\n"
          ".ab h1{color:#79c0ff}\n"
          ".ab h2{color:#7ee787}\n"
          ".ab h3{color:#ffa657}\n"
          ".ab h4{color:#ff7b72}\n"
          ".ab h5,.ab h6{color:#d2a8ff}\n"
          ".ab p{margin:.7em 0}\n"
          ".ab .code-block pre,.ab pre{background:#0d1117;border:1px solid #30363d;"
                  "border-left:3px solid #3f6fb2;border-radius:6px;padding:12px 14px;"
                  "overflow-x:auto;margin:1em 0}\n"
          ".ab code{font-family:ui-monospace,'SFMono-Regular',Consolas,monospace;font-size:.88em}\n"
          ".ab pre code{color:#c9d1d9}\n"
          ".ab :not(pre)>code{background:rgba(210,153,34,.14);padding:2px 7px;"
                             "border-radius:4px;color:#ffa657;border:1px solid rgba(210,153,34,.38)}\n"
          ".ab strong{color:#f0f6fc;font-weight:700}\n"
          ".ab em{color:#c4d6ec;font-style:italic}\n"
          ".ab strong em,.ab em strong{color:#ffdfa8}\n"
          ".ab del{color:#8b949e;text-decoration:line-through}\n"
          ".ab blockquote{background:rgba(74,144,226,.08);border-left:4px solid #4a90e2;"
                         "padding:.55em 1em .55em 1.1em;color:#9fb0c3;margin:1em 0;"
                         "border-radius:0 6px 6px 0}\n"
          ".ab table{border-collapse:collapse;width:100%;margin:1em 0}\n"
          ".ab tbody tr:nth-child(even){background:rgba(110,118,129,.06)}\n"
          ".ab th,.ab td{border:1px solid #30363d;padding:6px 10px;text-align:left}\n"
          ".ab th{background:#161b22;font-weight:600;color:#f0f6fc}\n"
          ".ab img{max-width:100%;border-radius:4px}\n"
          ".ab ul,.ab ol{padding-left:1.5em;margin:.5em 0}\n"
          ".ab ul li::marker{color:#e3b341}\n"
          ".ab ol li::marker{color:#79c0ff}\n"
          ".ab a{color:#58a6ff;text-decoration:none;border-bottom:1px solid rgba(88,166,255,.35)}\n"
          ".ab a:hover{color:#79c0ff;border-bottom-color:#79c0ff}\n"
          ".ab hr{border:none;border-top:1px solid #30363d;margin:1.4em 0;opacity:.9}\n"
          ".sec-num{color:#8b949e;font-weight:400;font-size:.9em;letter-spacing:.02em}\n"
          ".ab{counter-reset:sc1 sc2 sc3 sc4 sc5 sc6}\n"
          ".ab h1{counter-reset:sc2 sc3 sc4 sc5 sc6;counter-increment:sc1}\n"
          ".ab h2{counter-reset:sc3 sc4 sc5 sc6;counter-increment:sc2}\n"
          ".ab h3{counter-reset:sc4 sc5 sc6;counter-increment:sc3}\n"
          ".ab h4{counter-reset:sc5 sc6;counter-increment:sc4}\n"
          ".ab h5{counter-reset:sc6;counter-increment:sc5}\n"
          ".ab h6{counter-increment:sc6}\n"
          ".ab h1::before{content:counter(sc1)'. ';color:#8b949e;font-weight:400;font-size:.88em}\n"
          ".ab h2::before{content:counter(sc1)'.'counter(sc2)' ';color:#8b949e;font-weight:400;font-size:.88em}\n"
          ".ab h3::before{content:counter(sc1)'.'counter(sc2)'.'counter(sc3)' ';color:#8b949e;font-weight:400;font-size:.88em}\n"
          ".ab h4::before{content:counter(sc1)'.'counter(sc2)'.'counter(sc3)'.'counter(sc4)' ';color:#8b949e;font-weight:400;font-size:.88em}\n"
          ".ab h5::before{content:counter(sc1)'.'counter(sc2)'.'counter(sc3)'.'counter(sc4)'.'counter(sc5)' ';color:#8b949e;font-weight:400;font-size:.88em}\n"
          /* 打印/PDF：彻底隐藏顶栏、全站文章目录、本文 TOC，并展开正文（多浏览器兼容） */
          "@media print{\n"
          "html,body{height:auto!important;max-height:none!important;overflow:visible!important;"
          "background:#fff!important;color:#1a1a2e!important;"
          "-webkit-print-color-adjust:exact;print-color-adjust:exact}\n"
          "body{display:block!important}\n"
          "nav.topbar,nav#sidebar,nav#toc,.sidebar,.toc,\n"
          ".st-top,.sidebar-body,.toc-top,.toc-body,\n"
          ".edit-btn,.copy-btn,.panel-toggle,.panel-label{display:none!important;"
          "width:0!important;height:0!important;max-height:0!important;overflow:hidden!important;"
          "visibility:hidden!important;position:absolute!important;left:-9999px!important;"
          "clip:rect(0,0,0,0)!important}\n"
          ".layout{display:block!important;flex:none!important;overflow:visible!important;"
          "height:auto!important;max-height:none!important}\n"
          ".content{display:block!important;overflow:visible!important;flex:none!important;"
          "max-height:none!important;height:auto!important;width:100%!important;"
          "padding:12px 16px!important;position:static!important}\n"
          "article,#article-body{overflow:visible!important;max-height:none!important}\n"
          ".ab pre,.ab table,.ab img,.ab .code-block{page-break-inside:avoid}\n"
          "}\n"
          "</style>\n</head>\n<body>\n", fp);

    fputs("<nav class=\"topbar\"><a href=\"", fp);
    fputs(rp, fp);
    fputs("notewikiindex.html\">\u6587\u7ae0\u7d22\u5f15</a> \u00b7 <a href=\"", fp);
    fputs(rp, fp);
    fputs("notewiki.html\">\u2190 NoteWiki</a>", fp);
    if (category && category[0]) { fputs(" / ", fp); wiki_fhtml(fp, category); }
    fputs(" <a class=\"edit-btn\" href=\"", fp);
    fputs(rp, fp);
    fputs("notewiki.html?edit=", fp);
    fputs(id, fp);
    fputs("\">\u270f \u7f16\u8f91</a>"
          "<button class=\"copy-btn\" id=\"export-md-btn\" onclick=\"exportMdZip()\">\u5bfc\u51fa MD ZIP</button>"
          "<button class=\"copy-btn\" id=\"export-pdf-btn\" onclick=\"exportPdf()\">\u5bfc\u51fa PDF</button>"
          "</nav>\n", fp);

    fputs("<div class=\"layout\">\n"
          "<nav class=\"sidebar\" id=\"sidebar\">"
          "<div class=\"st-top\">"
          "<span class=\"panel-label\">\u6587\u7ae0\u76ee\u5f55</span>"
          "<button class=\"panel-toggle\" id=\"sb-toggle\">&#9664;</button>"
          "</div>"
          "<div class=\"sidebar-body\" id=\"sidebar-body\"></div>"
          "</nav>\n"
          "<div class=\"content\">\n"
          "<article>\n<h1 class=\"at\">", fp);
    wiki_fhtml(fp, title);
    fputs("</h1>\n<div class=\"am\">\u66f4\u65b0\uff1a", fp);
    fputs(updated, fp);
    fputs("</div>\n<div class=\"ab\" id=\"article-body\">\n", fp);
    if (html_body) wiki_fwrite_body(fp, html_body, rp);
    fputs("\n</div>\n</article>\n</div>\n"
          "<nav class=\"toc\" id=\"toc\">"
          "<div class=\"toc-top\">"
          "<button class=\"panel-toggle\" id=\"toc-toggle\">&#9654;</button>"
          "<span class=\"panel-label\">\u672c\u6587\u76ee\u5f55</span>"
          "</div>"
          "<div class=\"toc-body\" id=\"toc-body\"></div>"
          "</nav>\n"
          "</div>\n", fp);

    fputs("<script>window.WIKI_CUR_ID=", fp);
    wiki_fjs(fp, id ? id : "");
    fputs(";</script>\n", fp);

    fputs("<script>\n"
          "function showToast(msg){\n"
          "  var t=document.getElementById('_wk_toast');\n"
          "  if(!t){\n"
          "    t=document.createElement('div');\n"
          "    t.id='_wk_toast';\n"
          "    t.style.cssText='position:fixed;bottom:28px;right:28px;z-index:99999;'\n"
          "      +'background:#238636;color:#fff;padding:10px 22px;'\n"
          "      +'border-radius:8px;font-size:14px;font-weight:500;'\n"
          "      +'box-shadow:0 4px 16px rgba(0,0,0,.6);display:none;pointer-events:none';\n"
          "    document.body.appendChild(t);\n"
          "  }\n"
          "  t.textContent=msg;\n"
          "  t.style.display='block';\n"
          "  clearTimeout(t._tid);\n"
          "  t._tid=setTimeout(function(){t.style.display='none';},2500);\n"
          "}\n"
          "function execCopy(text){\n"
          "  var ta=document.createElement('textarea');\n"
          "  ta.value=text;\n"
          "  ta.style.cssText='position:fixed;top:0;left:0;width:1px;height:1px;opacity:0';\n"
          "  document.body.appendChild(ta);\n"
          "  ta.focus();ta.select();ta.setSelectionRange(0,ta.value.length);\n"
          "  var ok=false;try{ok=document.execCommand('copy');}catch(e){}\n"
          "  document.body.removeChild(ta);\n"
          "  return ok;\n"
          "}\n"
          "function copyText(text,msg){\n"
          "  if(navigator.clipboard&&window.isSecureContext){\n"
          "    navigator.clipboard.writeText(text)\n"
          "      .then(function(){showToast(msg);})\n"
          "      .catch(function(){if(execCopy(text))showToast(msg);});\n"
          "  } else {\n"
          "    if(execCopy(text))showToast(msg);\n"
          "  }\n"
          "}\n"
          "function exportMdZip(){\n"
          "  if(window.location.protocol==='file:'){\n"
          "    showToast('\u79bb\u7ebf\u6a21\u5f0f\u4e0d\u652f\u6301\u5bfc\u51fa MD ZIP');\n"
          "    return;\n"
          "  }\n"
          "  var a=document.createElement('a');\n"
          "  a.href='/api/wiki-export-md-zip?id='+encodeURIComponent(window.WIKI_CUR_ID||'');\n"
          "  a.style.display='none';document.body.appendChild(a);a.click();document.body.removeChild(a);\n"
          "}\n"
          "/* exportPdf \u7531 sidebar.js \u6ce8\u5165\uff0c\u4e0e notewiki \u4e00\u81f4 */\n"
          "document.addEventListener('click', function(e){\n"
          "  var btn = e.target.closest('.ab .code-block .copy-btn');\n"
          "  if (!btn) return;\n"
          "  var pre = btn.parentElement ? btn.parentElement.querySelector('pre') : null;\n"
          "  var text = pre ? pre.textContent : '';\n"
          "  copyText(text, '\u2713 \u4ee3\u7801\u5df2\u590d\u5236\u5230\u526a\u8d34\u677f');\n"
          "  var orig = btn.textContent;\n"
          "  btn.textContent = '\u2713 \u5df2\u590d\u5236';\n"
          "  btn.classList.add('copied');\n"
          "  setTimeout(function(){btn.textContent = orig; btn.classList.remove('copied');}, 1600);\n"
          "});\n"
          "</script>\n", fp);

    fputs("<script src=\"", fp);
    fputs(rp, fp);
    fputs("sidebar.js\"></script>\n</body>\n</html>\n", fp);

    fclose(fp);
    return 0;
}

/* ── wiki_collect_dirs ────────────────────────────────────── */

static void wiki_collect_dirs(strbuf_t *sb, const char *base,
                               const char *rel, int *pfirst, int top)
{
    char full[1024];
    if (rel[0]) snprintf(full, sizeof(full), "%s/%s", base, rel);
    else        snprintf(full, sizeof(full), "%s", base);
    DIR *d = opendir(full);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (top && (strcmp(de->d_name,"md_db")==0 || strcmp(de->d_name,"uploads")==0)) continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", full, de->d_name);
        struct stat st; if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        char relpath[512];
        if (rel[0]) snprintf(relpath, sizeof(relpath), "%s/%s", rel, de->d_name);
        else        snprintf(relpath, sizeof(relpath), "%s", de->d_name);
        if (!*pfirst) SB_LIT(sb, ","); *pfirst = 0;
        sb_json_str(sb, relpath);
        wiki_collect_dirs(sb, base, relpath, pfirst, 0);
    }
    closedir(d);
}

/* ── wiki_extract_body ────────────────────────────────────── */

static void wiki_extract_body(const char *html_path, strbuf_t *hbody)
{
#define BODY_START "<div class=\"ab\" id=\"article-body\">\n"
#define BODY_END   "\n</div>\n</article>"
    strbuf_t hfull = {0};
    FILE *fp = fopen(html_path, "r");
    if (fp) {
        char buf[8192]; size_t nr;
        while ((nr = fread(buf, 1, sizeof(buf), fp)) > 0) sb_append(&hfull, buf, nr);
        fclose(fp);
    }
    if (hfull.data) {
        const char *start = strstr(hfull.data, BODY_START);
        if (start) {
            start += strlen(BODY_START);
            const char *end = strstr(start, BODY_END);
            if (end) sb_append(hbody, start, (size_t)(end - start));
        }
    }
    free(hfull.data);
#undef BODY_START
#undef BODY_END
}

/* ── wiki_rewrite_html (forward-declared above) ───────────── */

static void wiki_rewrite_html(const char *id, const char *title,
                               const char *cat, const char *updated)
{
    char html_path[1024];
    if (cat && cat[0])
        snprintf(html_path, sizeof(html_path), "%s/%s/%s.html", WIKI_ROOT, cat, id);
    else
        snprintf(html_path, sizeof(html_path), "%s/%s.html", WIKI_ROOT, id);
    strbuf_t hbody = {0};
    wiki_extract_body(html_path, &hbody);
    wiki_write_html_file(html_path, id, title, cat ? cat : "", updated,
                         hbody.data ? hbody.data : "");
    free(hbody.data);
}

/* ── wiki_search_dir ──────────────────────────────────────── */

#define WIKI_FIND_NONE ((size_t)-1)

/* 在 hay[0..haylen) 中找与 q（已小写）的不区分大小写子串，返回起始下标或 WIKI_FIND_NONE */
static size_t wiki_find_icase_submem(const unsigned char *hay, size_t haylen,
                                     const char *q, size_t qlen)
{
    if (!hay || !q || !qlen || haylen < qlen) return WIKI_FIND_NONE;
    for (size_t i = 0; i + qlen <= haylen; i++) {
        size_t j;
        for (j = 0; j < qlen; j++) {
            if (tolower(hay[i + j]) != (unsigned char)q[j]) break;
        }
        if (j == qlen) return i;
    }
    return WIKI_FIND_NONE;
}

/* 将 [pos, pos+match_len) 前后各取一段，折叠空白写入 out；cap 为允许达到的 out->len 上限 */
static void wiki_append_match_context(strbuf_t *out, const char *hay, size_t haylen,
                                      size_t pos, size_t match_len,
                                      size_t before, size_t after, size_t cap)
{
    if (!hay || haylen == 0 || pos > haylen || cap <= out->len + 8) return;
    if (match_len > haylen) match_len = haylen;
    if (pos + match_len > haylen) return;

    size_t start = pos > before ? pos - before : 0;
    while (start < haylen && ((unsigned char)hay[start] & 0xC0) == 0x80) start++;

    size_t end = pos + match_len + after;
    if (end > haylen) end = haylen;

    int lead_el = (start > 0);
    int trail_el = (end < haylen);

    if (lead_el && out->len + 3 < cap) SB_LIT(out, "\xe2\x80\xa6"); /* … */

    int prev_space = 0;
    for (size_t i = start; i < end && out->len + 4 < cap; i++) {
        unsigned char c = (unsigned char)hay[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            if (!prev_space) {
                if (SB_LIT(out, " ") < 0) return;
                prev_space = 1;
            }
            continue;
        }
        if (c == ' ') {
            if (!prev_space) {
                if (SB_LIT(out, " ") < 0) return;
                prev_space = 1;
            }
            continue;
        }
        if (c < 32) continue;
        prev_space = 0;
        if (sb_append(out, (const char *)(hay + i), 1) < 0) return;
    }

    if (trail_el && out->len + 3 < cap) SB_LIT(out, "\xe2\x80\xa6");
}

static void wiki_search_dir(strbuf_t *sb, int *pfirst, const char *dir, const char *q)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { wiki_search_dir(sb, pfirst, child, q); continue; }
        size_t nl = strlen(de->d_name);
        if (nl < 4 || strcmp(de->d_name + nl - 3, ".md") != 0) continue;
        FILE *fp = fopen(child, "r"); if (!fp) continue;
        char ml[4096] = {0}; fgets(ml, sizeof(ml), fp);
        strbuf_t body = {0}; char buf[8192]; size_t nr;
        while ((nr = fread(buf, 1, sizeof(buf), fp)) > 0) sb_append(&body, buf, nr);
        fclose(fp);
        char id[128]={0},title[512]={0},cat[512]={0},cre[64]={0},upd[64]={0};
        if (strncmp(ml,"<!--META ",9)==0) {
            char *end=strstr(ml,"-->"); if (end) { *end='\0';
                const char *mj=ml+9;
                json_get_str(mj,"id",id,sizeof(id)); json_get_str(mj,"title",title,sizeof(title));
                json_get_str(mj,"category",cat,sizeof(cat)); json_get_str(mj,"created",cre,sizeof(cre));
                json_get_str(mj,"updated",upd,sizeof(upd)); }
        }
        if (!id[0]) { free(body.data); continue; }
        size_t qlen = strlen(q);
        size_t ti = WIKI_FIND_NONE;
        size_t bi = WIKI_FIND_NONE;
        if (qlen > 0) {
            if (title[0])
                ti = wiki_find_icase_submem((const unsigned char *)title, strlen(title), q, qlen);
            if (body.data)
                bi = wiki_find_icase_submem((const unsigned char *)body.data, body.len, q, qlen);
        }
        strbuf_t snippet = {0};
        const size_t snip_cap = 960;
        if (ti != WIKI_FIND_NONE) {
            SB_LIT(&snippet, "\xe3\x80\x90\xe6\xa0\x87\xe9\xa2\x98\xe3\x80\x91"); /* 【标题】 */
            wiki_append_match_context(&snippet, title, strlen(title), ti, qlen,
                                      48, 96, snip_cap);
        }
        if (bi != WIKI_FIND_NONE) {
            if (snippet.len) {
                SB_LIT(&snippet, " ");
                SB_LIT(&snippet, "\xe3\x80\x90\xe6\xad\xa3\xe6\x96\x87\xe3\x80\x91"); /* 【正文】 */
            } else {
                SB_LIT(&snippet, "\xe3\x80\x90\xe6\xad\xa3\xe6\x96\x87\xe3\x80\x91");
            }
            wiki_append_match_context(&snippet, body.data, body.len, bi, qlen,
                                      64, 128, snip_cap);
        }
        free(body.data);
        if (ti == WIKI_FIND_NONE && bi == WIKI_FIND_NONE) {
            free(snippet.data);
            continue;
        }
        if (!*pfirst) SB_LIT(sb, ","); *pfirst = 0;
        SB_LIT(sb,"{\"id\":"); sb_json_str(sb,id);
        SB_LIT(sb,",\"title\":"); sb_json_str(sb,title);
        SB_LIT(sb,",\"category\":"); sb_json_str(sb,cat);
        SB_LIT(sb,",\"created\":"); sb_json_str(sb,cre);
        SB_LIT(sb,",\"updated\":"); sb_json_str(sb,upd);
        SB_LIT(sb,",\"snippet\":"); sb_json_str(sb, snippet.data ? snippet.data : "");
        SB_LIT(sb,"}");
        free(snippet.data);
    }
    closedir(d);
}

/* ── rmdir_recursive ──────────────────────────────────────── */

static void rmdir_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { rmdir(path); return; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) rmdir_recursive(child);
        else unlink(child);
    }
    closedir(d);
    rmdir(path);
}

/* ── GET /api/wiki-list ───────────────────────────────────── */

void handle_api_wiki_list(int client_fd)
{
    wiki_ensure_dirs();
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"articles\":[");
    int first = 1;
    wiki_scan_md_dir(&sb, &first, WIKI_MD_DB);
    SB_LIT(&sb, "],\"categories\":[");
    int firstcat = 1;
    wiki_collect_dirs(&sb, WIKI_ROOT, "", &firstcat, 1);
    SB_LIT(&sb, "]}");
    if (sb.data) send_json(client_fd, 200, "OK", sb.data, sb.len);
    else send_json(client_fd, 200, "OK", "{\"articles\":[],\"categories\":[]}", 30);
    free(sb.data);
}

/* ── GET /api/wiki-read ───────────────────────────────────── */

void handle_api_wiki_read(int client_fd, const char *path_qs)
{
    char id[128] = {0};
    const char *qs = strchr(path_qs, '?');
    if (qs) {
        const char *p = strstr(qs, "id=");
        if (p) { p += 3; size_t j=0; while(*p&&*p!='&'&&j<sizeof(id)-1) id[j++]=*p++; }
    }
    if (!id[0]) { send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing id\"}",33); return; }
    for (size_t i=0;id[i];i++) {
        unsigned char c=(unsigned char)id[i];
        if (!isalnum(c)&&c!='_'&&c!='-') {
            send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid id\"}",33); return;
        }
    }
    char path[768];
    if (wiki_md_find(path, sizeof(path), id) < 0) {
        send_json(client_fd,404,"Not Found","{\"ok\":false,\"error\":\"not found\"}",32); return;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) { send_json(client_fd,404,"Not Found","{\"ok\":false,\"error\":\"not found\"}",32); return; }
    char line[4096]; fgets(line, sizeof(line), fp);
    strbuf_t body = {0}; char buf[8192]; size_t nr;
    while ((nr = fread(buf,1,sizeof(buf),fp)) > 0) sb_append(&body,buf,nr);
    fclose(fp);
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"content\":"); sb_json_str(&sb, body.data ? body.data : "");
    SB_LIT(&sb, "}");
    free(body.data);
    if (sb.data) send_json(client_fd,200,"OK",sb.data,sb.len);
    else send_json(client_fd,500,"Internal Server Error","{\"ok\":false}",12);
    free(sb.data);
}

/* ── GET /api/wiki-export-md-zip ───────────────────────────── */
void handle_api_wiki_export_md_zip(int client_fd, const char *path_qs)
{
    char id[128] = {0};
    const char *qs = strchr(path_qs, '?');
    if (qs) {
        const char *p = strstr(qs, "id=");
        if (p) { p += 3; size_t j = 0; while (*p && *p != '&' && j < sizeof(id)-1) id[j++] = *p++; }
    }
    if (!id[0]) { send_json(client_fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"missing id\"}", 33); return; }
    for (size_t i = 0; id[i]; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!isalnum(c) && c != '_' && c != '-') {
            send_json(client_fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"invalid id\"}", 33); return;
        }
    }

    char md_path[768];
    if (wiki_md_find(md_path, sizeof(md_path), id) < 0) {
        send_json(client_fd, 404, "Not Found", "{\"ok\":false,\"error\":\"not found\"}", 32); return;
    }

    FILE *fp = fopen(md_path, "r");
    if (!fp) { send_json(client_fd, 404, "Not Found", "{\"ok\":false,\"error\":\"not found\"}", 32); return; }
    char meta_line[4096] = {0};
    fgets(meta_line, sizeof(meta_line), fp);
    strbuf_t body = {0}; char tmp[8192]; size_t nr;
    while ((nr = fread(tmp, 1, sizeof(tmp), fp)) > 0) sb_append(&body, tmp, nr);
    fclose(fp);

    _strnode_t *refs = NULL;
    wiki_collect_upload_refs_from_md(body.data ? body.data : "", &refs);
    wiki_md_rewrite_uploads_to_assets(&body);

    char work_root[1024];
    snprintf(work_root, sizeof(work_root), "/tmp/wiki_export_%ld_%ld", (long)getpid(), (long)time(NULL));
    char pkg_dir[1152];
    snprintf(pkg_dir, sizeof(pkg_dir), "%s/pkg", work_root);
    if (mkdir_p(pkg_dir) != 0) {
        _strlist_free(refs);
        free(body.data);
        send_json(client_fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"mkdir failed\"}", 35); return;
    }

    char md_out[1280];
    snprintf(md_out, sizeof(md_out), "%s/article.md", pkg_dir);
    fp = fopen(md_out, "wb");
    if (!fp) {
        _strlist_free(refs);
        free(body.data); rmdir_recursive(work_root);
        send_json(client_fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"write md\"}", 33); return;
    }
    if (body.data) fwrite(body.data, 1, body.len, fp);
    fclose(fp);
    free(body.data);

    for (_strnode_t *n = refs; n; n = n->next) {
        char src[1024], dst[1280], dst_dir[1152];
        snprintf(src, sizeof(src), "%s/%s", WIKI_UPLOADS, n->s);
        snprintf(dst, sizeof(dst), "%s/assets/%s", pkg_dir, n->s);
        snprintf(dst_dir, sizeof(dst_dir), "%s", dst);
        char *slash = strrchr(dst_dir, '/');
        if (slash) { *slash = '\0'; mkdir_p(dst_dir); }
        wiki_copy_file(src, dst);
    }
    _strlist_free(refs);

    char zip_path[1280];
    snprintf(zip_path, sizeof(zip_path), "%s/%s.zip", work_root, id);
    char cmd[3072];
    snprintf(cmd, sizeof(cmd), "cd \"%s\" && zip -q -r \"%s\" .", pkg_dir, zip_path);
    if (system(cmd) != 0) {
        rmdir_recursive(work_root);
        send_json(client_fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"zip failed\"}", 34); return;
    }

    char dl_name[192];
    snprintf(dl_name, sizeof(dl_name), "%s_md_bundle.zip", id);
    if (wiki_send_attachment_file(client_fd, zip_path, dl_name) < 0) {
        rmdir_recursive(work_root);
        send_json(client_fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"send failed\"}", 35); return;
    }
    rmdir_recursive(work_root);
}

/* ── POST /api/wiki-save ──────────────────────────────────── */

void handle_api_wiki_save(int client_fd, const char *body)
{
    wiki_ensure_dirs();
    char id[128]={0}, title[512]={0}, cat[512]={0}, created[64]={0};
    json_get_str(body,"id",id,sizeof(id));
    json_get_str(body,"title",title,sizeof(title));
    json_get_str(body,"category",cat,sizeof(cat));
    json_get_str(body,"created",created,sizeof(created));
    if (!title[0]) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing title\"}",38); return;
    }
    if (!register_subdir_safe(cat)) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid category\"}",41); return;
    }
    if (id[0]) {
        for (size_t i=0;id[i];i++) {
            unsigned char c=(unsigned char)id[i];
            if (!isalnum(c)&&c!='_'&&c!='-') {
                send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid id\"}",33); return;
            }
        }
    } else {
        wiki_gen_id(id, sizeof(id));
    }

    char force_str[8]={0}; json_get_str(body,"force",force_str,sizeof(force_str));
    int force = (strcmp(force_str,"true")==0);
    if (!force && id[0] && !created[0]) {
        char chk[768];
        if (wiki_md_find(chk, sizeof(chk), id) == 0) {
            send_json(client_fd, 409, "Conflict", "{\"ok\":false,\"error\":\"duplicate\"}", 31); return;
        }
    }
    char now[64]; wiki_now_iso(now, sizeof(now));
    if (!created[0]) strncpy(created, now, sizeof(created)-1);

    char *content = json_get_str_alloc(body, "content");
    char *html    = json_get_str_alloc(body, "html");
    if (!content || !html) {
        free(content); free(html);
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing content/html\"}",45); return;
    }

    char old_md_path[768] = {0}, old_cat[512] = {0};
    if (wiki_md_find(old_md_path, sizeof(old_md_path), id) == 0) {
        FILE *oldfp = fopen(old_md_path, "r");
        if (oldfp) {
            char oml[4096]={0}; fgets(oml, sizeof(oml), oldfp); fclose(oldfp);
            if (strncmp(oml,"<!--META ",9)==0) {
                char *oend=strstr(oml,"-->"); if (oend) { *oend='\0'; json_get_str(oml+9,"category",old_cat,sizeof(old_cat)); }
            }
        }
    }

    char md_path[768];
    wiki_md_write_path(md_path, sizeof(md_path), id, cat);
    FILE *fp = fopen(md_path, "wb");
    if (!fp) { free(content); free(html);
        send_json(client_fd,500,"Internal Server Error","{\"ok\":false,\"error\":\"write md\"}",33); return; }
    strbuf_t meta = {0};
    SB_LIT(&meta, "<!--META ");
    SB_LIT(&meta,"{\"id\":"); sb_json_str(&meta,id);
    SB_LIT(&meta,",\"title\":"); sb_json_str(&meta,title);
    SB_LIT(&meta,",\"category\":"); sb_json_str(&meta,cat);
    SB_LIT(&meta,",\"created\":"); sb_json_str(&meta,created);
    SB_LIT(&meta,",\"updated\":"); sb_json_str(&meta,now);
    SB_LIT(&meta, "}-->\n");
    if (meta.data) fwrite(meta.data,1,meta.len,fp);
    fwrite(content,1,strlen(content),fp);
    fclose(fp);
    free(meta.data); free(content);

    if (old_md_path[0] && strcmp(old_md_path, md_path) != 0) unlink(old_md_path);
    if (old_cat[0] && strcmp(old_cat, cat) != 0) {
        char old_html[1024];
        snprintf(old_html, sizeof(old_html), "%s/%s/%s.html", WIKI_ROOT, old_cat, id);
        unlink(old_html);
    }

    char html_path[1024];
    if (cat[0]) {
        char cat_dir[768]; snprintf(cat_dir, sizeof(cat_dir), "%s/%s", WIKI_ROOT, cat);
        mkdir_p(cat_dir);
        snprintf(html_path, sizeof(html_path), "%s/%s/%s.html", WIKI_ROOT, cat, id);
    } else {
        snprintf(html_path, sizeof(html_path), "%s/%s.html", WIKI_ROOT, id);
    }
    wiki_write_html_file(html_path, id, title, cat, now, html);
    free(html);

    const char *rel = html_path + strlen(WEB_ROOT) + 1;
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"id\":"); sb_json_str(&sb, id);
    SB_LIT(&sb, ",\"url\":\"/"); sb_append(&sb, rel, strlen(rel)); SB_LIT(&sb, "\"}");
    if (sb.data) send_json(client_fd,200,"OK",sb.data,sb.len);
    else send_json(client_fd,200,"OK","{\"ok\":true}",11);
    free(sb.data);
    LOG_INFO("wiki_save id=%s html=%s", id, html_path);
}

/* ── POST /api/wiki-delete ────────────────────────────────── */

void handle_api_wiki_delete(int client_fd, const char *body)
{
    char id[128]={0}; json_get_str(body,"id",id,sizeof(id));
    if (!id[0]) { send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing id\"}",34); return; }
    for (size_t i=0;id[i];i++) {
        unsigned char c=(unsigned char)id[i];
        if (!isalnum(c)&&c!='_'&&c!='-') {
            send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid id\"}",33); return;
        }
    }
    char cat[512]={0}, md_path[768];
    if (wiki_md_find(md_path, sizeof(md_path), id) == 0) {
        FILE *fp = fopen(md_path,"r");
        if (fp) {
            char line[4096]={0}; fgets(line,sizeof(line),fp); fclose(fp);
            if (strncmp(line,"<!--META ",9)==0) {
                char *end=strstr(line,"-->");
                if (end) { *end='\0'; json_get_str(line+9,"category",cat,sizeof(cat)); }
            }
        }
        unlink(md_path);
    }
    char html_path[1024];
    if (cat[0]) snprintf(html_path,sizeof(html_path),"%s/%s/%s.html",WIKI_ROOT,cat,id);
    else        snprintf(html_path,sizeof(html_path),"%s/%s.html",WIKI_ROOT,id);
    unlink(html_path);
    send_json(client_fd,200,"OK","{\"ok\":true}",11);
    LOG_INFO("wiki_delete id=%s",id);
}

/* ── GET /api/wiki-search ─────────────────────────────────── */

void handle_api_wiki_search(int client_fd, const char *path_qs)
{
    char q[256] = {0};
    const char *qs = strchr(path_qs, '?');
    if (qs) {
        const char *p = strstr(qs, "q=");
        if (p) { p += 2; size_t j=0; while(*p&&*p!='&'&&j<sizeof(q)-1) q[j++]=*p++; }
    }
    url_decode_report_fn(q);
    if (!q[0]) { send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing q\"}",32); return; }
    for (char *p=q; *p; p++) *p=(char)tolower((unsigned char)*p);

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"articles\":[");
    int first = 1;
    wiki_search_dir(&sb, &first, WIKI_MD_DB, q);
    SB_LIT(&sb, "]}");
    if (sb.data) send_json(client_fd, 200, "OK", sb.data, sb.len);
    else send_json(client_fd, 200, "OK", "{\"ok\":true,\"articles\":[]}", 24);
    free(sb.data);
}

/* ── GET /api/wiki-rebuild-html ───────────────────────────── */

void handle_api_wiki_rebuild_html(int client_fd)
{
    wiki_ensure_dirs();
    int count = 0;
    wiki_rebuild_md_dir(&count, WIKI_MD_DB);
    char resp[64];
    int rlen = snprintf(resp, sizeof(resp), "{\"ok\":true,\"rebuilt\":%d}", count);
    send_json(client_fd, 200, "OK", resp, (size_t)rlen);
    LOG_INFO("wiki_rebuild_html count=%d", count);
}

/* ── POST /api/wiki-rename-article ───────────────────────── */

void handle_api_wiki_rename_article(int client_fd, const char *body)
{
    wiki_ensure_dirs();
    char id[128]={0}, new_title[512]={0};
    json_get_str(body, "id", id, sizeof(id));
    json_get_str(body, "title", new_title, sizeof(new_title));
    if (!id[0] || !new_title[0]) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing id/title\"}",40); return;
    }
    for (size_t i=0; id[i]; i++) {
        unsigned char c=(unsigned char)id[i];
        if (!isalnum(c)&&c!='_'&&c!='-') {
            send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid id\"}",33); return;
        }
    }
    char md_path[768];
    if (wiki_md_find(md_path, sizeof(md_path), id) < 0) {
        send_json(client_fd,404,"Not Found","{\"ok\":false,\"error\":\"not found\"}",32); return;
    }
    FILE *fp = fopen(md_path, "r");
    if (!fp) { send_json(client_fd,404,"Not Found","{\"ok\":false,\"error\":\"not found\"}",32); return; }
    char meta_line[4096]={0}; fgets(meta_line, sizeof(meta_line), fp);
    strbuf_t cbuf={0}; char buf[8192]; size_t nr;
    while ((nr=fread(buf,1,sizeof(buf),fp))>0) sb_append(&cbuf,buf,nr);
    fclose(fp);

    char cat[512]={0}, created[64]={0};
    if (strncmp(meta_line,"<!--META ",9)==0) {
        char *end=strstr(meta_line,"-->");
        if (end) { *end='\0'; const char *mj=meta_line+9;
            json_get_str(mj,"category",cat,sizeof(cat));
            json_get_str(mj,"created",created,sizeof(created)); }
    }
    char now[64]; wiki_now_iso(now, sizeof(now));

    fp = fopen(md_path, "wb");
    if (!fp) { free(cbuf.data);
        send_json(client_fd,500,"Internal Server Error","{\"ok\":false,\"error\":\"write md\"}",33); return; }
    strbuf_t mb={0};
    SB_LIT(&mb,"<!--META "); SB_LIT(&mb,"{\"id\":"); sb_json_str(&mb,id);
    SB_LIT(&mb,",\"title\":"); sb_json_str(&mb,new_title);
    SB_LIT(&mb,",\"category\":"); sb_json_str(&mb,cat);
    SB_LIT(&mb,",\"created\":"); sb_json_str(&mb,created);
    SB_LIT(&mb,",\"updated\":"); sb_json_str(&mb,now);
    SB_LIT(&mb,"}-->\n");
    if (mb.data) fwrite(mb.data,1,mb.len,fp);
    if (cbuf.data) fwrite(cbuf.data,1,cbuf.len,fp);
    fclose(fp); free(mb.data); free(cbuf.data);

    wiki_rewrite_html(id, new_title, cat, now);
    send_json(client_fd,200,"OK","{\"ok\":true}",11);
    LOG_INFO("wiki_rename_article id=%s new_title=%s", id, new_title);
}

/* ── POST /api/wiki-rename-cat ────────────────────────────── */

void handle_api_wiki_rename_cat(int client_fd, const char *body)
{
    wiki_ensure_dirs();
    char old_path[512]={0}, new_name[256]={0};
    json_get_str(body, "old_path", old_path, sizeof(old_path));
    json_get_str(body, "new_name", new_name, sizeof(new_name));
    if (!old_path[0] || !new_name[0]) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing params\"}",38); return;
    }
    if (!register_subdir_safe(old_path) || !register_subdir_safe(new_name)) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid path\"}",37); return;
    }
    char new_path[512]={0};
    const char *slash = strrchr(old_path, '/');
    if (slash)
        snprintf(new_path,sizeof(new_path),"%.*s/%s",(int)(slash-old_path),old_path,new_name);
    else
        snprintf(new_path,sizeof(new_path),"%s",new_name);

    char new_full_root[1024], old_full_root[1024];
    snprintf(old_full_root,sizeof(old_full_root),"%s/%s",WIKI_ROOT,old_path);
    snprintf(new_full_root,sizeof(new_full_root),"%s/%s",WIKI_ROOT,new_path);
    struct stat st;
    if (stat(new_full_root,&st)==0) {
        send_json(client_fd,409,"Conflict","{\"ok\":false,\"error\":\"already exists\"}",38); return;
    }
    if (rename(old_full_root,new_full_root)!=0 && errno!=ENOENT) {
        char err[128]; snprintf(err,sizeof(err),"{\"ok\":false,\"error\":\"%s\"}",strerror(errno));
        send_json(client_fd,500,"Internal Server Error",err,strlen(err)); return;
    }
    char old_full_md[1024], new_full_md[1024];
    snprintf(old_full_md,sizeof(old_full_md),"%s/%s",WIKI_MD_DB,old_path);
    snprintf(new_full_md,sizeof(new_full_md),"%s/%s",WIKI_MD_DB,new_path);
    rename(old_full_md,new_full_md);

    size_t old_len = strlen(old_path);
    wiki_update_cat_in_dir(WIKI_MD_DB, old_path, new_path, old_len);
    send_json(client_fd,200,"OK","{\"ok\":true}",11);
    LOG_INFO("wiki_rename_cat old=%s new=%s", old_path, new_path);
}

/* ── POST /api/wiki-delete-cat ────────────────────────────── */

void handle_api_wiki_delete_cat(int client_fd, const char *body)
{
    wiki_ensure_dirs();
    char path[512]={0}; json_get_str(body,"path",path,sizeof(path));
    if (!path[0]) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing path\"}",37); return;
    }
    if (!register_subdir_safe(path)) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid path\"}",37); return;
    }
    char md_cat_dir[1024]; snprintf(md_cat_dir, sizeof(md_cat_dir), "%s/%s", WIKI_MD_DB, path);
    if (wiki_md_dir_has_md(md_cat_dir)) {
        send_json(client_fd,409,"Conflict","{\"ok\":false,\"error\":\"not empty\"}", 30); return;
    }
    char full_root[1024], full_md[1024];
    snprintf(full_root,sizeof(full_root),"%s/%s",WIKI_ROOT,path);
    snprintf(full_md,  sizeof(full_md),  "%s/%s",WIKI_MD_DB,path);
    rmdir_recursive(full_md);
    rmdir_recursive(full_root);
    send_json(client_fd,200,"OK","{\"ok\":true}",11);
    LOG_INFO("wiki_delete_cat path=%s", path);
}

/* ── POST /api/wiki-move-article ──────────────────────────── */

void handle_api_wiki_move_article(int client_fd, const char *body)
{
    wiki_ensure_dirs();
    char id[128]={0}, new_cat[512]={0};
    json_get_str(body, "id", id, sizeof(id));
    json_get_str(body, "category", new_cat, sizeof(new_cat));
    if (!id[0]) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing id\"}",33); return;
    }
    for (size_t i=0; id[i]; i++) {
        unsigned char c=(unsigned char)id[i];
        if (!isalnum(c)&&c!='_'&&c!='-') {
            send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid id\"}",33); return;
        }
    }
    char md_path[768];
    if (wiki_md_find(md_path, sizeof(md_path), id) < 0) {
        send_json(client_fd,404,"Not Found","{\"ok\":false,\"error\":\"not found\"}",32); return;
    }
    FILE *fp = fopen(md_path,"r");
    if (!fp) { send_json(client_fd,404,"Not Found","{\"ok\":false,\"error\":\"not found\"}",32); return; }
    char meta_line[4096]={0}; fgets(meta_line,sizeof(meta_line),fp);
    strbuf_t cbuf={0}; char buf[8192]; size_t nr;
    while ((nr=fread(buf,1,sizeof(buf),fp))>0) sb_append(&cbuf,buf,nr);
    fclose(fp);

    char old_cat[512]={0}, title[512]={0}, created[64]={0};
    if (strncmp(meta_line,"<!--META ",9)==0) {
        char *mend=strstr(meta_line,"-->");
        if (mend) { *mend='\0'; const char *mj=meta_line+9;
            json_get_str(mj,"category",old_cat,sizeof(old_cat));
            json_get_str(mj,"title",title,sizeof(title));
            json_get_str(mj,"created",created,sizeof(created)); }
    }

    if (strcmp(old_cat,new_cat)==0) {
        free(cbuf.data);
        send_json(client_fd,200,"OK","{\"ok\":true}",11); return;
    }

    char old_html[1024];
    if (old_cat[0]) snprintf(old_html,sizeof(old_html),"%s/%s/%s.html",WIKI_ROOT,old_cat,id);
    else            snprintf(old_html,sizeof(old_html),"%s/%s.html",WIKI_ROOT,id);
    char new_html[1024];
    if (new_cat[0]) snprintf(new_html,sizeof(new_html),"%s/%s/%s.html",WIKI_ROOT,new_cat,id);
    else            snprintf(new_html,sizeof(new_html),"%s/%s.html",WIKI_ROOT,id);

    strbuf_t hbody = {0};
    wiki_extract_body(old_html, &hbody);

    if (new_cat[0]) {
        char new_dir[1024]; snprintf(new_dir,sizeof(new_dir),"%s/%s",WIKI_ROOT,new_cat);
        mkdir_p(new_dir);
    }

    char now[64]; wiki_now_iso(now,sizeof(now));
    wiki_write_html_file(new_html, id, title, new_cat, now, hbody.data ? hbody.data : "");
    free(hbody.data);
    unlink(old_html);

    char new_md_path[768];
    wiki_md_write_path(new_md_path, sizeof(new_md_path), id, new_cat);
    fp = fopen(new_md_path,"wb");
    if (fp) {
        strbuf_t mb={0};
        SB_LIT(&mb,"<!--META "); SB_LIT(&mb,"{\"id\":"); sb_json_str(&mb,id);
        SB_LIT(&mb,",\"title\":"); sb_json_str(&mb,title);
        SB_LIT(&mb,",\"category\":"); sb_json_str(&mb,new_cat);
        SB_LIT(&mb,",\"created\":"); sb_json_str(&mb,created);
        SB_LIT(&mb,",\"updated\":"); sb_json_str(&mb,now);
        SB_LIT(&mb,"}-->\n");
        if (mb.data) fwrite(mb.data,1,mb.len,fp);
        if (cbuf.data) fwrite(cbuf.data,1,cbuf.len,fp);
        fclose(fp); free(mb.data);
    }
    free(cbuf.data);
    if (strcmp(md_path, new_md_path) != 0) unlink(md_path);

    send_json(client_fd,200,"OK","{\"ok\":true}",11);
    LOG_INFO("wiki_move_article id=%s old_cat=%s new_cat=%s", id, old_cat, new_cat);
}

/* ── POST /api/wiki-mkdir ─────────────────────────────────── */

void handle_api_wiki_mkdir(int client_fd, const char *body)
{
    wiki_ensure_dirs();
    char path[512]={0}; json_get_str(body,"path",path,sizeof(path));
    if (!path[0]) { send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing path\"}",37); return; }
    if (!register_subdir_safe(path)) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid path\"}",37); return;
    }
    char full[1024]; snprintf(full,sizeof(full),"%s/%s",WIKI_ROOT,path);
    if (mkdir_p(full)!=0 && errno!=EEXIST) {
        char err[128]; snprintf(err,sizeof(err),"{\"ok\":false,\"error\":\"%s\"}",strerror(errno));
        send_json(client_fd,500,"Internal Server Error",err,strlen(err)); return;
    }
    char full_md[1024]; snprintf(full_md,sizeof(full_md),"%s/%s",WIKI_MD_DB,path);
    mkdir_p(full_md);
    send_json(client_fd,200,"OK","{\"ok\":true}",11);
}

/* ── POST /api/wiki-cleanup-uploads ──────────────────────── */

static void _strlist_add(_strnode_t **head, const char *s, size_t len)
{
    char *dup = malloc(len + 1);
    if (!dup) return;
    memcpy(dup, s, len); dup[len] = '\0';
    _strnode_t *n = malloc(sizeof(_strnode_t));
    if (!n) { free(dup); return; }
    n->s = dup; n->next = *head; *head = n;
}

static int _strlist_has(_strnode_t *head, const char *s)
{
    while (head) { if (strcmp(head->s, s) == 0) return 1; head = head->next; }
    return 0;
}

static void _strlist_free(_strnode_t *head)
{
    while (head) { _strnode_t *n = head->next; free(head->s); free(head); head = n; }
}

static void collect_upload_refs(const char *dir, _strnode_t **refs)
{
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { collect_upload_refs(child, refs); continue; }
        size_t nl = strlen(de->d_name);
        if (nl < 4 || strcmp(de->d_name + nl - 3, ".md") != 0) continue;
        FILE *fp = fopen(child, "r"); if (!fp) continue;
        char skip[4096]; fgets(skip, sizeof(skip), fp);
        strbuf_t buf = {0}; char tmp[8192]; size_t nr;
        while ((nr = fread(tmp, 1, sizeof(tmp), fp)) > 0) sb_append(&buf, tmp, nr);
        fclose(fp);
        if (!buf.data) continue;
        const char *marker = "/wiki/uploads/"; size_t mlen = strlen(marker);
        const char *p = buf.data;
        while ((p = strstr(p, marker)) != NULL) {
            p += mlen;
            const char *end = p;
            while (*end && *end != ')' && *end != '"' && *end != '\'' && !isspace((unsigned char)*end)) end++;
            size_t plen = (size_t)(end - p);
            if (plen > 0 && plen < 768) _strlist_add(refs, p, plen);
            p = end;
        }
        free(buf.data);
    }
    closedir(d);
}

static void cleanup_unreferenced(const char *dir, const char *rel,
                                  _strnode_t *refs, int *pdel, int *pkept)
{
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st; if (stat(child, &st) != 0) continue;
        char relc[768];
        if (rel[0]) snprintf(relc, sizeof(relc), "%s/%s", rel, de->d_name);
        else        snprintf(relc, sizeof(relc), "%s", de->d_name);
        if (S_ISDIR(st.st_mode)) { cleanup_unreferenced(child, relc, refs, pdel, pkept); continue; }
        if (_strlist_has(refs, relc)) { (*pkept)++; }
        else { unlink(child); (*pdel)++; LOG_INFO("cleanup_uploads: delete %s", child); }
    }
    closedir(d);
}

void handle_api_wiki_cleanup_uploads(int client_fd)
{
    wiki_ensure_dirs();
    _strnode_t *refs = NULL;
    collect_upload_refs(WIKI_MD_DB, &refs);
    int deleted = 0, kept = 0;
    cleanup_unreferenced(WIKI_UPLOADS, "", refs, &deleted, &kept);
    _strlist_free(refs);
    char resp[128];
    int rlen = snprintf(resp, sizeof(resp), "{\"ok\":true,\"deleted\":%d,\"kept\":%d}", deleted, kept);
    send_json(client_fd, 200, "OK", resp, (size_t)rlen);
    LOG_INFO("wiki_cleanup_uploads deleted=%d kept=%d", deleted, kept);
}

/* ── POST /api/wiki-upload ────────────────────────────────── */

void handle_api_wiki_upload(int client_fd, const char *req_headers,
                             const char *body, size_t body_len)
{
    wiki_ensure_dirs();
    char filename[256]={0};
    http_header_value(req_headers,"X-Wiki-Filename",filename,sizeof(filename));
    url_decode_report_fn(filename);
    if (!filename[0]) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing filename\"}",41); return;
    }
    if (!wiki_upload_safe(filename)) {
        send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid filename\"}",42); return;
    }
    char cat[512]={0};
    http_header_value(req_headers,"X-Wiki-Category",cat,sizeof(cat));
    url_decode_report_fn(cat);
    char upload_dir[1024];
    if (cat[0] && register_subdir_safe(cat)) {
        snprintf(upload_dir, sizeof(upload_dir), "%s/%s", WIKI_UPLOADS, cat);
        mkdir_p(upload_dir);
    } else {
        snprintf(upload_dir, sizeof(upload_dir), "%s", WIKI_UPLOADS);
    }
    char filepath[1024]; snprintf(filepath,sizeof(filepath),"%s/%s",upload_dir,filename);
    FILE *fp = fopen(filepath,"wb");
    if (!fp) { send_json(client_fd,500,"Internal Server Error","{\"ok\":false,\"error\":\"open\"}",29); return; }
    if (body_len>0) fwrite(body,1,body_len,fp);
    fclose(fp);
    strbuf_t sb={0};
    SB_LIT(&sb,"{\"ok\":true,\"url\":\"/wiki/uploads/");
    if (cat[0] && register_subdir_safe(cat)) {
        sb_append(&sb,cat,strlen(cat)); SB_LIT(&sb,"/");
    }
    sb_append(&sb,filename,strlen(filename));
    SB_LIT(&sb,"\"}");
    if (sb.data) send_json(client_fd,200,"OK",sb.data,sb.len);
    else send_json(client_fd,200,"OK","{\"ok\":true}",11);
    free(sb.data);
    LOG_INFO("wiki_upload %s (%zu bytes)",filepath,body_len);
}
