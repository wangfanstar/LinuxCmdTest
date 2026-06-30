#include "wiki.h"
#include "http_handler.h"
#include "http_utils.h"
#include "log.h"
#include "platform.h"
#include "auth_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#define open  _open
#define read  _read
#define close _close
#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#endif
#endif
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>

/* ── 路径常量 ─────────────────────────────────────────────── */

#define WIKI_ROOT      WEB_ROOT "/wiki"
#define WIKI_MD_DB     WIKI_ROOT "/md_db"
#define WIKI_UPLOADS   WIKI_ROOT "/uploads"
#define WIKI_TRASH     WIKI_ROOT "/trash"
#define WIKI_TRASH_MD  WIKI_TRASH "/md"
#define WIKI_TRASH_HTML WIKI_TRASH "/html"
#define WIKI_INDEX_FILE WIKI_ROOT "/wiki-index.json"
#define WIKI_INDEX_TMP  WIKI_ROOT "/wiki-index.json.tmp"

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
    time_t t = time(NULL);
    struct tm tm;
    platform_localtime_wall(&t, &tm);
    unsigned long tid = (unsigned long)pthread_self() & 0xFFFF;
    snprintf(buf, sz, "note_%04d%02d%02d_%02d%02d%02d_%04lx",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, tid);
}

static void wiki_now_iso(char *buf, size_t sz)
{
    time_t t = time(NULL);
    struct tm tm;
    platform_gmtime_utc(&t, &tm);
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

/* 由 md 文件绝对路径得到 category（相对 md_db）与 id（不含 .md） */
static void wiki_cat_id_from_md_abspath(const char *abspath, char *cat, size_t catsz, char *id, size_t idsz)
{
    if (cat) cat[0] = '\0';
    if (id) id[0] = '\0';
    if (!abspath || !cat || !id) return;
    size_t rl = strlen(WIKI_MD_DB);
    if (strncmp(abspath, WIKI_MD_DB, rl) != 0) return;
    const char *p = abspath + rl;
    while (*p == '/' || *p == '\\') p++;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", p);
    for (char *q = tmp; *q; q++) {
        if (*q == '\\') *q = '/';
    }
    char *slash = strrchr(tmp, '/');
    if (!slash) {
        snprintf(id, idsz, "%s", tmp);
        size_t li = strlen(id);
        if (li > 3 && strcmp(id + li - 3, ".md") == 0) id[li - 3] = '\0';
        return;
    }
    *slash = '\0';
    snprintf(cat, catsz, "%s", tmp);
    snprintf(id, idsz, "%s", slash + 1);
    size_t li = strlen(id);
    if (li > 3 && strcmp(id + li - 3, ".md") == 0) id[li - 3] = '\0';
}

/* 从正文提取首个 # 标题；若无则 out 为空 */
static void wiki_extract_md_title(const char *md_text, char *out, size_t outsz)
{
    if (!out || outsz == 0) return;
    out[0] = '\0';
    if (!md_text) return;
    const char *p = md_text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
        const char *s = p;
        while (linelen > 0 && isspace((unsigned char)*s)) {
            s++;
            linelen--;
        }
        if (linelen > 0 && *s == '#') {
            s++;
            while (*s == '#' || isspace((unsigned char)*s)) s++;
            size_t tlen = nl ? (size_t)(nl - s) : strlen(s);
            while (tlen > 0 && (s[tlen - 1] == '\r' || s[tlen - 1] == '\n')) tlen--;
            while (tlen > 0 && isspace((unsigned char)s[tlen - 1])) tlen--;
            if (tlen >= outsz) tlen = outsz - 1;
            memcpy(out, s, tlen);
            out[tlen] = '\0';
            return;
        }
        if (!nl) break;
        p = nl + 1;
    }
}

static void wiki_append_article_json_record(strbuf_t *sb, const char *id, const char *title,
                                            const char *cat, const char *cre, const char *upd,
                                            const char *last_author, const char *authors_json_arr)
{
    SB_LIT(sb, "{\"id\":");
    sb_json_str(sb, id);
    SB_LIT(sb, ",\"title\":");
    sb_json_str(sb, title ? title : "");
    SB_LIT(sb, ",\"category\":");
    sb_json_str(sb, cat ? cat : "");
    SB_LIT(sb, ",\"created\":");
    sb_json_str(sb, cre ? cre : "");
    SB_LIT(sb, ",\"updated\":");
    sb_json_str(sb, upd ? upd : "");
    SB_LIT(sb, ",\"lastAuthor\":");
    sb_json_str(sb, last_author && last_author[0] ? last_author : "Admin");
    SB_LIT(sb, ",\"authors\":");
    if (authors_json_arr && authors_json_arr[0])
        sb_append(sb, authors_json_arr, strlen(authors_json_arr));
    else
        SB_LIT(sb, "[\"Admin\"]");
    SB_LIT(sb, "}");
}

static int json_get_raw(const char *json, const char *key,
                        char *out, size_t len);

static void wiki_scan_md_dir(strbuf_t *sb, int *pfirst, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            wiki_scan_md_dir(sb, pfirst, child);
            continue;
        }
        size_t nl = strlen(de->d_name);
        if (nl < 4 || strcmp(de->d_name + nl - 3, ".md") != 0) continue;
        FILE *fp = fopen(child, "rb");
        if (!fp) continue;
        char line[4096] = {0};
        int ok = (fgets(line, sizeof(line), fp) != NULL);
        strbuf_t rest = {0};
        char buf[8192];
        size_t nr;
        while (ok && (nr = fread(buf, 1, sizeof(buf), fp)) > 0)
            sb_append(&rest, buf, nr);
        fclose(fp);
        if (!ok) {
            free(rest.data);
            continue;
        }

        char path_cat[512] = {0}, path_id[128] = {0};
        wiki_cat_id_from_md_abspath(child, path_cat, sizeof(path_cat), path_id, sizeof(path_id));

        char id[128] = {0}, title[512] = {0}, cat[512] = {0}, cre[64] = {0}, upd[64] = {0};
        char last_author_out[128] = {0};
        char authors_json_out[2048] = {0};

        char now_iso[64];
        wiki_now_iso(now_iso, sizeof(now_iso));

        if (strncmp(line, "<!--META ", 9) == 0) {
            char *end = strstr(line, "-->");
            if (!end) {
                free(rest.data);
                continue;
            }
            *end = '\0';
            const char *mj = line + 9;
            json_get_str(mj, "id", id, sizeof(id));
            json_get_str(mj, "title", title, sizeof(title));
            json_get_str(mj, "category", cat, sizeof(cat));
            json_get_str(mj, "created", cre, sizeof(cre));
            json_get_str(mj, "updated", upd, sizeof(upd));
            if (!id[0]) {
                free(rest.data);
                continue;
            }
            if (!cre[0]) strncpy(cre, now_iso, sizeof(cre) - 1);
            if (!upd[0]) strncpy(upd, now_iso, sizeof(upd) - 1);
            /* last_author 与 lastAuthor 兼容；无首行 META 的 md 作者仅在 SQLite 表 */
            json_get_str(mj, "last_author", last_author_out, sizeof(last_author_out));
            if (!last_author_out[0]) json_get_str(mj, "lastAuthor", last_author_out, sizeof(last_author_out));
            json_get_raw(mj, "authors", authors_json_out, sizeof(authors_json_out));
            (void)auth_wiki_md_meta_upsert_scan_meta(id, title, cat, cre, upd);
        } else {
            snprintf(id, sizeof(id), "%s", path_id);
            snprintf(cat, sizeof(cat), "%s", path_cat);
            strbuf_t fulltxt = {0};
            sb_append(&fulltxt, line, strlen(line));
            if (rest.data && rest.len)
                sb_append(&fulltxt, rest.data, rest.len);
            wiki_extract_md_title(fulltxt.data ? fulltxt.data : "", title, sizeof(title));
            free(fulltxt.data);
            if (!title[0]) snprintf(title, sizeof(title), "%s", id);
            strncpy(cre, now_iso, sizeof(cre) - 1);
            strncpy(upd, now_iso, sizeof(upd) - 1);
            (void)auth_wiki_md_meta_ensure_scan_plain(id, cat, title, now_iso);
        }
        free(rest.data);

        wiki_md_meta_row_t row;
        memset(&row, 0, sizeof(row));
        if (auth_wiki_md_meta_get(id, &row) == 0 && row.found) {
            if (strncmp(line, "<!--META ", 9) != 0) {
                if (row.created[0]) strncpy(cre, row.created, sizeof(cre) - 1);
                if (row.updated[0]) strncpy(upd, row.updated, sizeof(upd) - 1);
                if (row.title[0]) strncpy(title, row.title, sizeof(title) - 1);
                snprintf(last_author_out, sizeof(last_author_out), "%s",
                         row.last_author[0] ? row.last_author : "Admin");
                snprintf(authors_json_out, sizeof(authors_json_out), "%s",
                         row.authors_json[0] ? row.authors_json : "[\"Admin\"]");
            } else {
                /* 首行有 META 时，若行内未带作者或为占位，则用 wiki_md_meta 中保存的真实账号 */
                if (row.last_author[0] &&
                    (!last_author_out[0] || strcmp(last_author_out, "Admin") == 0)) {
                    snprintf(last_author_out, sizeof(last_author_out), "%s", row.last_author);
                }
                if (row.authors_json[0] &&
                    (!authors_json_out[0] || strcmp(authors_json_out, "[\"Admin\"]") == 0 ||
                     strcmp(authors_json_out, "[]") == 0)) {
                    snprintf(authors_json_out, sizeof(authors_json_out), "%s", row.authors_json);
                }
            }
        }

        if (!*pfirst) SB_LIT(sb, ",");
        *pfirst = 0;
        wiki_append_article_json_record(sb, id, title, cat, cre, upd, last_author_out, authors_json_out);
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
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            wiki_rebuild_md_dir(pcount, child);
            continue;
        }
        size_t nl = strlen(de->d_name);
        if (nl < 4 || strcmp(de->d_name + nl - 3, ".md") != 0) continue;
        FILE *fp = fopen(child, "rb");
        if (!fp) continue;
        char ml[4096] = {0};
        fgets(ml, sizeof(ml), fp);
        fclose(fp);
        char id[128] = {0}, title[512] = {0}, cat[512] = {0}, upd[64] = {0};
        char now_iso[64];
        wiki_now_iso(now_iso, sizeof(now_iso));

        if (strncmp(ml, "<!--META ", 9) == 0) {
            char *mend = strstr(ml, "-->");
            if (!mend) continue;
            *mend = '\0';
            const char *mj = ml + 9;
            json_get_str(mj, "id", id, sizeof(id));
            json_get_str(mj, "title", title, sizeof(title));
            json_get_str(mj, "category", cat, sizeof(cat));
            json_get_str(mj, "updated", upd, sizeof(upd));
            if (!id[0]) continue;
            if (!upd[0]) strncpy(upd, now_iso, sizeof(upd) - 1);
        } else {
            wiki_cat_id_from_md_abspath(child, cat, sizeof(cat), id, sizeof(id));
            if (!id[0]) continue;
            wiki_md_meta_row_t row;
            memset(&row, 0, sizeof(row));
            if (auth_wiki_md_meta_get(id, &row) == 0 && row.found) {
                if (row.updated[0]) strncpy(upd, row.updated, sizeof(upd) - 1);
                if (row.title[0]) strncpy(title, row.title, sizeof(title) - 1);
            }
            if (!upd[0]) strncpy(upd, now_iso, sizeof(upd) - 1);
            if (!title[0]) snprintf(title, sizeof(title), "%s", id);
        }
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
    for (const char *p = fn; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && c != '.' && c != '_' && c != '-') return 0;
    }
    const char *dot = strrchr(fn, '.');
    if (!dot) return 1; /* 支持无后缀文件 */
    static const char *exts[] = {
        ".jpg",".jpeg",".png",".gif",".svg",".webp",
        ".pdf",".txt",".md",".csv",".json",".log",
        ".c",".h",".cpp",".cc",".cxx",".hpp",".hh",
        ".sh",".bash",".zsh",".fish",
        ".py",".js",".mjs",".cjs",".ts",".tsx",
        ".java",".go",".rs",".php",".rb",".pl",".lua",
        ".sql",".xml",".yml",".yaml",".toml",".ini",".cfg",
        ".doc",".docx",".xls",".xlsx",".ppt",".pptx",
        ".zip",".tar",".gz",".bz2",".xz",".7z",".rar",
        ".mp3",".wav",".mp4",".mov",".avi",NULL
    };
    for (int i = 0; exts[i]; i++)
        if (strcasecmp(dot, exts[i]) == 0) return 1;
    return 0;
}

