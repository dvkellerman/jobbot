#include "sources/rss.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "sources/source.h"
#include "text.h"
#include "util.h"

int rss_flavor_parse(const char *name, rss_flavor *flavor)
{
    if (strcmp(name, "generic") == 0)
        *flavor = RSS_GENERIC;
    else if (strcmp(name, "dou") == 0)
        *flavor = RSS_DOU;
    else if (strcmp(name, "djinni") == 0)
        *flavor = RSS_DJINNI;
    else
        return -1;
    return 0;
}

/* Text of the first child element called `name`, trimmed; NULL when absent
 * or blank. The caller frees it. */
static char *child_text(const xmlNode *parent, const char *name)
{
    for (const xmlNode *n = parent->children; n; n = n->next) {
        if (n->type != XML_ELEMENT_NODE || strcmp((const char *)n->name, name) != 0)
            continue;
        xmlChar *content = xmlNodeGetContent(n);
        if (!content)
            return NULL;
        char *text = xstrdup((const char *)content);
        xmlFree(content);
        str_trim(text);
        if (*text == '\0') {
            free(text);
            return NULL;
        }
        return text;
    }
    return NULL;
}

/* Copy of `url` without its query string and fragment. */
static char *url_without_query(const char *url)
{
    size_t len = strcspn(url, "?#");
    return xstrndup(url, len);
}

/* Splits "A, B, C" on ", " and returns the pieces; `*n` receives the count. */
static char **split_commas(const char *s, size_t *n)
{
    char **parts = NULL;
    size_t count = 0;
    const char *p = s;
    for (;;) {
        const char *sep = strstr(p, ", ");
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        parts = xrealloc(parts, (count + 1) * sizeof *parts);
        parts[count] = xstrndup(p, len);
        str_trim(parts[count]);
        count++;
        if (!sep)
            break;
        p = sep + 2;
    }
    *n = count;
    return parts;
}

static int looks_like_salary(const char *s)
{
    return strchr(s, '$') != NULL || strstr(s, "€") != NULL || strstr(s, "₴") != NULL ||
           strstr(s, "грн") != NULL;
}

/* DOU titles read "Role в Company, [salary,] place, place". Splits that into
 * the job's fields. A title without the " в " separator is left as it is. */
static void dou_split_title(job *j)
{
    static const char SEP[] = " \xD0\xB2 "; /* " в " */
    char *sep = NULL;
    for (char *p = strstr(j->title, SEP); p; p = strstr(p + 1, SEP))
        sep = p; /* the last one: a role may itself contain " в " */
    if (!sep || sep == j->title)
        return;

    size_t n = 0;
    char **parts = split_commas(sep + strlen(SEP), &n);
    if (n == 0 || *parts[0] == '\0') {
        for (size_t i = 0; i < n; i++)
            free(parts[i]);
        free(parts);
        return;
    }

    buf location;
    buf_init(&location);
    for (size_t i = 1; i < n; i++) {
        if (*parts[i] == '\0')
            continue;
        if (!j->salary && looks_like_salary(parts[i])) {
            j->salary = xstrdup(parts[i]);
            continue;
        }
        if (location.len)
            buf_puts(&location, ", ");
        buf_puts(&location, parts[i]);
    }

    j->company = xstrdup(parts[0]);
    if (location.len)
        j->location = buf_steal(&location);
    buf_free(&location);
    *sep = '\0';
    str_trim(j->title);

    for (size_t i = 0; i < n; i++)
        free(parts[i]);
    free(parts);
}

/* The digits that start the last path segment of a Djinni job link
 * ("https://djinni.co/jobs/841429-ios-developer-swift/" gives "841429"),
 * or NULL when the link does not have that shape. */
static char *djinni_id(const char *link)
{
    char *path = url_without_query(link);
    size_t len = strlen(path);
    while (len && path[len - 1] == '/')
        path[--len] = '\0';
    const char *seg = strrchr(path, '/');
    seg = seg ? seg + 1 : path;
    size_t digits = strspn(seg, "0123456789");
    char *id = digits ? xstrndup(seg, digits) : NULL;
    free(path);
    return id;
}

