#include "sources/htmlcards.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/HTMLparser.h>
#include <libxml/uri.h>
#include <libxml/xpath.h>

#include "text.h"
#include "util.h"

/* Evaluates `expr` relative to `node` and returns the result as a
 * single-line string, or NULL when the expression is NULL, fails, or yields
 * nothing but whitespace. */
static char *eval_text(xmlXPathContextPtr xp, xmlNodePtr node, const char *expr)
{
    if (!expr)
        return NULL;
    xmlXPathObjectPtr obj = xmlXPathNodeEval(node, (const xmlChar *)expr, xp);
    if (!obj)
        return NULL;
    xmlChar *value = xmlXPathCastToString(obj);
    xmlXPathFreeObject(obj);
    if (!value)
        return NULL;
    char *text = xstrdup((const char *)value);
    xmlFree(value);
    text_squash(text);
    if (*text == '\0') {
        free(text);
        return NULL;
    }
    return text;
}

/* Removes elements whose contents are not text a visitor reads (inline
 * style sheets, scripts, icons), so they do not end up in titles. Boards
 * built with CSS-in-JS put <style> elements in the middle of a card. */
static void prune_unreadable(xmlNodePtr parent)
{
    static const char *const SKIP[] = {"style", "script", "noscript", "template", "svg", NULL};
    xmlNodePtr next;
    for (xmlNodePtr n = parent->children; n; n = next) {
        next = n->next;
        if (n->type != XML_ELEMENT_NODE)
            continue;
        int skip = 0;
        for (size_t i = 0; SKIP[i] && !skip; i++)
            skip = strcmp((const char *)n->name, SKIP[i]) == 0;
        if (skip) {
            xmlUnlinkNode(n);
            xmlFreeNode(n);
        } else {
            prune_unreadable(n);
        }
    }
}

static int has_ext_id(const job_list *l, const char *ext_id)
{
    for (size_t i = 0; i < l->n; i++)
        if (l->items[i].ext_id && strcmp(l->items[i].ext_id, ext_id) == 0)
            return 1;
    return 0;
}

/* Makes `href` absolute and optionally drops its query string. Returns NULL
 * when the result is not an http(s) address. */
static char *card_url(const char *href, const char *base_url, int strip_query)
{
    char *url;
    if (base_url) {
        xmlChar *abs = xmlBuildURI((const xmlChar *)href, (const xmlChar *)base_url);
        if (!abs)
            return NULL;
        url = xstrdup((const char *)abs);
        xmlFree(abs);
    } else {
        url = xstrdup(href);
    }
    if (strip_query)
        url[strcspn(url, "?#")] = '\0';
    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
        free(url);
        return NULL;
    }
    return url;
}

int html_cards_parse(const char *html, size_t len, const char *base_url, const card_spec *spec,
                     job_list *out, char *err, size_t errlen)
{
    if (len == 0 || str_blank(html))
        return 0;
    if (len > (size_t)1 << 30) {
        snprintf(err, errlen, "page is too large");
        return -1;
    }
    htmlDocPtr doc = htmlReadMemory(html, (int)len, base_url, "UTF-8",
                                    HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
                                        HTML_PARSE_NOWARNING | HTML_PARSE_NONET);
    if (!doc) {
        snprintf(err, errlen, "could not parse the page as HTML");
        return -1;
    }
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root)
        prune_unreadable(root);
    xmlXPathContextPtr xp = xmlXPathNewContext(doc);
    xmlXPathObjectPtr items = xp ? xmlXPathEvalExpression((const xmlChar *)spec->item, xp) : NULL;
    if (!items || items->type != XPATH_NODESET) {
        snprintf(err, errlen, "could not search the page for postings");
        xmlXPathFreeObject(items);
        xmlXPathFreeContext(xp);
        xmlFreeDoc(doc);
        return -1;
    }

    int cards = items->nodesetval ? items->nodesetval->nodeNr : 0;
    int readable = 0;
    for (int i = 0; i < cards; i++) {
        xmlNodePtr node = items->nodesetval->nodeTab[i];
        char *id = eval_text(xp, node, spec->id);
        char *href = eval_text(xp, node, spec->url);
        char *title = eval_text(xp, node, spec->title);
        char *url = href ? card_url(href, base_url, spec->url_strip_query) : NULL;
        free(href);

        const char *ext_id = id;
        if (id && spec->id_after) {
            const char *cut = strrchr(id, spec->id_after);
            if (cut)
                ext_id = cut + 1;
        }
        if (!ext_id || !*ext_id || !url || !title) {
            free(id);
            free(url);
            free(title);
            continue;
        }
        readable++;
        if (has_ext_id(out, ext_id)) {
            free(id);
            free(url);
            free(title);
            continue;
        }

        job *j = job_list_add(out);
        j->ext_id = xstrdup(ext_id);
        j->url = url;
        j->title = title;
        j->company = eval_text(xp, node, spec->company);
        j->location = eval_text(xp, node, spec->location);
        j->posted_at = eval_text(xp, node, spec->posted);
        j->description = eval_text(xp, node, spec->summary);
        free(id);
    }
    xmlXPathFreeObject(items);
    xmlXPathFreeContext(xp);
    xmlFreeDoc(doc);

    if (cards > 0 && readable == 0) {
        snprintf(err, errlen,
                 "found %d result card(s) but could not read any of them; "
                 "the page layout has probably changed",
                 cards);
        return -1;
    }
    return cards;
}