static int wiki_ref_list_has(_strnode_t *head, const char *s)
{
    while (head) {
        if (strcmp(head->s, s) == 0) return 1;
        head = head->next;
    }
    return 0;
}

static int wiki_is_image_ref(const char *path)
{
    const char *dot = strrchr(path ? path : "", '.');
    if (!dot) return 0;
    return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 ||
           strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".gif") == 0 ||
           strcasecmp(dot, ".svg") == 0 || strcasecmp(dot, ".webp") == 0 ||
           strcasecmp(dot, ".bmp") == 0 || strcasecmp(dot, ".ico") == 0;
}

/* 生成不与现有文件冲突的上传文件名：
   先尝试原名；若已存在则追加 _1、_2... */
static int wiki_upload_unique_name(char *out, size_t outsz,
                                   const char *dir, const char *orig)
{
    if (!out || outsz == 0 || !dir || !orig || !orig[0]) return -1;
    snprintf(out, outsz, "%s", orig);
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", dir, out);
    struct stat st;
    if (stat(path, &st) != 0) return 0; /* 不存在，直接可用 */

    const char *dot = strrchr(orig, '.');
    char name[256] = {0};
    char ext[64] = {0};
    if (dot && dot != orig) {
        size_t nlen = (size_t)(dot - orig);
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, orig, nlen);
        name[nlen] = '\0';
        snprintf(ext, sizeof(ext), "%s", dot);
    } else {
        snprintf(name, sizeof(name), "%s", orig);
        ext[0] = '\0';
    }

    for (int i = 1; i < 10000; i++) {
        snprintf(out, outsz, "%s_%d%s", name, i, ext);
        snprintf(path, sizeof(path), "%s/%s", dir, out);
        if (stat(path, &st) != 0) return 0;
    }
    return -1;
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

static int wiki_fsync_file(FILE *fp)
{
    if (!fp) return -1;
    if (fflush(fp) != 0) return -1;
#ifdef _WIN32
    return _commit(_fileno(fp));
#else
    return fsync(fileno(fp));
#endif
}

static int wiki_rename_replace(const char *tmp_path, const char *final_path)
{
#ifdef _WIN32
    unlink(final_path);
#endif
    return rename(tmp_path, final_path);
}

static int wiki_send_download_file(http_sock_t fd, const char *filepath,
                                   const char *download_name, const char *content_type)
{
    struct stat st;
    if (stat(filepath, &st) < 0 || S_ISDIR(st.st_mode)) return -1;
    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) return -1;

    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Content-Length: %ld\r\n"
        "Cache-Control: no-store\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        content_type, download_name, (long)st.st_size);
    (void)http_sock_send_all(fd, header, (size_t)hlen);

    char buf[65536];
    ssize_t n;
    while ((n = read(file_fd, buf, sizeof(buf))) > 0) {
        if (http_sock_send_all(fd, buf, (size_t)n) < 0) {
            break;
        }
    }
    close(file_fd);
    return 0;
}

static int wiki_send_attachment_file(http_sock_t fd, const char *filepath, const char *download_name)
{
    return wiki_send_download_file(fd, filepath, download_name, "application/zip");
}

static int wiki_title_to_filename_base(const char *title, char *out, size_t outsz)
{
    size_t j = 0;
    if (!title || !out || outsz < 2) return -1;
    for (size_t i = 0; title[i] && j + 1 < outsz; i++) {
        unsigned char c = (unsigned char)title[i];
        if (isalnum(c)) out[j++] = (char)c;
        else if (c == ' ' || c == '-' || c == '_') out[j++] = '_';
    }
    while (j > 0 && out[j - 1] == '_') j--;
    if (j == 0) {
        const char *def = "wiki_export";
        snprintf(out, outsz, "%s", def);
        return 0;
    }
    out[j] = '\0';
    return 0;
}

static int wiki_run_wkhtmltopdf_with_outline(const char *in_html, const char *out_pdf)
{
    char cmd[4096];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
             "wkhtmltopdf --quiet --encoding utf-8 --enable-local-file-access "
             "--outline --outline-depth 6 "
             "--footer-right \"[page] / [toPage]\" --footer-font-size 9 --footer-spacing 4 "
             "\"%s\" \"%s\"",
             in_html, out_pdf);
    return system(cmd);
#else
    /* Linux: first try wkhtmltopdf directly, then try xvfb-run for headless servers. */
    snprintf(cmd, sizeof(cmd),
             "wkhtmltopdf --quiet --encoding utf-8 --enable-local-file-access "
             "--outline --outline-depth 6 "
             "--footer-right \"[page] / [toPage]\" --footer-font-size 9 --footer-spacing 4 "
             "\"%s\" \"%s\"",
             in_html, out_pdf);
    if (system(cmd) == 0) return 0;

    snprintf(cmd, sizeof(cmd),
             "xvfb-run -a wkhtmltopdf --quiet --encoding utf-8 --enable-local-file-access "
             "--outline --outline-depth 6 "
             "--footer-right \"[page] / [toPage]\" --footer-font-size 9 --footer-spacing 4 "
             "\"%s\" \"%s\"",
             in_html, out_pdf);
    return system(cmd);
#endif
}

static int wiki_has_wkhtmltopdf(void)
{
#ifdef _WIN32
    return system("where wkhtmltopdf >nul 2>nul") == 0;
#else
    return system("command -v wkhtmltopdf >/dev/null 2>&1") == 0;
#endif
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

static void wiki_write_related_attachments(FILE *fp, const char *html_body, const char *prefix)
{
    if (!fp || !html_body || !html_body[0]) return;
    const char *marker = "/wiki/uploads/";
    size_t mlen = strlen(marker);
    const char *p = html_body;
    _strnode_t *refs = NULL;

    while ((p = strstr(p, marker)) != NULL) {
        p += mlen;
        const char *end = p;
        while (*end && *end != ')' && *end != '"' && *end != '\'' &&
               *end != '<' && *end != '>' && !isspace((unsigned char)*end)) end++;
        size_t len = (size_t)(end - p);
        if (len > 0 && len < 768 && strstr(p, "..") == NULL && *p != '/') {
            char ref[768];
            memcpy(ref, p, len);
            ref[len] = '\0';
            if (!wiki_is_image_ref(ref) && !wiki_ref_list_has(refs, ref)) {
                _strlist_add(&refs, ref, strlen(ref));
            }
        }
        p = end;
    }

    if (!refs) return;

    fputs("\n<section class=\"rel-attachments\">\n"
          "<h2 id=\"related-attachments\">\u76f8\u5173\u9644\u4ef6</h2>\n"
          "<ul>\n", fp);
    for (_strnode_t *n = refs; n; n = n->next) {
        const char *base = strrchr(n->s, '/');
        base = base ? (base + 1) : n->s;
        fputs("<li><a href=\"", fp);
        fputs(prefix ? prefix : "", fp);
        fputs("uploads/", fp);
        wiki_fhtml(fp, n->s);
        fputs("\" target=\"_blank\" rel=\"noopener noreferrer\">", fp);
        wiki_fhtml(fp, base);
        fputs("</a></li>\n", fp);
    }
    fputs("</ul>\n</section>\n", fp);
    _strlist_free(refs);
}

static int wiki_body_has_related_attachments_section(const char *html_body)
{
    if (!html_body) return 0;
    return strstr(html_body, "id=\"related-attachments\"") != NULL ||
           strstr(html_body, "id='related-attachments'") != NULL;
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

/* 将 wiki_md_meta.authors_json（JSON 数组）格式化为可读「、」分隔 */
static void wiki_fprint_authors_json_readable(FILE *fp, const char *aj)
{
    if (!aj || !aj[0]) {
        fputs("Admin", fp);
        return;
    }
    const char *p = aj;
    while (*p && *p != '[') p++;
    if (*p != '[') {
        fputs("Admin", fp);
        return;
    }
    p++;
    int first = 1;
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ']' || !*p) break;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"') break;
        p++;
        char buf[256];
        size_t j = 0;
        while (*p && *p != '"' && j < sizeof(buf) - 1) {
            if (*p == '\\' && p[1]) {
                p++;
                buf[j++] = *p++;
            } else
                buf[j++] = *p++;
        }
        buf[j] = '\0';
        if (*p == '"') p++;
        if (!first) fputs("\xe3\x80\x81", fp); /* 顿号 */
        first = 0;
        wiki_fhtml(fp, buf);
    }
    if (first) fputs("Admin", fp);
}

/* 已发布 HTML：文首元信息（更新 / 最后编辑 / 贡献者）。
 * 与 wiki-read 一致：优先 MD 首行 META 中的 authors / last_author，SQLite 仅补缺或替换占位 Admin。 */
static void wiki_fputs_am_block(FILE *fp, const char *article_id, const char *updated_fallback)
{
    wiki_md_meta_row_t row;
    memset(&row, 0, sizeof(row));
    const char *upd = updated_fallback ? updated_fallback : "";
    char m_la[128] = {0};
    char m_aj[2048] = {0};
    int has_meta = 0;

    char md_path[768];
    if (article_id && article_id[0] && wiki_md_find(md_path, sizeof(md_path), article_id) == 0) {
        FILE *mfp = fopen(md_path, "rb");
        if (mfp) {
            char line[4096];
            if (fgets(line, sizeof(line), mfp) && strncmp(line, "<!--META ", 9) == 0) {
                char *end = strstr(line, "-->");
                if (end) {
                    *end = '\0';
                    const char *mj = line + 9;
                    json_get_str(mj, "last_author", m_la, sizeof(m_la));
                    if (!m_la[0]) json_get_str(mj, "lastAuthor", m_la, sizeof(m_la));
                    json_get_raw(mj, "authors", m_aj, sizeof(m_aj));
                    has_meta = 1;
                }
            }
            fclose(mfp);
        }
    }

    char la_buf[128];
    char aj_buf[2048];
    const char *la;
    const char *aj;

    if (!has_meta) {
        la = "Admin";
        aj = "[\"Admin\"]";
        if (auth_wiki_md_meta_get(article_id, &row) == 0 && row.found) {
            if (row.updated[0]) upd = row.updated;
            if (row.last_author[0]) la = row.last_author;
            if (row.authors_json[0]) aj = row.authors_json;
        }
    } else {
        (void)auth_wiki_md_meta_get(article_id, &row);
        if (row.found && row.updated[0]) upd = row.updated;
        if (row.found && row.last_author[0] &&
            (!m_la[0] || strcmp(m_la, "Admin") == 0))
            snprintf(m_la, sizeof(m_la), "%s", row.last_author);
        if (row.found && row.authors_json[0] &&
            (!m_aj[0] || strcmp(m_aj, "[\"Admin\"]") == 0 ||
             strcmp(m_aj, "[]") == 0))
            snprintf(m_aj, sizeof(m_aj), "%s", row.authors_json);
        if (!m_la[0]) snprintf(m_la, sizeof(m_la), "Admin");
        if (!m_aj[0]) snprintf(m_aj, sizeof(m_aj), "[\"Admin\"]");
        snprintf(la_buf, sizeof(la_buf), "%s", m_la);
        snprintf(aj_buf, sizeof(aj_buf), "%s", m_aj);
        la = la_buf;
        aj = aj_buf;
    }
    fputs("<div class=\"am\">\u66f4\u65b0\uff1a", fp);
    wiki_fhtml(fp, upd);
    fputs(" \xc2\xb7 \xe6\x9c\x80\xe5\x90\x8e\xe7\xbc\x96\xe8\xbe\x91\xef\xbc\x9a", fp);
    wiki_fhtml(fp, la);
    fputs("<br>\u8d21\u732e\u8005\uff1a", fp);
    wiki_fprint_authors_json_readable(fp, aj);
    fputs("</div>\n<div class=\"ab\" id=\"article-body\">\n", fp);
}

/* ── wiki_write_html_file ─────────────────────────────────── */

