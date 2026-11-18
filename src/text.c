#include "text.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

#include "util.h"

/* ---- UTF-8 helpers ---------------------------------------------------- */

/* Decodes one code point starting at `s`. Returns its length in bytes
 * (1-4) and stores the code point, or returns 0 when the bytes at `s` are
 * not a valid, shortest-form UTF-8 sequence. `s` must be NUL-terminated. */
static size_t utf8_decode(const unsigned char *s, unsigned *cp)
{
    unsigned char c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    size_t len;
    unsigned v, min;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
        v = c & 0x1Fu;
        min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        v = c & 0x0Fu;
        min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        v = c & 0x07u;
        min = 0x10000;
    } else {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) /* also stops at the terminating NUL */
            return 0;
        v = (v << 6) | (s[i] & 0x3Fu);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
        return 0;
    *cp = v;
    return len;
}

/* Encodes a code point that needs exactly two bytes (U+0080..U+07FF). */
static void utf8_put2(unsigned char *s, unsigned cp)
{
    s[0] = (unsigned char)(0xC0 | (cp >> 6));
    s[1] = (unsigned char)(0x80 | (cp & 0x3F));
}

static int is_space_cp(unsigned cp)
{
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v' ||
           cp == 0xA0 || cp == 0x2007 || cp == 0x202F || cp == 0x200B || cp == 0xFEFF;
}

static int is_word_cp(unsigned cp)
{
    if (cp < 0x80)
        return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
    if (cp >= 0xC0 && cp <= 0x24F)
        return cp != 0xD7 && cp != 0xF7; /* Latin letters, minus the two math signs */
    if (cp >= 0x370 && cp <= 0x3FF)
        return 1; /* Greek */
    if (cp >= 0x400 && cp <= 0x52F)
        return 1; /* Cyrillic */
    return 0;
}

/* Lower-cases a code point. Every mapping here keeps the UTF-8 length. */
static unsigned fold_cp(unsigned cp)
{
    if (cp >= 'A' && cp <= 'Z')
        return cp + 32;
    if (cp < 0xC0)
        return cp;
    if (cp <= 0xDE)
        return cp == 0xD7 ? cp : cp + 32;
    if (cp >= 0x100 && cp <= 0x17F) {
        if (cp == 0x130 || cp == 0x131 || cp == 0x138 || cp == 0x149 || cp == 0x178 || cp == 0x17F)
            return cp;
        /* Upper-case letters sit on odd code points in these two runs and on
         * even code points in the rest of the block. */
        int upper_is_odd = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
        if (upper_is_odd)
            return (cp & 1) ? cp + 1 : cp;
        return (cp & 1) ? cp : cp + 1;
    }
    if (cp >= 0x400 && cp <= 0x40F)
        return cp + 0x50;
    if (cp >= 0x410 && cp <= 0x42F)
        return cp + 0x20;
    if (cp >= 0x460 && cp <= 0x4BF)
        return (cp & 1) ? cp : cp + 1; /* includes Ukrainian Ґ (U+0490) */
    return cp;
}

void text_sanitize(char *s)
{
    unsigned char *p = (unsigned char *)s;
    while (*p) {
        unsigned cp;
        size_t n = utf8_decode(p, &cp);
        if (n == 0) {
            *p++ = '?';
            continue;
        }
        if (n == 1 && (cp < 0x20 || cp == 0x7F) && cp != '\n' && cp != '\t')
            *p = '?';
        p += n;
    }
}

void text_squash(char *s)
{
    const unsigned char *in = (const unsigned char *)s;
    char *out = s;
    int pending_space = 0;

    while (*in) {
        unsigned cp;
        size_t n = utf8_decode(in, &cp);
        if (n == 0) { /* copy an invalid byte through unchanged */
            n = 1;
            cp = 0xFFFD;
        }
        if (is_space_cp(cp)) {
            pending_space = 1;
        } else {
            if (pending_space && out != s)
                *out++ = ' ';
            pending_space = 0;
            memmove(out, in, n);
            out += n;
        }
        in += n;
    }
    *out = '\0';
}

int text_truncate(char *s, size_t max_bytes)
{
    size_t len = strlen(s);
    if (len <= max_bytes)
        return 0;
    size_t cut = max_bytes;
    /* Step back off any continuation bytes so no sequence is split. */
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80)
        cut--;
    s[cut] = '\0';
    return 1;
}