static void add_item(const xmlNode *item, rss_flavor flavor, job_list *out)
{
    char *title = child_text(item, "title");
    char *link = child_text(item, "link");
    char *guid = child_text(item, "guid");
    char *description = child_text(item, "description");
    char *pub_date = child_text(item, "pubDate");

    job *j = job_list_add(out);
    /* Feed text is HTML that has been XML-escaped (DOU escapes titles twice,
     * e.g. "&amp;amp;"), so run it through the HTML converter. */
    j->title = title ? html_to_line(title) : NULL;
    j->description = description ? html_to_text(description) : NULL;
    j->posted_at = pub_date ? xstrdup(pub_date) : NULL;

    if (link) {
        switch (flavor) {
        case RSS_DOU:
            j->url = url_without_query(link);
            j->ext_id = xstrdup(j->url);
            break;
        case RSS_DJINNI:
            j->url = xstrdup(link);
            j->ext_id = djinni_id(link);
            if (!j->ext_id)
                j->ext_id = xstrdup(link);
            break;
        case RSS_GENERIC:
            j->url = xstrdup(link);
            j->ext_id = xstrdup(guid ? guid : link);
            break;
        }
    }
    if (flavor == RSS_DOU && j->title)
        dou_split_title(j);

    if (!job_is_complete(j))
        job_list_drop_last(out);

    free(title);
    free(link);
    free(guid);
    free(description);
    free(pub_date);
}

static void collect_items(const xmlNode *node, rss_flavor flavor, job_list *out)
{
    for (const xmlNode *n = node; n; n = n->next) {
        if (n->type != XML_ELEMENT_NODE)
            continue;
        if (strcmp((const char *)n->name, "item") == 0)
            add_item(n, flavor, out);
        else
            collect_items(n->children, flavor, out);
    }
}

int rss_parse(const char *xml, size_t len, rss_flavor flavor, job_list *out, char *err,
              size_t errlen)
{
    if (len == 0 || len > (size_t)INT32_MAX) {
        snprintf(err, errlen, "feed is empty or too large");
        return -1;
    }
    /* No entity substitution and no network: a feed cannot make the parser
     * read local files or fetch URLs. */
    xmlDocPtr doc = xmlReadMemory(xml, (int)len, "feed.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING |
                                      XML_PARSE_NOCDATA);
    if (!doc) {
        snprintf(err, errlen, "feed is not well-formed XML");
        return -1;
    }
    const xmlNode *root = xmlDocGetRootElement(doc);
    if (!root || strcmp((const char *)root->name, "rss") != 0) {
        snprintf(err, errlen, "expected an <rss> document, got <%s>",
                 root ? (const char *)root->name : "nothing");
        xmlFreeDoc(doc);
        return -1;
    }
    collect_items(root->children, flavor, out);
    xmlFreeDoc(doc);
    return 0;
}

/* ---- source type ------------------------------------------------------ */

static const char *const RSS_KEYS[] = {"url", "flavor", NULL};

static int rss_validate(const ini_section *cfg, char *err, size_t errlen)
{
    const char *url = ini_get(cfg, "url", "");
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        snprintf(err, errlen, "'url' must be set to an http(s) feed address");
        return -1;
    }
    rss_flavor flavor;
    const char *name = ini_get(cfg, "flavor", "generic");
    if (rss_flavor_parse(name, &flavor) != 0) {
        snprintf(err, errlen, "'flavor' must be generic, dou or djinni, got '%s'", name);
        return -1;
    }
    return 0;
}

static int rss_fetch(const source_ctx *ctx, source_result *out)
{
    rss_flavor flavor = RSS_GENERIC;
    rss_flavor_parse(ini_get(ctx->cfg, "flavor", "generic"), &flavor);

    static const char *const headers[] = {
        "Accept: application/rss+xml, application/xml;q=0.9, text/xml;q=0.8, */*;q=0.5", NULL};
    http_request req = {0};
    req.url = ini_get(ctx->cfg, "url", "");
    req.headers = headers;
    req.retries = 1;

    http_response resp;
    if (source_http(ctx, &req, &resp, out->error, sizeof out->error) != 0)
        return -1;
    int rc = rss_parse(resp.body.data ? resp.body.data : "", resp.body.len, flavor, &out->jobs,
                       out->error, sizeof out->error);
    http_response_free(&resp);
    return rc;
}

const source_type source_type_rss = {
    .type = "rss",
    .keys = RSS_KEYS,
    .validate = rss_validate,
    .fetch = rss_fetch,
};