static int wiki_write_html_file(const char *filepath,
    const char *id, const char *title, const char *category,
    const char *updated, const char *html_body)
{
    char tmp_path[1152];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", filepath);
    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) return -1;

    /* 相对路径前缀，使 HTML 文件可离线用 file:// 打开 */
    char rp[64];
    wiki_rel_prefix(rp, sizeof(rp), category);

    fputs("<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
          "<meta charset=\"UTF-8\">\n"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
          "<link rel=\"icon\" href=\"/favicon.svg\" type=\"image/svg+xml\">\n"
          "<title>", fp);
    wiki_fhtml(fp, title);
    fputs(" - NoteWiki</title>\n<style>\n"
          "*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}\n"
          "html,body{height:100%;overflow:hidden}\n"
          "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
               "background:#0d1117;color:#c9d1d9;line-height:1.7;"
               "display:flex;flex-direction:column}\n"
          "nav.topbar{background:#161b22;border-bottom:1px solid #30363d;"
                    "padding:8px 20px;font-size:.85rem;flex-shrink:0;"
                    "display:flex;align-items:center;gap:6px;flex-wrap:wrap}\n"
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
          ".wk-page-search{display:inline-flex;align-items:center;gap:4px;"
                   "margin-left:6px;flex:0 1 260px;min-width:180px}\n"
          ".wk-page-search input{width:100%;min-width:120px;background:#0d1117;"
                   "color:#c9d1d9;border:1px solid #30363d;border-radius:5px;"
                   "padding:4px 8px;font-size:.78rem;outline:none}\n"
          ".wk-page-search input:focus{border-color:#4a90e2;"
                   "box-shadow:0 0 0 2px rgba(74,144,226,.18)}\n"
          ".wk-page-search .copy-btn{margin-left:0;padding:4px 9px}\n"
          "@media(max-width:720px){nav.topbar{padding:8px 12px}"
                   ".edit-btn{margin-left:0}.wk-page-search{order:20;"
                   "flex-basis:100%;margin-left:0}.wk-page-search input{min-width:0}}\n"
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
          ".ab h1::before,.ab h2::before,.ab h3::before,.ab h4::before{content:attr(data-secnum);color:#8b949e;font-weight:400;font-size:.88em;margin-right:.1em}\n"
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
          ".ab .math-block{margin:1em 0;overflow-x:auto}\n"
          ".ab .mermaid-block{margin:1em 0;padding:10px 12px;border:1px solid #30363d;"
              "border-radius:6px;background:#101722;overflow:auto}\n"
          ".ab .mermaid{min-width:220px}\n"
          ".ab mjx-container{color:#c9d1d9 !important}\n"
          ".ab mjx-container[display='true']{margin:1em 0;overflow-x:auto}\n"
          ".ab .footnotes{margin-top:1.6em;padding-top:.2em}\n"
          ".ab .footnotes hr{margin:.7em 0 1em}\n"
          ".ab .footnotes ol{padding-left:1.5em;margin:.2em 0}\n"
          ".ab .footnotes li{color:#8b949e;font-size:.9em;line-height:1.7;margin:.35em 0}\n"
          ".ab .fn-ref a{border-bottom:none;font-size:.8em;vertical-align:super}\n"
          ".ab .fn-backref{margin-left:.35em;border-bottom:none}\n"
          ".ab .rel-attachments{margin-top:2em;padding-top:1em;border-top:1px dashed #30363d}\n"
          ".ab .rel-attachments h2{margin:0 0 .6em;color:#79c0ff}\n"
          ".ab .rel-attachments ul{padding-left:1.2em;margin:.2em 0}\n"
          ".ab .rel-attachments li{margin:.22em 0}\n"
          ".sec-num{color:#8b949e;font-weight:400;font-size:.9em;letter-spacing:.02em}\n"
          /* 打印/PDF：彻底隐藏顶栏、全站文章目录、本文 TOC，并展开正文（多浏览器兼容） */
          "@media print{\n"
          "html,body{height:auto!important;max-height:none!important;overflow:visible!important;"
          "background:#fff!important;color:#1a1a2e!important;"
          "-webkit-print-color-adjust:exact;print-color-adjust:exact}\n"
          "body{display:block!important}\n"
          "nav.topbar,nav#sidebar,nav#toc,.sidebar,.toc,\n"
          ".st-top,.sidebar-body,.toc-top,.toc-body,\n"
          ".edit-btn,.copy-btn,.wk-page-search,.panel-toggle,.panel-label{display:none!important;"
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
          ".modal-overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.55);z-index:999;align-items:center;justify-content:center}\n"
          ".modal-overlay.open{display:flex}\n"
          ".modal-box{background:#161b22;border:1px solid #30363d;border-radius:10px;width:340px;max-width:92vw;box-shadow:0 8px 32px rgba(0,0,0,.5)}\n"
          ".modal-box.modal-wide{width:min(1100px,96vw)}\n"
          ".modal-head{padding:13px 16px 9px;border-bottom:1px solid #30363d;font-weight:600;font-size:.88rem;color:#f0f6fc;display:flex;justify-content:space-between;align-items:center}\n"
          ".modal-body{padding:14px 16px}\n"
          ".modal-foot{padding:10px 16px 13px;border-top:1px solid #30363d;display:flex;gap:8px;justify-content:flex-end}\n"
          ".modal-close{background:none;border:none;color:#8b949e;cursor:pointer;font-size:1.1rem;line-height:1}\n"
          ".btn{padding:5px 12px;border:none;border-radius:5px;font-size:.78rem;cursor:pointer;display:inline-flex;align-items:center;gap:4px;transition:opacity .15s;white-space:nowrap}\n"
          ".btn:hover{opacity:.85}\n"
          ".btn-secondary{background:#27334d;color:#c9d1d9;border:1px solid #30363d}\n"
          ".btn-sm{padding:4px 9px;font-size:.74rem}\n"
          ".history-summary{font-size:.76rem;color:#8b949e;margin-bottom:10px}\n"
          ".history-table-wrap{max-height:min(62vh,560px);overflow:auto;border:1px solid #30363d;border-radius:8px}\n"
          ".history-table{width:100%;border-collapse:collapse;font-size:.76rem}\n"
          ".history-table th,.history-table td{border-bottom:1px solid #30363d;padding:7px 9px;text-align:left;white-space:nowrap}\n"
          ".history-table th{position:sticky;top:0;background:#1a2236;color:#f0f6fc;z-index:1}\n"
          ".history-table td:nth-child(1),.history-table td:nth-child(5){font-family:\"SFMono-Regular\",Consolas,monospace}\n"
          ".history-empty{padding:22px 12px;text-align:center;color:#8b949e;font-size:.78rem}\n"
          ".hist-detail-grid{display:grid;grid-template-columns:140px 1fr;gap:8px 10px}\n"
          ".hist-detail-k{font-size:.75rem;color:#8b949e;text-align:right;padding-top:2px}\n"
          ".hist-detail-v{font-size:.8rem;color:#c9d1d9;word-break:break-all;background:#111827;border:1px solid #30363d;border-radius:6px;padding:6px 8px}\n"
          ".hist-mode-toggle{display:flex;gap:6px;margin-bottom:8px;flex-wrap:wrap}\n"
          ".hist-mode-btn{padding:3px 8px;border:1px solid #30363d;background:#1a2236;color:#8b949e;border-radius:6px;font-size:.72rem;cursor:pointer}\n"
          ".hist-mode-btn.on{background:#4a90e2;border-color:#4a90e2;color:#fff}\n"
          ".hist-diff-line{font-family:\"Cascadia Mono\",\"SFMono-Regular\",Consolas,monospace;line-height:1.45;padding:2px 6px;border-radius:4px;margin:1px 0}\n"
          ".hist-diff-del{background:rgba(248,81,73,.18);color:#ffd7d5}\n"
          ".hist-diff-add{background:rgba(63,185,80,.2);color:#b6f3c1}\n"
          ".hist-diff-note{background:rgba(139,148,158,.16);color:#c9d1d9}\n"
          ".hist-diff-ctx{background:rgba(110,118,129,.12);color:#c9d1d9}\n"
          "</style>\n</head>\n<body>\n", fp);

    fputs("<nav class=\"topbar\"><a href=\"/wiki/notewikiindex.html\">\u6587\u7ae0\u7d22\u5f15</a> \u00b7 <a href=\"/wiki/notewiki.html\">\u2190 NoteWiki</a>", fp);
    if (category && category[0]) { fputs(" / ", fp); wiki_fhtml(fp, category); }
    fputs(" <a class=\"edit-btn\" href=\"/wiki/notewiki.html?edit=", fp);
    fputs(id, fp);
    fputs("\">\u270f \u7f16\u8f91</a>"
          "<button class=\"copy-btn\" id=\"view-history-btn\" onclick=\"openHistoryModal()\">\u5386\u53f2\u8bb0\u5f55</button>"
          "<button class=\"copy-btn\" id=\"export-md-btn\" onclick=\"exportMdZip()\">\u5bfc\u51fa MD ZIP</button>"
          "<button class=\"copy-btn\" id=\"export-pdf-btn\" onclick=\"exportPdf()\">\u5bfc\u51fa PDF</button>"
          "<span class=\"wk-page-search\" id=\"wk-page-search\">"
          "<input id=\"wk-page-search-input\" type=\"search\" placeholder=\"\u641c\u7d22 Wiki\" autocomplete=\"off\" spellcheck=\"false\">"
          "<button type=\"button\" class=\"copy-btn\" id=\"wk-page-search-btn\">\u641c\u7d22</button>"
          "</span>"
          "</nav>\n", fp);

    fputs(
        "<div class=\"modal-overlay\" id=\"modal-history\">"
          "<div class=\"modal-box modal-wide\">"
            "<div class=\"modal-head\">历史修改记录"
              "<button class=\"modal-close\" onclick=\"closeHistoryModal()\">✕</button></div>"
            "<div class=\"modal-body\">"
              "<div class=\"history-summary\" id=\"history-summary\">加载中…</div>"
              "<div class=\"history-table-wrap\" id=\"history-wrap\">"
                "<div class=\"history-empty\">加载中…</div>"
              "</div>"
            "</div>"
            "<div class=\"modal-foot\">"
              "<button class=\"btn btn-secondary\" onclick=\"closeHistoryModal()\">关闭</button>"
            "</div>"
          "</div>"
        "</div>\n"
        "<div class=\"modal-overlay\" id=\"modal-history-detail\">"
          "<div class=\"modal-box modal-wide\">"
            "<div class=\"modal-head\">历史详情"
              "<button class=\"modal-close\" onclick=\"_wkCloseHistDetail()\">✕</button></div>"
            "<div class=\"modal-body\">"
              "<div class=\"hist-detail-grid\">"
                "<div class=\"hist-detail-k\">标题</div>"
                "<div class=\"hist-detail-v\" id=\"histd-title\"></div>"
                "<div class=\"hist-detail-k\">分类</div>"
                "<div class=\"hist-detail-v\" id=\"histd-category\"></div>"
                "<div class=\"hist-detail-k\">save_txn_id</div>"
                "<div class=\"hist-detail-v\" id=\"histd-txn\"></div>"
                "<div class=\"hist-detail-k\">内容长度</div>"
                "<div class=\"hist-detail-v\" id=\"histd-len\"></div>"
                "<div class=\"hist-detail-k\">修改内容</div>"
                "<div class=\"hist-detail-v\" style=\"white-space:pre-wrap;max-height:62vh;overflow:auto\">"
                  "<div class=\"hist-mode-toggle\">"
                    "<button type=\"button\" class=\"hist-mode-btn on\" id=\"histd-mode-changes\" onclick=\"_wkSetDiffMode(false)\">仅显示变更行</button>"
                    "<button type=\"button\" class=\"hist-mode-btn\" id=\"histd-mode-context\" onclick=\"_wkSetDiffMode(true)\">显示上下文行</button>"
                  "</div>"
                  "<div id=\"histd-diff\"></div>"
                "</div>"
              "</div>"
            "</div>"
            "<div class=\"modal-foot\">"
              "<button class=\"btn btn-secondary\" onclick=\"_wkCloseHistDetail()\">关闭</button>"
            "</div>"
          "</div>"
        "</div>\n", fp);

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
    fputs("</h1>\n", fp);
    wiki_fputs_am_block(fp, id, updated);
    if (html_body) wiki_fwrite_body(fp, html_body, rp);
    if (html_body) wiki_write_related_attachments(fp, html_body, rp);
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
          "function _wkEsc(s){return String(s==null?'':s).replace(/[&<>\"]/g,function(c){return c==='&'?'&amp;':c==='<'?'&lt;':c==='>'?'&gt;':'&quot;';});}\n"
          "var _wkHistItems=[],_wkHistDiffCtx=false,_wkHistDiffCur='',_wkHistDiffPrev='';\n"
          "function _wkFilterSameVersion(items){\n"
          "  if(!items||!items.length) return [];\n"
          "  var out=[],last=null;\n"
          "  for(var i=0;i<items.length;i++){\n"
          "    var it=items[i],c=(it&&typeof it.content==='string')?it.content:null;\n"
          "    if(c===null){out.push(it);continue;}\n"
          "    if(!out.length||c!==last){out.push(it);last=c;}\n"
          "  }\n"
          "  return out;\n"
          "}\n"
          "function _wkRenderHistRows(items){\n"
          "  if(!items||!items.length) return '<div class=\"history-empty\">\\u6682\\u65e0\\u5386\\u53f2\\u8bb0\\u5f55</div>';\n"
          "  var rows=items.map(function(it){\n"
          "    return '<tr>'\n"
          "      +'<td>'+_wkEsc(String(it.id||''))+'</td>'\n"
          "      +'<td>'+_wkEsc(String(it.createdAt||''))+'</td>'\n"
          "      +'<td>'+_wkEsc(String(it.editor||''))+'</td>'\n"
          "      +'<td>'+_wkEsc(String(it.ip||''))+'</td>'\n"
          "      +'<td>'+_wkEsc(String(it.saveTxnId||''))+'</td>'\n"
          "      +'<td>'+_wkEsc(String(it.contentLength||0))+'</td>'\n"
          "      +'<td><button class=\"btn btn-secondary btn-sm\" onclick=\"_wkOpenHistDetail('+Number(it.id||0)+')\">\\u8be6\\u60c5</button></td>'\n"
          "      +'</tr>';\n"
          "  }).join('');\n"
          "  return '<table class=\"history-table\"><thead><tr>'\n"
          "    +'<th>#</th><th>\\u4fdd\\u5b58\\u65f6\\u95f4</th><th>\\u7f16\\u8f91\\u8005</th><th>IP</th><th>save_txn_id</th><th>\\u5185\\u5bb9\\u957f\\u5ea6</th><th>\\u64cd\\u4f5c</th>'\n"
          "    +'</tr></thead><tbody>'+rows+'</tbody></table>';\n"
          "}\n"
          "function closeHistoryModal(){\n"
          "  var m=document.getElementById('modal-history');\n"
          "  if(m) m.classList.remove('open');\n"
          "}\n"
          "function openHistoryModal(){\n"
          "  if(window.location.protocol==='file:'){showToast('\\u79bb\\u7ebf\\u6a21\\u5f0f\\u4e0d\\u652f\\u6301\\u5386\\u53f2\\u67e5\\u8be2');return;}\n"
          "  var id=window.WIKI_CUR_ID||'';\n"
          "  if(!id){showToast('\\u7f3a\\u5c11\\u6587\\u7ae0 ID');return;}\n"
          "  var modal=document.getElementById('modal-history');\n"
          "  var summary=document.getElementById('history-summary');\n"
          "  var wrap=document.getElementById('history-wrap');\n"
          "  if(!modal||!summary||!wrap) return;\n"
          "  summary.textContent='\\u6587\\u7ae0 ID\\uff1a'+id+'\\uff08\\u52a0\\u8f7d\\u4e2d\\u2026\\uff09';\n"
          "  wrap.innerHTML='<div class=\"history-empty\">\\u52a0\\u8f7d\\u4e2d\\u2026</div>';\n"
          "  _wkHistItems=[];\n"
          "  modal.classList.add('open');\n"
          "  fetch('/api/wiki-md-history?id='+encodeURIComponent(id)+'&limit=100&with_content=1')\n"
          "    .then(function(r){return r.json().then(function(d){return{ok:r.ok,data:d,status:r.status};});})\n"
          "    .then(function(resp){\n"
          "      var d=resp.data||{};\n"
          "      if(!resp.ok||!d.ok){\n"
          "        var msg=(d&&d.error)?d.error:('HTTP '+resp.status);\n"
          "        summary.textContent='\\u6587\\u7ae0 ID\\uff1a'+id;\n"
          "        wrap.innerHTML='<div class=\"history-empty\">\\u8bfb\\u53d6\\u5931\\u8d25\\uff1a'+_wkEsc(msg)+'</div>';\n"
          "        return;\n"
          "      }\n"
          "      var items=d.items||[];\n"
          "      var filtered=_wkFilterSameVersion(items);\n"
          "      _wkHistItems=filtered;\n"
          "      var removed=items.length-filtered.length;\n"
          "      summary.textContent='\\u6587\\u7ae0 ID\\uff1a'+id+'\\uff0c\\u5171 '+filtered.length+' \\u6761\\uff08\\u6309\\u65f6\\u95f4\\u5012\\u5e8f\\uff09'+(removed>0?'\\uff0c\\u5df2\\u8fc7\\u6ee4 '+removed+' \\u6761\\u76f8\\u540c\\u7248\\u672c':'');\n"
          "      wrap.innerHTML=_wkRenderHistRows(filtered);\n"
          "    })\n"
          "    .catch(function(e){\n"
          "      summary.textContent='\\u6587\\u7ae0 ID\\uff1a'+id;\n"
          "      wrap.innerHTML='<div class=\"history-empty\">\\u8bf7\\u6c42\\u5931\\u8d25\\uff1a'+_wkEsc((e&&e.message)||String(e))+'</div>';\n"
          "    });\n"
          "}\n"
          "function _wkCloseHistDetail(){\n"
          "  var m=document.getElementById('modal-history-detail');\n"
          "  if(m) m.classList.remove('open');\n"
          "}\n"
          "function _wkBuildDiff(cur,prev,ctx){\n"
          "  var ca=String(cur||'').replace(/\\r\\n?/g,'\\n').split('\\n');\n"
          "  var pa=String(prev||'').replace(/\\r\\n?/g,'\\n').split('\\n');\n"
          "  var out=[];\n"
          "  if(!prev&&cur) out.push('<div class=\"hist-diff-line hist-diff-note\">\\u9996\\u6b21\\u5b58\\u6863\\uff0c\\u65e0\\u4e0a\\u4e00\\u7248\\u672c\\u53ef\\u5bf9\\u6bd4\\u3002</div>');\n"
          "  var max=Math.max(ca.length,pa.length),changed=[];\n"
          "  for(var k=0;k<max;k++){if((k<pa.length?pa[k]:null)!==(k<ca.length?ca[k]:null))changed.push(k);}\n"
          "  function near(idx){if(!ctx)return false;for(var j=0;j<changed.length;j++){if(Math.abs(changed[j]-idx)<=2)return true;}return false;}\n"
          "  var skipped=false;\n"
          "  for(var i=0;i<max;i++){\n"
          "    var a=i<pa.length?pa[i]:null,b=i<ca.length?ca[i]:null;\n"
          "    if(a===b){\n"
          "      if(near(i)){out.push('<div class=\"hist-diff-line hist-diff-ctx\">  L'+(i+1)+': '+_wkEsc(a||'')+'</div>');skipped=false;}\n"
          "      else if(ctx&&!skipped&&changed.length>0){out.push('<div class=\"hist-diff-line hist-diff-note\">... \\u7701\\u7565\\u672a\\u6539\\u52a8\\u884c ...</div>');skipped=true;}\n"
          "      continue;\n"
          "    }\n"
          "    if(a!==null)out.push('<div class=\"hist-diff-line hist-diff-del\">- L'+(i+1)+': '+_wkEsc(a)+'</div>');\n"
          "    if(b!==null)out.push('<div class=\"hist-diff-line hist-diff-add\">+ L'+(i+1)+': '+_wkEsc(b)+'</div>');\n"
          "    skipped=false;\n"
          "    if(out.length>1200){out.push('<div class=\"hist-diff-line hist-diff-note\">... \\u53d8\\u66f4\\u5185\\u5bb9\\u8fc7\\u957f\\uff0c\\u5df2\\u622a\\u65ad\\u663e\\u793a ...</div>');break;}\n"
          "  }\n"
          "  if(!out.length)return '<div class=\"hist-diff-line hist-diff-note\">\\u8be5\\u7248\\u672c\\u4e0e\\u4e0a\\u4e00\\u7248\\u672c\\u5185\\u5bb9\\u4e00\\u81f4\\u3002</div>';\n"
          "  return out.join('');\n"
          "}\n"
          "function _wkRenderDiff(){\n"
          "  var box=document.getElementById('histd-diff');\n"
          "  if(!box)return;\n"
          "  box.innerHTML=_wkBuildDiff(_wkHistDiffCur,_wkHistDiffPrev,_wkHistDiffCtx);\n"
          "  var b1=document.getElementById('histd-mode-changes'),b2=document.getElementById('histd-mode-context');\n"
          "  if(b1)b1.classList.toggle('on',!_wkHistDiffCtx);\n"
          "  if(b2)b2.classList.toggle('on',!!_wkHistDiffCtx);\n"
          "}\n"
          "function _wkSetDiffMode(ctx){_wkHistDiffCtx=!!ctx;_wkRenderDiff();}\n"
          "function _wkOpenHistDetail(historyId){\n"
          "  var hit=null,hitIdx=-1;\n"
          "  for(var i=0;i<_wkHistItems.length;i++){\n"
          "    if(String(_wkHistItems[i].id||'')===String(historyId||'')){hit=_wkHistItems[i];hitIdx=i;break;}\n"
          "  }\n"
          "  if(!hit){showToast('\\u672a\\u627e\\u5230\\u8be5\\u5386\\u53f2\\u8bb0\\u5f55\\u8be6\\u60c5');return;}\n"
          "  document.getElementById('histd-title').textContent=String(hit.title||'');\n"
          "  document.getElementById('histd-category').textContent=String(hit.category||'');\n"
          "  document.getElementById('histd-txn').textContent=String(hit.saveTxnId||'');\n"
          "  document.getElementById('histd-len').textContent=String(hit.contentLength||0);\n"
          "  var prev=hitIdx+1<_wkHistItems.length?_wkHistItems[hitIdx+1]:null;\n"
          "  _wkHistDiffCur=hit.content||'';\n"
          "  _wkHistDiffPrev=prev?prev.content||'':'';\n"
          "  _wkHistDiffCtx=false;\n"
          "  _wkRenderDiff();\n"
          "  document.getElementById('modal-history-detail').classList.add('open');\n"
          "}\n"
          "document.addEventListener('click',function(e){\n"
          "  if(e.target===document.getElementById('modal-history')) closeHistoryModal();\n"
          "  if(e.target===document.getElementById('modal-history-detail')) _wkCloseHistDetail();\n"
          "});\n"
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
    fputs("rich-render.js\"></script>\n"
          "<script src=\"", fp);
    fputs(rp, fp);
    fputs("sidebar.js\"></script>\n</body>\n</html>\n", fp);

    if (wiki_fsync_file(fp) != 0) {
        fclose(fp);
        unlink(tmp_path);
        return -1;
    }
    fclose(fp);
    if (wiki_rename_replace(tmp_path, filepath) != 0) {
        unlink(tmp_path);
        return -1;
    }
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
        if (top && (strcmp(de->d_name,"md_db")==0 ||
                    strcmp(de->d_name,"uploads")==0 ||
                    strcmp(de->d_name,"vendor")==0 ||
                    strcmp(de->d_name,"sqlite_db")==0 ||
                    strcmp(de->d_name,"trash")==0)) continue;
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