char *text_fold(const char *s)
{
    char *out = xstrdup(s);
    unsigned char *p = (unsigned char *)out;
    while (*p) {
        unsigned cp;
        size_t n = utf8_decode(p, &cp);
        if (n == 0) {
            p++;
            continue;
        }
        unsigned f = fold_cp(cp);
        if (f != cp) {
            if (n == 1)
                *p = (unsigned char)f;
            else if (n == 2)
                utf8_put2(p, f);
        }
        p += n;
    }
    return out;
}

/* Code point that ends just before `pos`, or 0 at the start of the text. */
static unsigned cp_before(const char *text, const char *pos)
{
    if (pos == text)
        return 0;
    const char *p = pos - 1;
    while (p > text && ((unsigned char)*p & 0xC0) == 0x80 && pos - p < 4)
        p--;
    unsigned cp;
    if (utf8_decode((const unsigned char *)p, &cp) == (size_t)(pos - p))
        return cp;
    return 0;
}

static unsigned cp_at(const char *pos)
{
    unsigned cp;
    if (*pos == '\0' || utf8_decode((const unsigned char *)pos, &cp) == 0)
        return 0;
    return cp;
}

int text_has_word(const char *text, const char *word)
{
    size_t wlen = strlen(word);
    if (wlen == 0)
        return 0;
    /* A boundary is only required on a side where the word itself starts or
     * ends with a letter or digit, so "c++" and ".net" can still match. */
    int need_left = is_word_cp(cp_at(word));
    int need_right = is_word_cp(cp_before(word, word + wlen));

    for (const char *hit = strstr(text, word); hit; hit = strstr(hit + 1, word)) {
        if (need_left && is_word_cp(cp_before(text, hit)))
            continue;
        if (need_right && is_word_cp(cp_at(hit + wlen)))
            continue;
        return 1;
    }
    return 0;
}

/* ---- HTML to text ----------------------------------------------------- */

static int is_block(const xmlChar *name)
{
    static const char *const blocks[] = {"p",  "div", "ul", "ol", "table", "tr", "h1",
                                         "h2", "h3",  "h4", "h5", "h6",    "section",
                                         "article", "blockquote", "pre", "dl", "dt", "dd", NULL};
    for (size_t i = 0; blocks[i]; i++)
        if (strcasecmp((const char *)name, blocks[i]) == 0)
            return 1;
    return 0;
}

static void walk(const xmlNode *node, buf *out)
{
    for (; node; node = node->next) {
        if (node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE) {
            if (node->content)
                buf_puts(out, (const char *)node->content);
            continue;
        }
        if (node->type != XML_ELEMENT_NODE)
            continue;

        const char *name = (const char *)node->name;
        if (strcasecmp(name, "script") == 0 || strcasecmp(name, "style") == 0 ||
            strcasecmp(name, "head") == 0)
            continue;
        if (strcasecmp(name, "br") == 0) {
            buf_putc(out, '\n');
            continue;
        }
        int block = is_block(node->name);
        int item = strcasecmp(name, "li") == 0;
        if (block)
            buf_putc(out, '\n');
        if (item)
            buf_puts(out, "\n- ");
        walk(node->children, out);
        if (block || item)
            buf_putc(out, '\n');
    }
}

/* Trims each line, collapses spaces within a line, and drops blank lines. */
static char *tidy_lines(const char *raw)
{
    buf out;
    buf_init(&out);
    const char *p = raw;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *line = xstrndup(p, len);
        text_squash(line);
        if (*line && strcmp(line, "-") != 0) {
            if (out.len)
                buf_putc(&out, '\n');
            buf_puts(&out, line);
        }
        free(line);
        p += len + (eol ? 1 : 0);
    }
    return buf_steal(&out);
}

char *html_to_text(const char *html)
{
    if (!html || !*html)
        return xstrdup("");
    size_t len = strlen(html);
    if (len > (size_t)INT32_MAX)
        return xstrdup("");

    htmlDocPtr doc = htmlReadMemory(html, (int)len, NULL, "UTF-8",
                                    HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
                                        HTML_PARSE_NOWARNING | HTML_PARSE_NONET);
    if (!doc)
        return xstrdup("");

    buf raw;
    buf_init(&raw);
    walk(xmlDocGetRootElement(doc), &raw);
    xmlFreeDoc(doc);

    char *text = tidy_lines(raw.data ? raw.data : "");
    buf_free(&raw);
    text_sanitize(text);
    return text;
}

char *html_to_line(const char *html)
{
    char *text = html_to_text(html);
    text_squash(text);
    return text;
}
