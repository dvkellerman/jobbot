#include "match.h"

#include <stdlib.h>
#include <string.h>

#include "text.h"
#include "util.h"

void keywords_parse(keyword_list *k, const char *csv)
{
    k->words = NULL;
    k->n = 0;
    if (!csv)
        return;

    const char *p = csv;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char *word = xstrndup(p, len);
        text_squash(word);
        if (*word) {
            k->words = xrealloc(k->words, (k->n + 1) * sizeof *k->words);
            k->words[k->n++] = text_fold(word);
        }
        free(word);
        p += len + (comma ? 1 : 0);
    }
}

void keywords_free(keyword_list *k)
{
    for (size_t i = 0; i < k->n; i++)
        free(k->words[i]);
    free(k->words);
    k->words = NULL;
    k->n = 0;
}

const char *keywords_find(const keyword_list *k, const char *folded_text)
{
    for (size_t i = 0; i < k->n; i++)
        if (text_has_word(folded_text, k->words[i]))
            return k->words[i];
    return NULL;
}

void matcher_free(matcher *m)
{
    keywords_free(&m->include);
    keywords_free(&m->exclude);
}

/* Folded, single-line copy of `s` ("" for NULL). */
static char *normalized(const char *s)
{
    char *copy = xstrdup(s ? s : "");
    text_squash(copy);
    char *folded = text_fold(copy);
    free(copy);
    return folded;
}

match_result match_job(const matcher *m, const job *j)
{
    match_result r = {0, NULL, NULL};
    char *title = normalized(j->title);

    r.included_by = keywords_find(&m->include, title);
    if (!r.included_by && m->match_tags && j->tags) {
        char *tags = normalized(j->tags);
        r.included_by = keywords_find(&m->include, tags);
        free(tags);
    }
    r.excluded_by = keywords_find(&m->exclude, title);
    r.matched = r.included_by != NULL && r.excluded_by == NULL;

    free(title);
    return r;
}

/* Appends the letters and digits of `folded` to `out`, dropping everything
 * else, so "Acme, Inc." and "ACME Inc" compare equal. */
static void append_alnum(buf *out, const char *folded)
{
    for (const unsigned char *p = (const unsigned char *)folded; *p; p++) {
        int ascii_alnum = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z');
        if (ascii_alnum || *p >= 0x80)
            buf_putc(out, (char)*p);
    }
}

char *job_fingerprint(const job *j)
{
    if (str_blank(j->company) || str_blank(j->title))
        return NULL;

    char *company = normalized(j->company);
    char *title = normalized(j->title);
    buf out;
    buf_init(&out);
    append_alnum(&out, company);
    size_t company_len = out.len;
    buf_putc(&out, '|');
    append_alnum(&out, title);
    int empty_side = company_len == 0 || out.len == company_len + 1;
    free(company);
    free(title);

    if (empty_side) {
        buf_free(&out);
        return NULL;
    }
    return buf_steal(&out);
}