#define WIKI_SEARCH_MAX_RESULTS   200
#define WIKI_SEARCH_MAX_FILE_BYTES (512 * 1024)  /* 单文件最多扫描 512KB */
#define WIKI_INDEX_MAX_FILE_BYTES  (512 * 1024)  /* 单文件最多写入索引 512KB */

/* 跳过非内容目录：构建产物、归档、版本控制等 */
static int wiki_search_skip_dir(const char *name)
{
    if (!name || !name[0]) return 1;
    if (name[0] == '.') return 1;
    if (strcmp(name, "archive") == 0 || strcmp(name, "build") == 0 ||
        strcmp(name, "trash") == 0 || strcmp(name, "node_modules") == 0) return 1;
    return 0;
}

static void wiki_search_dir(strbuf_t *sb, int *pfirst, const char *dir, const char *q,
                            int *pcount, int max_results, int max_file_bytes)
{
    if (*pcount >= max_results) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && *pcount < max_results) {
        if (de->d_name[0] == '.') continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!wiki_search_skip_dir(de->d_name))
                wiki_search_dir(sb, pfirst, child, q, pcount, max_results, max_file_bytes);
            continue;
        }
        size_t nl = strlen(de->d_name);
        if (nl < 4 || strcmp(de->d_name + nl - 3, ".md") != 0) continue;
        FILE *fp = fopen(child, "rb");
        if (!fp) continue;
        char ml[4096] = {0};
        int ok = (fgets(ml, sizeof(ml), fp) != NULL);
        strbuf_t body = {0};
        char buf[8192];
        size_t nr;
        while (ok && (nr = fread(buf, 1, sizeof(buf), fp)) > 0) {
            if (body.len + nr > (size_t)max_file_bytes) {
                size_t rem = (size_t)max_file_bytes - body.len;
                if (rem > 0) sb_append(&body, buf, rem);
                break;
            }
            sb_append(&body, buf, nr);
        }
        fclose(fp);
        if (!ok) {
            free(body.data);
            continue;
        }

        char path_cat[512] = {0}, path_id[128] = {0};
        wiki_cat_id_from_md_abspath(child, path_cat, sizeof(path_cat), path_id, sizeof(path_id));

        char id[128] = {0}, title[512] = {0}, cat[512] = {0}, cre[64] = {0}, upd[64] = {0};
        char last_author_out[128] = {0};
        char authors_json_out[2048] = {0};
        char now_iso[64];
        wiki_now_iso(now_iso, sizeof(now_iso));

        if (strncmp(ml, "<!--META ", 9) == 0) {
            char *end = strstr(ml, "-->");
            if (!end) {
                free(body.data);
                continue;
            }
            *end = '\0';
            const char *mj = ml + 9;
            json_get_str(mj, "id", id, sizeof(id));
            json_get_str(mj, "title", title, sizeof(title));
            json_get_str(mj, "category", cat, sizeof(cat));
            json_get_str(mj, "created", cre, sizeof(cre));
            json_get_str(mj, "updated", upd, sizeof(upd));
            if (!id[0]) {
                free(body.data);
                continue;
            }
            if (!cre[0]) strncpy(cre, now_iso, sizeof(cre) - 1);
            if (!upd[0]) strncpy(upd, now_iso, sizeof(upd) - 1);
            json_get_str(mj, "last_author", last_author_out, sizeof(last_author_out));
            if (!last_author_out[0]) json_get_str(mj, "lastAuthor", last_author_out, sizeof(last_author_out));
            json_get_raw(mj, "authors", authors_json_out, sizeof(authors_json_out));
            (void)auth_wiki_md_meta_upsert_scan_meta(id, title, cat, cre, upd);
        } else {
            snprintf(id, sizeof(id), "%s", path_id);
            snprintf(cat, sizeof(cat), "%s", path_cat);
            strbuf_t fulltxt = {0};
            sb_append(&fulltxt, ml, strlen(ml));
            if (body.data && body.len)
                sb_append(&fulltxt, body.data, body.len);
            wiki_extract_md_title(fulltxt.data ? fulltxt.data : "", title, sizeof(title));
            free(fulltxt.data);
            if (!title[0]) snprintf(title, sizeof(title), "%s", id);
            strncpy(cre, now_iso, sizeof(cre) - 1);
            strncpy(upd, now_iso, sizeof(upd) - 1);
            (void)auth_wiki_md_meta_ensure_scan_plain(id, cat, title, now_iso);
        }

        wiki_md_meta_row_t row;
        memset(&row, 0, sizeof(row));
        if (auth_wiki_md_meta_get(id, &row) == 0 && row.found) {
            if (strncmp(ml, "<!--META ", 9) != 0) {
                if (row.created[0]) strncpy(cre, row.created, sizeof(cre) - 1);
                if (row.updated[0]) strncpy(upd, row.updated, sizeof(upd) - 1);
                if (row.title[0]) strncpy(title, row.title, sizeof(title) - 1);
                snprintf(last_author_out, sizeof(last_author_out), "%s",
                         row.last_author[0] ? row.last_author : "Admin");
                snprintf(authors_json_out, sizeof(authors_json_out), "%s",
                         row.authors_json[0] ? row.authors_json : "[\"Admin\"]");
            } else {
                if (row.last_author[0] &&
                    (!last_author_out[0] || strcmp(last_author_out, "Admin") == 0)) {
                    snprintf(last_author_out, sizeof(last_author_out), "%s", row.last_author);
                }
                if (row.authors_json[0] &&
                    (!authors_json_out[0] || strcmp(authors_json_out, "[\"Admin\"]") == 0 ||
                     strcmp(authors_json_out, "[]") == 0)) {
                    snprintf(authors_json_out, sizeof(authors_json_out), "%s", row.authors_json);
                }
            }
        }

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
        if (!*pfirst) SB_LIT(sb, ",");
        *pfirst = 0;
        SB_LIT(sb, "{\"id\":");
        sb_json_str(sb, id);
        SB_LIT(sb, ",\"title\":");
        sb_json_str(sb, title);
        SB_LIT(sb, ",\"category\":");
        sb_json_str(sb, cat);
        SB_LIT(sb, ",\"created\":");
        sb_json_str(sb, cre);
        SB_LIT(sb, ",\"updated\":");
        sb_json_str(sb, upd);
        SB_LIT(sb, ",\"lastAuthor\":");
        sb_json_str(sb, last_author_out);
        SB_LIT(sb, ",\"authors\":");
        sb_append(sb, authors_json_out, strlen(authors_json_out));
        SB_LIT(sb, ",\"snippet\":");
        sb_json_str(sb, snippet.data ? snippet.data : "");
        SB_LIT(sb, "}");
        free(snippet.data);
    }
    closedir(d);
}

