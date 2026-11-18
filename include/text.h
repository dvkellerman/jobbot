#ifndef JOBBOT_TEXT_H
#define JOBBOT_TEXT_H

#include <stddef.h>

/* Converts an HTML fragment to plain UTF-8 text: tags are removed, entities
 * decoded, block elements become line breaks and list items get a "- "
 * prefix. Returns a new string (empty for NULL or unparseable input). */
char *html_to_text(const char *html);
/* Same, flattened to a single line with runs of whitespace collapsed. */
char *html_to_line(const char *html);

/* Makes `s` safe to store and send, in place: invalid UTF-8 bytes and control
 * characters other than newline and tab become '?'. */
void text_sanitize(char *s);
/* Collapses every run of whitespace (including newlines and no-break spaces)
 * to one space and trims the ends, in place. */
void text_squash(char *s);
/* Shortens `s` in place to at most `max_bytes` bytes without splitting a
 * UTF-8 sequence. Returns 1 when the string was shortened. */
int text_truncate(char *s, size_t max_bytes);

/* Returns a new lower-cased copy of `s` for case-insensitive matching.
 * Covers ASCII, Latin-1, Latin Extended-A (Polish) and Cyrillic. */
char *text_fold(const char *s);
/* True when `word` occurs in `text` as a whole word or phrase: the characters
 * on either side must not be letters or digits. Both arguments must already
 * be folded with text_fold(). */
int text_has_word(const char *text, const char *word);

#endif