static void wiki_index_collect_dirs(strbuf_t *sb, const char *base,
                                    const char *rel, int *pfirst)
{
    char full[1024];
    if (rel && rel[0]) snprintf(full, sizeof(full), "%s/%s", base, rel);
    else               snprintf(full, sizeof(full), "%s", base);
    DIR *d = opendir(full);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (wiki_search_skip_dir(de->d_name)) continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", full, de->d_name);
        struct stat st;
        if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        char relpath[512];
        if (rel && rel[0]) snprintf(relpath, sizeof(relpath), "%s/%s", rel, de->d_name);
        else               snprintf(relpath, sizeof(relpath), "%s", de->d_name);
        if (!*pfirst) SB_LIT(sb, ",");
        *pfirst = 0;
        sb_json_str(sb, relpath);
        wiki_index_collect_dirs(sb, base, relpath, pfirst);
    }
    closedir(d);
}

static void wiki_index_dir(strbuf_t *sb, int *pfirst, const char *dir, int *pcount)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!wiki_search_skip_dir(de->d_name))
                wiki_index_dir(sb, pfirst, child, pcount);
            continue;
        }

        size_t nl = strlen(de->d_name);
        if (nl < 4 || strcmp(de->d_name + nl - 3, ".md") != 0) continue;
        FILE *fp = fopen(child, "rb");
        if (!fp) continue;

        char ml[4096] = {0};
        int ok = (fgets(ml, sizeof(ml), fp) != NULL);
        strbuf_t body = {0};
        int truncated = 0;
        char buf[8192];
        size_t nr;
        while (ok && (nr = fread(buf, 1, sizeof(buf), fp)) > 0) {
            if (body.len + nr > (size_t)WIKI_INDEX_MAX_FILE_BYTES) {
                size_t rem = (size_t)WIKI_INDEX_MAX_FILE_BYTES - body.len;
                if (rem > 0) sb_append(&body, buf, rem);
                truncated = 1;
                break;
            }
            sb_append(&body, buf, nr);
        }
        fclose(fp);
        if (!ok) {
            free(body.data);
            continue;
        }

        char path_cat[512] = {0}, path_id[128] = {0};
        wiki_cat_id_from_md_abspath(child, path_cat, sizeof(path_cat), path_id, sizeof(path_id));

        char id[128] = {0}, title[512] = {0}, cat[512] = {0}, cre[64] = {0}, upd[64] = {0};
        char now_iso[64];
        wiki_now_iso(now_iso, sizeof(now_iso));

        if (strncmp(ml, "<!--META ", 9) == 0) {
            char *end = strstr(ml, "-->");
            if (!end) {
                free(body.data);
                continue;
            }
            *end = '\0';
            const char *mj = ml + 9;
            json_get_str(mj, "id", id, sizeof(id));
            json_get_str(mj, "title", title, sizeof(title));
            json_get_str(mj, "category", cat, sizeof(cat));
            json_get_str(mj, "created", cre, sizeof(cre));
            json_get_str(mj, "updated", upd, sizeof(upd));
            if (!id[0]) {
                free(body.data);
                continue;
            }
            if (!cat[0]) snprintf(cat, sizeof(cat), "%s", path_cat);
            if (!cre[0]) strncpy(cre, now_iso, sizeof(cre) - 1);
            if (!upd[0]) strncpy(upd, now_iso, sizeof(upd) - 1);
            if (!title[0]) snprintf(title, sizeof(title), "%s", id);
        } else {
            snprintf(id, sizeof(id), "%s", path_id);
            snprintf(cat, sizeof(cat), "%s", path_cat);
            strbuf_t fulltxt = {0};
            sb_append(&fulltxt, ml, strlen(ml));
            if (body.data && body.len)
                sb_append(&fulltxt, body.data, body.len);
            wiki_extract_md_title(fulltxt.data ? fulltxt.data : "", title, sizeof(title));
            if (fulltxt.data) {
                free(body.data);
                body = fulltxt;
            } else {
                free(fulltxt.data);
            }
            if (!id[0]) {
                free(body.data);
                continue;
            }
            if (!title[0]) snprintf(title, sizeof(title), "%s", id);
            strncpy(cre, now_iso, sizeof(cre) - 1);
            strncpy(upd, now_iso, sizeof(upd) - 1);

            wiki_md_meta_row_t row;
            memset(&row, 0, sizeof(row));
            if (auth_wiki_md_meta_get(id, &row) == 0 && row.found) {
                if (row.created[0]) strncpy(cre, row.created, sizeof(cre) - 1);
                if (row.updated[0]) strncpy(upd, row.updated, sizeof(upd) - 1);
                if (row.title[0]) strncpy(title, row.title, sizeof(title) - 1);
            }
        }

        if (!*pfirst) SB_LIT(sb, ",");
        *pfirst = 0;
        SB_LIT(sb, "{\"id\":");
        sb_json_str(sb, id);
        SB_LIT(sb, ",\"title\":");
        sb_json_str(sb, title);
        SB_LIT(sb, ",\"category\":");
        sb_json_str(sb, cat);
        SB_LIT(sb, ",\"created\":");
        sb_json_str(sb, cre);
        SB_LIT(sb, ",\"updated\":");
        sb_json_str(sb, upd);
        SB_LIT(sb, ",\"content\":");
        sb_json_str(sb, body.data ? body.data : "");
        SB_LIT(sb, ",\"truncated\":");
        if (truncated) SB_LIT(sb, "true");
        else           SB_LIT(sb, "false");
        SB_LIT(sb, "}");
        free(body.data);
        (*pcount)++;
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

void handle_api_wiki_list(http_sock_t client_fd)
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

void handle_api_wiki_read(http_sock_t client_fd, const char *path_qs)
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
    char line[4096];
    if (!fgets(line, sizeof(line), fp)) { fclose(fp);
        send_json(client_fd,404,"Not Found","{\"ok\":false,\"error\":\"not found\"}",32); return; }
    strbuf_t body = {0};
    if (strncmp(line, "<!--META ", 9) != 0)
        sb_append(&body, line, strlen(line));
    char buf[8192];
    size_t nr;
    while ((nr = fread(buf,1,sizeof(buf),fp)) > 0) sb_append(&body,buf,nr);
    fclose(fp);
    /* extract meta fields from META line */
    char m_id[128] = {0}, m_title[512] = {0}, m_cat[512] = {0};
    char m_cre[64] = {0}, m_upd[64] = {0};
    char m_la[128] = {0}, m_aj[2048] = {0};
    if (line[0] && strncmp(line, "<!--META ", 9) == 0) {
        char *end = strstr(line, "-->");
        if (end) {
            *end = '\0';
            json_get_str(line + 9, "id", m_id, sizeof(m_id));
            json_get_str(line + 9, "title", m_title, sizeof(m_title));
            json_get_str(line + 9, "category", m_cat, sizeof(m_cat));
            json_get_str(line + 9, "created", m_cre, sizeof(m_cre));
            json_get_str(line + 9, "updated", m_upd, sizeof(m_upd));
            json_get_str(line + 9, "last_author", m_la, sizeof(m_la));
            if (!m_la[0]) json_get_str(line + 9, "lastAuthor", m_la, sizeof(m_la));
            json_get_raw(line + 9, "authors", m_aj, sizeof(m_aj));
        }
    }
    if (!m_id[0]) snprintf(m_id, sizeof(m_id), "%s", id);
    if (!m_title[0]) snprintf(m_title, sizeof(m_title), "%s", id);

    wiki_md_meta_row_t wrow;
    memset(&wrow, 0, sizeof(wrow));
    if (auth_wiki_md_meta_get(m_id, &wrow) == 0 && wrow.found) {
        if (strncmp(line, "<!--META ", 9) != 0) {
            if (wrow.last_author[0]) snprintf(m_la, sizeof(m_la), "%s", wrow.last_author);
            if (wrow.authors_json[0]) snprintf(m_aj, sizeof(m_aj), "%s", wrow.authors_json);
        } else {
            if (wrow.last_author[0] && (!m_la[0] || strcmp(m_la, "Admin") == 0))
                snprintf(m_la, sizeof(m_la), "%s", wrow.last_author);
            if (wrow.authors_json[0] &&
                (!m_aj[0] || strcmp(m_aj, "[\"Admin\"]") == 0 || strcmp(m_aj, "[]") == 0)) {
                snprintf(m_aj, sizeof(m_aj), "%s", wrow.authors_json);
            }
        }
    }
    if (!m_la[0]) snprintf(m_la, sizeof(m_la), "Admin");
    if (!m_aj[0]) snprintf(m_aj, sizeof(m_aj), "[\"Admin\"]");

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"content\":");
    sb_json_str(&sb, body.data ? body.data : "");
    SB_LIT(&sb, ",\"meta\":{");
    SB_LIT(&sb, "\"id\":\""); sb_append(&sb, m_id, strlen(m_id));
    SB_LIT(&sb, "\",\"title\":\""); sb_append(&sb, m_title, strlen(m_title));
    SB_LIT(&sb, "\",\"category\":\""); sb_append(&sb, m_cat, strlen(m_cat));
    SB_LIT(&sb, "\",\"created\":\""); sb_append(&sb, m_cre, strlen(m_cre));
    SB_LIT(&sb, "\",\"updated\":\""); sb_append(&sb, m_upd, strlen(m_upd));
    SB_LIT(&sb, "\",\"lastAuthor\":"); sb_json_str(&sb, m_la);
    SB_LIT(&sb, ",\"authors\":"); sb_append(&sb, m_aj, strlen(m_aj));
    SB_LIT(&sb, "}}");
    free(body.data);
    if (sb.data) send_json(client_fd,200,"OK",sb.data,sb.len);
    else send_json(client_fd,500,"Internal Server Error","{\"ok\":false}",12);
    free(sb.data);
}

/* ── GET /api/wiki-export-md-zip ───────────────────────────── */
void handle_api_wiki_export_md_zip(http_sock_t client_fd, const char *path_qs)
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
    strbuf_t body = {0};
    char tmp[8192];
    size_t nr;
    if (strncmp(meta_line, "<!--META ", 9) != 0)
        sb_append(&body, meta_line, strlen(meta_line));
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

/* ── POST /api/wiki-export-pdf ─────────────────────────────── */
void handle_api_wiki_export_pdf(http_sock_t client_fd, const char *body)
{
    char *title = json_get_str_alloc(body, "title");
    char *meta  = json_get_str_alloc(body, "meta");
    char *art   = json_get_str_alloc(body, "body");
    if (!art || !art[0]) {
        free(title); free(meta); free(art);
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing body\"}", 35);
        return;
    }

    char work_root[1024];
    snprintf(work_root, sizeof(work_root), "%s/.tmp_pdf_%ld_%ld",
             WIKI_ROOT, (long)getpid(), (long)time(NULL));
    if (mkdir_p(work_root) != 0) {
        free(title); free(meta); free(art);
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"mkdir failed\"}", 35);
        return;
    }

    char in_html[1280], out_pdf[1280];
    snprintf(in_html, sizeof(in_html), "%s/input.html", work_root);
    snprintf(out_pdf, sizeof(out_pdf), "%s/output.pdf", work_root);

    FILE *fp = fopen(in_html, "wb");
    if (!fp) {
        free(title); free(meta); free(art);
        rmdir_recursive(work_root);
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"write html failed\"}", 40);
        return;
    }

    fputs("<!doctype html><html><head><meta charset=\"utf-8\">"
          "<style>"
          "body{font-family:Arial,'PingFang SC','Microsoft YaHei',sans-serif;"
          "margin:18mm 14mm 20mm 14mm;color:#111;font-size:14px;line-height:1.7}"
          "h1,h2,h3,h4{page-break-after:avoid;break-after:avoid-page}"
          "h1{font-size:1.45rem;border-bottom:1px solid #ddd;padding-bottom:4px}"
          "h2{font-size:1.2rem}h3{font-size:1.07rem}h4{font-size:1rem}"
          "img{max-width:100%;height:auto}pre{white-space:pre-wrap;word-break:break-word}"
          "table{border-collapse:collapse;width:100%}"
          "th,td{border:1px solid #ddd;padding:6px}"
          ".pdf-title{font-size:24px;font-weight:700;margin:0 0 8px}"
          ".pdf-meta{color:#666;margin:0 0 14px;font-size:12px}"
          ".rel-attachments{margin-top:2em;padding-top:1em;border-top:1px dashed #ddd}"
          ".rel-attachments h2{margin:0 0 .6em;font-size:1.1rem;color:#222}"
          ".rel-attachments ul{padding-left:1.2em;margin:.2em 0}"
          ".rel-attachments li{margin:.22em 0}"
          ".pdf-bookmark-shadow-root{position:absolute;left:-100000px;top:0;width:1px;height:1px;overflow:hidden}"
          ".pdf-bookmark-shadow{margin:0;padding:0;font-size:1px;line-height:1px;color:transparent;border:0}"
          ".copy-btn{display:none!important}"
          ".art-content h1::before,.art-content h2::before,.art-content h3::before,.art-content h4::before{"
          "content:attr(data-secnum);color:#57606a;font-weight:400;font-size:.88em;margin-right:.1em}"
          "</style></head><body>", fp);

    fputs("<div class=\"pdf-title\">", fp);
    wiki_fhtml(fp, title ? title : "Wiki Export");
    fputs("</div>", fp);
    if (meta && meta[0]) {
        fputs("<div class=\"pdf-meta\">", fp);
        wiki_fhtml(fp, meta);
        fputs("</div>", fp);
    }
    fputs("<div class=\"art-content\">", fp);
    fputs(art, fp);
    if (!wiki_body_has_related_attachments_section(art)) {
        wiki_write_related_attachments(fp, art, "");
    }
    fputs("</div></body></html>", fp);
    fclose(fp);

    if (!wiki_has_wkhtmltopdf()) {
        const char *err = "{\"ok\":false,\"error\":\"wkhtmltopdf not found on server\"}";
        free(title); free(meta); free(art);
        rmdir_recursive(work_root);
        send_json(client_fd, 500, "Internal Server Error", err, strlen(err));
        return;
    }

    int rc = wiki_run_wkhtmltopdf_with_outline(in_html, out_pdf);
    if (rc != 0) {
        const char *err = "{\"ok\":false,\"error\":\"wkhtmltopdf failed (linux headless may need xvfb-run)\"}";
        free(title); free(meta); free(art);
        rmdir_recursive(work_root);
        send_json(client_fd, 500, "Internal Server Error", err, strlen(err));
        return;
    }

    char base[192], dl_name[224];
    wiki_title_to_filename_base(title ? title : "wiki_export", base, sizeof(base));
    snprintf(dl_name, sizeof(dl_name), "%s.pdf", base);

    free(title); free(meta); free(art);
    if (wiki_send_download_file(client_fd, out_pdf, dl_name, "application/pdf") < 0) {
        rmdir_recursive(work_root);
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"send failed\"}", 35);
        return;
    }
    rmdir_recursive(work_root);
}

/* ── POST /api/wiki-save ──────────────────────────────────── */

void handle_api_wiki_save(http_sock_t client_fd, const char *body,
                          const char *actor, const char *ip)
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

    /* ── optimistic locking ── */
    char base_updated[64] = {0};
    json_get_str(body, "base_updated", base_updated, sizeof(base_updated));
    char force_override_str[8] = {0};
    json_get_str(body, "force_override", force_override_str, sizeof(force_override_str));
    int force_override = (strcmp(force_override_str, "true") == 0);

    if (base_updated[0] && !force_override && id[0]) {
        char chk_path[768];
        if (wiki_md_find(chk_path, sizeof(chk_path), id) == 0) {
            FILE *chkf = fopen(chk_path, "rb");
            if (chkf) {
                fseek(chkf, 0, SEEK_END);
                long fsize = ftell(chkf);
                rewind(chkf);
                char *srv_raw = malloc((size_t)(fsize + 1));
                if (srv_raw) {
                    size_t nread = fread(srv_raw, 1, (size_t)fsize, chkf);
                    srv_raw[nread] = '\0';
                }
                fclose(chkf);

                if (srv_raw && strncmp(srv_raw, "<!--META ", 9) == 0) {
                    char *meta_end = strstr(srv_raw, "-->");
                    char srv_updated[64] = {0};
                    if (meta_end) {
                        *meta_end = '\0';
                        json_get_str(srv_raw + 9, "updated", srv_updated, sizeof(srv_updated));
                        *meta_end = '-'; /* restore for safety */
                    }
                    if (srv_updated[0] && strcmp(base_updated, srv_updated) != 0) {
                        /* conflict: extract server content (skip META line) */
                        const char *srv_content = "";
                        if (meta_end) {
                            srv_content = meta_end + 3; /* skip "-->" */
                            while (*srv_content == '\n' || *srv_content == '\r') srv_content++;
                        }

                        strbuf_t resp = {0};
                        SB_LIT(&resp, "{\"ok\":false,\"error\":\"conflict\"");
                        SB_LIT(&resp, ",\"server_updated\":\"");
                        sb_append(&resp, srv_updated, strlen(srv_updated));
                        SB_LIT(&resp, "\",\"server_content\":");
                        sb_json_str(&resp, srv_content);
                        SB_LIT(&resp, "}");
                        send_json(client_fd, 409, "Conflict", resp.data, resp.len);
                        free(resp.data);
                        free(srv_raw);
                        return;
                    }
                }
                free(srv_raw);
            }
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
    char *content_snapshot = strdup(content);
    if (!content_snapshot) {
        free(content); free(html);
        send_json(client_fd,500,"Internal Server Error","{\"ok\":false,\"error\":\"oom\"}",26); return;
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
    char md_tmp_path[896];
    snprintf(md_tmp_path, sizeof(md_tmp_path), "%s.tmp", md_path);
    FILE *fp = fopen(md_tmp_path, "wb");
    if (!fp) { free(content); free(html); free(content_snapshot);
        send_json(client_fd,500,"Internal Server Error","{\"ok\":false,\"error\":\"write md\"}",33); return; }
    /* accumulate authors from old file's META */
    char last_auth[128] = {0};
    char authors_json[2048] = {0};
    const char *editor = (actor && actor[0]) ? actor : "Admin";
    snprintf(last_auth, sizeof(last_auth), "%s", editor);
    /* read old META to get existing authors list */
    if (old_md_path[0]) {
        FILE *oldf = fopen(old_md_path, "rb");
        if (oldf) {
            char oml[4096] = {0};
            fgets(oml, sizeof(oml), oldf); fclose(oldf);
            if (strncmp(oml, "<!--META ", 9) == 0) {
                char *e = strstr(oml, "-->");
                if (e) { *e = '\0';
                    json_get_raw(oml + 9, "authors", authors_json, sizeof(authors_json));
                }
            }
        }
    }
    /* add current editor to authors array (simple inline) */
    {
        char needle[320];
        snprintf(needle, sizeof(needle), "\"%s\"", editor);
        if (!strstr(authors_json, needle)) {
            size_t al = strlen(authors_json);
            if (al < 2 || authors_json[0] != '[') {
                snprintf(authors_json, sizeof(authors_json), "[\"%s\"]", editor);
            } else if (authors_json[0] == '[' && authors_json[1] == ']') {
                snprintf(authors_json, sizeof(authors_json), "[\"%s\"]", editor);
            } else if (authors_json[al - 1] == ']') {
                authors_json[al - 1] = '\0';
                snprintf(authors_json + (al - 1), sizeof(authors_json) - (al - 1),
                         ",\"%s\"]", editor);
            }
        }
    }

    strbuf_t meta = {0};
    SB_LIT(&meta, "<!--META ");
    SB_LIT(&meta,"{\"id\":"); sb_json_str(&meta,id);
    SB_LIT(&meta,",\"title\":"); sb_json_str(&meta,title);
    SB_LIT(&meta,",\"category\":"); sb_json_str(&meta,cat);
    SB_LIT(&meta,",\"created\":"); sb_json_str(&meta,created);
    SB_LIT(&meta,",\"updated\":"); sb_json_str(&meta,now);
    SB_LIT(&meta,",\"last_author\":"); sb_json_str(&meta,last_auth);
    SB_LIT(&meta,",\"authors\":"); sb_append(&meta, authors_json, strlen(authors_json));
    SB_LIT(&meta, "}-->\n");
    if (meta.data) fwrite(meta.data,1,meta.len,fp);
    fwrite(content,1,strlen(content),fp);
    if (wiki_fsync_file(fp) != 0) {
        fclose(fp);
        unlink(md_tmp_path);
        free(meta.data); free(content); free(html); free(content_snapshot);
        send_json(client_fd,500,"Internal Server Error","{\"ok\":false,\"error\":\"fsync md\"}",33); return;
    }
    fclose(fp);
    if (wiki_rename_replace(md_tmp_path, md_path) != 0) {
        unlink(md_tmp_path);
        free(meta.data); free(content); free(html); free(content_snapshot);
        send_json(client_fd,500,"Internal Server Error","{\"ok\":false,\"error\":\"rename md\"}",34); return;
    }
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
    (void)auth_wiki_md_meta_on_editor_save(id, title, cat, now, actor ? actor : "Admin");
    if (wiki_write_html_file(html_path, id, title, cat, now, html) != 0) {
        free(content_snapshot);
        free(html);
        send_json(client_fd,500,"Internal Server Error","{\"ok\":false,\"error\":\"write html\"}",35); return;
    }


    {
        char save_txn_id[128] = {0};
        auth_gen_save_txn_id(save_txn_id, sizeof(save_txn_id));
        auth_md_backup_txn(id, title, cat, content_snapshot, html, actor ? actor : "", ip ? ip : "", save_txn_id);
        auth_audit_txn(ip ? ip : "", actor ? actor : "", "wiki_save_chain", id,
                       "md/html/index committed", save_txn_id);
    }
    free(content_snapshot);
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

/* ── trash helpers ─────────────────────────────────────────── */

/* Extract raw JSON value for a key (handles strings, arrays, objects). */
static int json_get_raw(const char *json, const char *key,
                        char *out, size_t len)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t' || *p == ':' ||
           *p == '\n' || *p == '\r') p++;
    if (*p == '"') {
        p++; size_t i = 0;
        while (*p && *p != '"' && i < len - 1) {
            if (*p == '\\' && p[1]) { out[i++] = p[1]; p += 2; }
            else { out[i++] = *p++; }
        }
        out[i] = '\0'; return 0;
    }
    if (*p == '[' || *p == '{') {
        char open = *p, close = (open == '[') ? ']' : '}';
        int depth = 1; size_t i = 0;
        out[i++] = *p++;
        while (*p && depth > 0 && i < len - 1) {
            if (*p == open) depth++;
            else if (*p == close) depth--;
            out[i++] = *p++;
        }
        out[i] = '\0'; return 0;
    }
    return -1;
}

/* Move file to trash, preserving relative path from base. */
static int wiki_trash_move(const char *src, const char *base,
                           const char *trash_base)
{
    size_t blen = strlen(base);
    if (strncmp(src, base, blen) != 0) return -1;
    const char *rel = src + blen;
    while (*rel == '/' || *rel == '\\') rel++;
    char dest[1024];
    snprintf(dest, sizeof(dest), "%s/%s", trash_base, rel);
    /* ensure parent dir exists */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", dest);
    char *slash = strrchr(dir, '/');
    if (slash) {
        /* also try backslash */
        char *bs = strrchr(dir, '\\');
        if (bs > slash) slash = bs;
        *slash = '\0';
        mkdir_p(dir);
    }
    if (rename(src, dest) == 0) return 0;
    /* cross-volume fallback: copy + unlink */
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dest, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
    unlink(src);
    return 0;
}

/* ── POST /api/wiki-delete ────────────────────────────────── */

void handle_api_wiki_delete(http_sock_t client_fd, const char *body,
                            const char *actor, const char *ip)
{
    char id[128]={0}; json_get_str(body,"id",id,sizeof(id));
    if (!id[0]) { send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing id\"}",34); return; }
    for (size_t i=0;id[i];i++) {
        unsigned char c=(unsigned char)id[i];
        if (!isalnum(c)&&c!='_'&&c!='-') {
            send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"invalid id\"}",33); return;
        }
    }

    char del_time[64]; wiki_now_iso(del_time, sizeof(del_time));
    const char *del_by = actor ? actor : "";
    if (!del_by[0]) del_by = ip ? ip : "";

    char cat[512]={0}, md_path[768];
    if (wiki_md_find(md_path, sizeof(md_path), id) == 0) {
        FILE *fp = fopen(md_path,"r");
        if (fp) {
            /* read full content */
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            rewind(fp);
            char *content = malloc((size_t)(fsize + 128));
            if (content) {
                size_t nread = fread(content, 1, (size_t)fsize, fp);
                content[nread] = '\0';
                /* extract category from META */
                if (strncmp(content, "<!--META ", 9) == 0) {
                    char *end = strstr(content, "-->");
                    if (end) {
                        *end = '\0';
                        json_get_str(content + 9, "category", cat, sizeof(cat));
                        *end = '-'; /* restore marker */
                    }
                }
                fclose(fp); fp = NULL;
                /* rewrite with deleted_at / deleted_by appended to META */
                char tmp_path[1024];
                snprintf(tmp_path, sizeof(tmp_path), "%s.tmpdel", md_path);
                FILE *tf = fopen(tmp_path, "wb");
                if (tf) {
                    /* find META end and write modified META + rest */
                    char *meta_end = strstr(content, "-->");
                    if (meta_end && meta_end >= content + 10) {
                        /* write up to -->, insert deleted fields, then write --> */
                        size_t prefix_len = (size_t)(meta_end - content);
                        fwrite(content, 1, prefix_len, tf);
                        fprintf(tf, ",\"deleted_at\":\"%s\"", del_time);
                        if (del_by[0])
                            fprintf(tf, ",\"deleted_by\":\"%s\"", del_by);
                        fwrite(meta_end, 1, strlen(meta_end), tf);
                    } else {
                        fwrite(content, 1, nread, tf);
                    }
                    fclose(tf);
                    /* atomic replace */
                    wiki_rename_replace(tmp_path, md_path);
                    unlink(tmp_path); /* clean up if rename didn't */
                }
            }
            free(content);
            if (fp) fclose(fp);
        }

        /* move md to trash */
        if (wiki_trash_move(md_path, WIKI_MD_DB, WIKI_TRASH_MD) != 0) {
            /* fallback: just remove it */
            unlink(md_path);
        }
    }
    if (!cat[0] && md_path[0]) {
        char tmp_id[128];
        wiki_cat_id_from_md_abspath(md_path, cat, sizeof(cat), tmp_id, sizeof(tmp_id));
    }
    auth_wiki_md_meta_delete(id);

    /* move html to trash */
    char html_path[1024];
    if (cat[0]) snprintf(html_path,sizeof(html_path),"%s/%s/%s.html",WIKI_ROOT,cat,id);
    else        snprintf(html_path,sizeof(html_path),"%s/%s.html",WIKI_ROOT,id);
    if (wiki_trash_move(html_path, WIKI_ROOT, WIKI_TRASH_HTML) != 0)
        unlink(html_path);

    send_json(client_fd,200,"OK","{\"ok\":true}",11);
    LOG_INFO("wiki_delete id=%s actor=%s",id, del_by);
}

/* ── trash scan helper ─────────────────────────────────────── */

static void wiki_trash_scan_dir(strbuf_t *sb, int *pfirst,
                                const char *dir, const char *cat_prefix)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(child, &st) != 0) continue;

        /* skip trash html dir */
        const char *base = strrchr(child, '/');
        if (!base) base = strrchr(child, '\\');
        if (!base) base = child - 1;
        const char *fname = base + 1;

        if (S_ISDIR(st.st_mode)) {
            char new_prefix[512];
            if (cat_prefix[0])
                snprintf(new_prefix, sizeof(new_prefix), "%s/%s",
                         cat_prefix, fname);
            else
                snprintf(new_prefix, sizeof(new_prefix), "%s", fname);
            wiki_trash_scan_dir(sb, pfirst, child, new_prefix);
        } else {
            size_t nl = strlen(fname);
            if (nl < 4 || strcmp(fname + nl - 3, ".md") != 0)
                continue;
            /* parse META from file */
            char art_id[128] = {0}, art_title[512] = {0};
            char art_cat[512] = {0}, art_upd[64] = {0};
            char art_del[64] = {0}, art_dby[128] = {0};
            if (nl > 3) {
                size_t id_len = nl - 3;
                if (id_len >= sizeof(art_id)) id_len = sizeof(art_id) - 1;
                memcpy(art_id, fname, id_len);
                art_id[id_len] = '\0';
            }
            /* category from directory prefix */
            if (cat_prefix[0])
                snprintf(art_cat, sizeof(art_cat), "%s", cat_prefix);

            FILE *fp = fopen(child, "r");
            if (fp) {
                char mline[4096] = {0};
                fgets(mline, sizeof(mline), fp);
                fclose(fp);
                if (strncmp(mline, "<!--META ", 9) == 0) {
                    char *end = strstr(mline, "-->");
                    if (end) {
                        *end = '\0';
                        json_get_str(mline + 9, "title",
                                     art_title, sizeof(art_title));
                        json_get_str(mline + 9, "updated",
                                     art_upd, sizeof(art_upd));
                        json_get_str(mline + 9, "deleted_at",
                                     art_del, sizeof(art_del));
                        json_get_str(mline + 9, "deleted_by",
                                     art_dby, sizeof(art_dby));
                        if (!art_cat[0])
                            json_get_str(mline + 9, "category",
                                         art_cat, sizeof(art_cat));
                    }
                }
            }
            if (!art_title[0])
                snprintf(art_title, sizeof(art_title), "%s", art_id);

            if (*pfirst) *pfirst = 0; else SB_LIT(sb, ",");
            SB_LIT(sb, "{");
            SB_LIT(sb, "\"id\":\""); sb_append(sb, art_id, strlen(art_id));
            SB_LIT(sb, "\",\"title\":");
            sb_json_str(sb, art_title);
            SB_LIT(sb, ",\"category\":\"");
            sb_append(sb, art_cat, strlen(art_cat));
            SB_LIT(sb, "\",\"updated\":\"");
            sb_append(sb, art_upd, strlen(art_upd));
            SB_LIT(sb, "\",\"deleted_at\":\"");
            sb_append(sb, art_del, strlen(art_del));
            SB_LIT(sb, "\",\"deleted_by\":\"");
            sb_append(sb, art_dby, strlen(art_dby));
            SB_LIT(sb, "\"}");
        }
    }
    closedir(d);
}

/* ── GET /api/wiki-trash-list ──────────────────────────────── */

void handle_api_wiki_trash_list(http_sock_t client_fd)
{
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"articles\":[");
    int first = 1;
    wiki_ensure_dirs();
    mkdir_p(WIKI_TRASH_MD);
    wiki_trash_scan_dir(&sb, &first, WIKI_TRASH_MD, "");
    SB_LIT(&sb, "]}");
    if (sb.data) send_json(client_fd, 200, "OK", sb.data, sb.len);
    else send_json(client_fd, 200, "OK",
                   "{\"ok\":true,\"articles\":[]}", 24);
    free(sb.data);
}

/* ── POST /api/wiki-trash-restore ──────────────────────────── */

void handle_api_wiki_trash_restore(http_sock_t client_fd,
                                   const char *body)
{
    char id[128] = {0}, cat[512] = {0};
    json_get_str(body, "id", id, sizeof(id));
    json_get_str(body, "category", cat, sizeof(cat));
    if (!id[0]) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"missing id\"}", 34);
        return;
    }

    /* construct trash paths */
    char trash_md[1024], trash_html[1024];
    if (cat[0]) {
        snprintf(trash_md, sizeof(trash_md),
                 "%s/%s/%s.md", WIKI_TRASH_MD, cat, id);
        snprintf(trash_html, sizeof(trash_html),
                 "%s/%s/%s.html", WIKI_TRASH_HTML, cat, id);
    } else {
        snprintf(trash_md, sizeof(trash_md),
                 "%s/%s.md", WIKI_TRASH_MD, id);
        snprintf(trash_html, sizeof(trash_html),
                 "%s/%s.html", WIKI_TRASH_HTML, id);
    }

    /* check if md exists in trash */
    struct stat st;
    if (stat(trash_md, &st) != 0) {
        send_json(client_fd, 404, "Not Found",
                  "{\"ok\":false,\"error\":\"not in trash\"}", 37);
        return;
    }

    wiki_ensure_dirs();

    /* restore md */
    char dest_md[1024];
    if (cat[0]) {
        char catdir[768];
        snprintf(catdir, sizeof(catdir), "%s/%s", WIKI_MD_DB, cat);
        mkdir_p(catdir);
        snprintf(dest_md, sizeof(dest_md),
                 "%s/%s/%s.md", WIKI_MD_DB, cat, id);
    } else {
        snprintf(dest_md, sizeof(dest_md), "%s/%s.md", WIKI_MD_DB, id);
    }
    if (rename(trash_md, dest_md) != 0) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"restore md failed\"}", 42);
        return;
    }

    /* restore html if exists */
    char dest_html[1024];
    if (cat[0]) {
        char htmldir[768];
        snprintf(htmldir, sizeof(htmldir), "%s/%s", WIKI_ROOT, cat);
        mkdir_p(htmldir);
        snprintf(dest_html, sizeof(dest_html),
                 "%s/%s/%s.html", WIKI_ROOT, cat, id);
    } else {
        snprintf(dest_html, sizeof(dest_html),
                 "%s/%s.html", WIKI_ROOT, id);
    }
    if (stat(trash_html, &st) == 0)
        rename(trash_html, dest_html);

    send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
    LOG_INFO("wiki_trash_restore id=%s cat=%s", id, cat);
}

/* ── POST /api/wiki-trash-empty ────────────────────────────── */

void handle_api_wiki_trash_empty(http_sock_t client_fd,
                                 const char *body)
{
    char id[128] = {0}, cat[512] = {0};
    json_get_str(body, "id", id, sizeof(id));
    json_get_str(body, "category", cat, sizeof(cat));

    if (id[0]) {
        /* delete single item from trash */
        char trash_md[1024], trash_html[1024];
        if (cat[0]) {
            snprintf(trash_md, sizeof(trash_md),
                     "%s/%s/%s.md", WIKI_TRASH_MD, cat, id);
            snprintf(trash_html, sizeof(trash_html),
                     "%s/%s/%s.html", WIKI_TRASH_HTML, cat, id);
        } else {
            snprintf(trash_md, sizeof(trash_md),
                     "%s/%s.md", WIKI_TRASH_MD, id);
            snprintf(trash_html, sizeof(trash_html),
                     "%s/%s.html", WIKI_TRASH_HTML, id);
        }
        unlink(trash_md);
        unlink(trash_html);
    } else {
        /* empty entire trash */
        rmdir_recursive(WIKI_TRASH_MD);
        rmdir_recursive(WIKI_TRASH_HTML);
        mkdir_p(WIKI_TRASH_MD);
        mkdir_p(WIKI_TRASH_HTML);
    }
    send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
    LOG_INFO("wiki_trash_empty id=%s", id[0] ? id : "all");
}

/* ── GET /api/wiki-search ─────────────────────────────────── */

void handle_api_wiki_search(http_sock_t client_fd, const char *path_qs)
{
    char q[256] = {0};
    const char *qs = strchr(path_qs, '?');
    if (qs) {
        const char *p = strstr(qs, "q=");
        if (p) { p += 2; size_t j=0; while(*p&&*p!='&'&&j<sizeof(q)-1) q[j++]=*p++; }
    }
    url_decode_report_fn(q);
    if (!q[0]) { send_json(client_fd,400,"Bad Request","{\"ok\":false,\"error\":\"missing q\"}",32); return; }
    /* 最短查询长度：2 字符（单字符全库扫描代价过高） */
    if (strlen(q) < 2) {
        send_json(client_fd, 200, "OK", "{\"ok\":true,\"articles\":[]}", 24);
        return;
    }
    for (char *p=q; *p; p++) *p=(char)tolower((unsigned char)*p);

    clock_t t0 = clock();
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"articles\":[");
    int first = 1, count = 0;
    wiki_search_dir(&sb, &first, WIKI_MD_DB, q, &count,
                    WIKI_SEARCH_MAX_RESULTS, WIKI_SEARCH_MAX_FILE_BYTES);
    SB_LIT(&sb, "]}");

    double elapsed = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (elapsed > 2.0) {
        LOG_INFO("wiki_search SLOW: q=\"%s\" results=%d time=%.2fs", q, count, elapsed);
    }

    if (sb.data) send_json(client_fd, 200, "OK", sb.data, sb.len);
    else send_json(client_fd, 200, "OK", "{\"ok\":true,\"articles\":[]}", 24);
    free(sb.data);
}

/* ── GET /api/wiki-refresh-index ──────────────────────────── */

void handle_api_wiki_refresh_index(http_sock_t client_fd)
{
    wiki_ensure_dirs();

    char now_iso[64];
    wiki_now_iso(now_iso, sizeof(now_iso));

    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"version\":1,\"generatedAt\":");
    sb_json_str(&sb, now_iso);
    SB_LIT(&sb, ",\"articles\":[");
    int first = 1, count = 0;
    wiki_index_dir(&sb, &first, WIKI_MD_DB, &count);
    SB_LIT(&sb, "],\"categories\":[");
    int firstcat = 1;
    wiki_index_collect_dirs(&sb, WIKI_MD_DB, "", &firstcat);
    SB_LIT(&sb, "]}");

    if (!sb.data) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"oom\"}", 26);
        return;
    }

    FILE *fp = fopen(WIKI_INDEX_TMP, "wb");
    if (!fp) {
        free(sb.data);
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"open index\"}", 35);
        return;
    }
    if (fwrite(sb.data, 1, sb.len, fp) != sb.len || wiki_fsync_file(fp) != 0) {
        fclose(fp);
        unlink(WIKI_INDEX_TMP);
        free(sb.data);
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"write index\"}", 36);
        return;
    }
    fclose(fp);

    size_t bytes = sb.len;
    free(sb.data);

    if (wiki_rename_replace(WIKI_INDEX_TMP, WIKI_INDEX_FILE) != 0) {
        unlink(WIKI_INDEX_TMP);
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"rename index\"}", 37);
        return;
    }

    char resp[128];
    int rlen = snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"path\":\"/wiki/wiki-index.json\",\"articles\":%d,\"bytes\":%lu}",
        count, (unsigned long)bytes);
    send_json(client_fd, 200, "OK", resp, (size_t)rlen);
    LOG_INFO("wiki_refresh_index articles=%d bytes=%lu", count, (unsigned long)bytes);
}

/* ── GET /api/wiki-rebuild-html ───────────────────────────── */

void handle_api_wiki_rebuild_html(http_sock_t client_fd)
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

void handle_api_wiki_rename_article(http_sock_t client_fd, const char *body)
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
    } else {
        char tid[128];
        wiki_cat_id_from_md_abspath(md_path, cat, sizeof(cat), tid, sizeof(tid));
        strbuf_t merged = {0};
        sb_append(&merged, meta_line, strlen(meta_line));
        if (cbuf.data) sb_append(&merged, cbuf.data, cbuf.len);
        free(cbuf.data);
        cbuf = merged;
    }
    char now[64];
    wiki_now_iso(now, sizeof(now));
    if (!created[0]) {
        strncpy(created, now, sizeof(created)-1);
        created[sizeof(created)-1] = '\0';
    }

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

    (void)auth_wiki_md_meta_on_editor_save(id, new_title, cat, now, "Admin");
    wiki_rewrite_html(id, new_title, cat, now);
    send_json(client_fd,200,"OK","{\"ok\":true}",11);
    LOG_INFO("wiki_rename_article id=%s new_title=%s", id, new_title);
}

/* ── POST /api/wiki-rename-cat ────────────────────────────── */

void handle_api_wiki_rename_cat(http_sock_t client_fd, const char *body)
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

void handle_api_wiki_delete_cat(http_sock_t client_fd, const char *body)
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

void handle_api_wiki_move_article(http_sock_t client_fd, const char *body)
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
    } else {
        char tid[128];
        wiki_cat_id_from_md_abspath(md_path, old_cat, sizeof(old_cat), tid, sizeof(tid));
        strbuf_t merged = {0};
        sb_append(&merged, meta_line, strlen(meta_line));
        if (cbuf.data) sb_append(&merged, cbuf.data, cbuf.len);
        free(cbuf.data);
        cbuf = merged;
        wiki_extract_md_title(cbuf.data ? cbuf.data : "", title, sizeof(title));
        if (!title[0]) snprintf(title, sizeof(title), "%s", id);
    }

    if (strcmp(old_cat,new_cat)==0) {
        free(cbuf.data);
        send_json(client_fd,200,"OK","{\"ok\":true}",11); return;
    }

    char now_move[64];
    wiki_now_iso(now_move, sizeof(now_move));
    if (!created[0]) {
        strncpy(created, now_move, sizeof(created)-1);
        created[sizeof(created)-1] = '\0';
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

    (void)auth_wiki_md_meta_update_category(id, new_cat, now_move);

    wiki_write_html_file(new_html, id, title, new_cat, now_move, hbody.data ? hbody.data : "");
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
        SB_LIT(&mb,",\"updated\":"); sb_json_str(&mb,now_move);
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

void handle_api_wiki_mkdir(http_sock_t client_fd, const char *body)
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

void handle_api_wiki_cleanup_uploads(http_sock_t client_fd)
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

void handle_api_wiki_upload(http_sock_t client_fd, const char *req_headers,
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
    char final_name[256]={0};
    if (wiki_upload_unique_name(final_name, sizeof(final_name), upload_dir, filename) != 0) {
        send_json(client_fd,500,"Internal Server Error","{\"ok\":false,\"error\":\"name conflict\"}",38); return;
    }
    char filepath[1024]; snprintf(filepath,sizeof(filepath),"%s/%s",upload_dir,final_name);
    FILE *fp = fopen(filepath,"wb");
    if (!fp) { send_json(client_fd,500,"Internal Server Error","{\"ok\":false,\"error\":\"open\"}",29); return; }
    if (body_len>0) fwrite(body,1,body_len,fp);
    fclose(fp);
    strbuf_t sb={0};
    SB_LIT(&sb,"{\"ok\":true,\"url\":\"/wiki/uploads/");
    if (cat[0] && register_subdir_safe(cat)) {
        sb_append(&sb,cat,strlen(cat)); SB_LIT(&sb,"/");
    }
    sb_append(&sb,final_name,strlen(final_name));
    SB_LIT(&sb,"\"}");
    if (sb.data) send_json(client_fd,200,"OK",sb.data,sb.len);
    else send_json(client_fd,200,"OK","{\"ok\":true}",11);
    free(sb.data);
    LOG_INFO("wiki_upload %s (%zu bytes)",filepath,body_len);
}

/* ── GET/POST /api/wiki-notewiki-prefs ─────────────────────────
 * 侧栏「文件夹筛选」JSON：空串表示显示全部；否则为非空 JSON 数组如 ["a","b"] */
static int wiki_json_extract_folder_filter(const char *body, char *out, size_t cap)
{
    if (!body || !out || cap < 2) return -1;
    out[0] = '\0';
    const char *p = strstr(body, "\"folderFilter\"");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (strncmp(p, "null", 4) == 0) return 0;
    if (*p != '[') return -1;
    const char *start = p;
    int depth = 0;
    for (; *p; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') {
            depth--;
            if (depth == 0) {
                p++;
                break;
            }
        }
    }
    if (depth != 0) return -1;
    size_t len = (size_t)(p - start);
    if (len >= cap) len = cap - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

void handle_api_wiki_notewiki_prefs_get(http_sock_t client_fd, const char *req_headers)
{
    char userkey[96] = "__guest__";
    auth_user_t u;
    if (auth_resolve_user_from_headers(req_headers, &u) == 0 && u.logged_in && u.username[0])
        snprintf(userkey, sizeof(userkey), "%s", u.username);

    char stored[4096];
    stored[0] = '\0';
    if (auth_notewiki_prefs_get(userkey, stored, sizeof(stored)) != 0) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"prefs read failed\"}", 42);
        return;
    }
    strbuf_t sb = {0};
    SB_LIT(&sb, "{\"ok\":true,\"folderFilter\":");
    if (!stored[0])
        SB_LIT(&sb, "null}");
    else {
        sb_append(&sb, stored, strlen(stored));
        SB_LIT(&sb, "}");
    }
    if (sb.data) send_json(client_fd, 200, "OK", sb.data, sb.len);
    else send_json(client_fd, 200, "OK", "{\"ok\":true,\"folderFilter\":null}", 32);
    free(sb.data);
}

void handle_api_wiki_notewiki_prefs_post(http_sock_t client_fd, const char *req_headers,
                                          const char *body)
{
    if (!body || !body[0]) {
        send_json(client_fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"empty body\"}", 35);
        return;
    }
    char userkey[96] = "__guest__";
    auth_user_t u;
    if (auth_resolve_user_from_headers(req_headers, &u) == 0 && u.logged_in && u.username[0])
        snprintf(userkey, sizeof(userkey), "%s", u.username);

    char extracted[4096];
    if (wiki_json_extract_folder_filter(body, extracted, sizeof(extracted)) != 0) {
        send_json(client_fd, 400, "Bad Request",
                  "{\"ok\":false,\"error\":\"bad folderFilter\"}", 39);
        return;
    }
    if (auth_notewiki_prefs_set(userkey, extracted) != 0) {
        send_json(client_fd, 500, "Internal Server Error",
                  "{\"ok\":false,\"error\":\"prefs save failed\"}", 42);
        return;
    }
    send_json(client_fd, 200, "OK", "{\"ok\":true}", 11);
}
