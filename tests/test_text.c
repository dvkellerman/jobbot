#include <stdlib.h>

#include "test.h"
#include "text.h"
#include "util.h"

static void check_html(const char *html, const char *expected)
{
    char *text = html_to_text(html);
    CHECK_STR(text, expected);
    free(text);
}

static void test_html_to_text(void)
{
    check_html(NULL, "");
    check_html("", "");
    check_html("plain text", "plain text");
    check_html("<p>One</p><p>Two <b>bold</b> &amp; more</p>", "One\nTwo bold & more");
    check_html("<p>Tasks:</p><ul><li>First</li><li>Second&nbsp;item</li></ul>",
               "Tasks:\n- First\n- Second item");
    check_html("line one<br>line two<br/>   line   three  ", "line one\nline two\nline three");
    check_html("<p>&nbsp;</p><p>Text</p><p>&nbsp;</p>", "Text");
    check_html("a<script>alert(1)</script>b<style>p{}</style>c", "abc");
    check_html("&lt;not a tag&gt; &#1055;&#x440;&#1080;&#1074;&#1110;&#1090; &quot;q&quot;",
               "<not a tag> Привіт \"q\"");
    /* Unclosed and stray tags are tolerated. */
    check_html("<p>Open <b>bold<p>Next</div>", "Open bold\nNext");

    char *line = html_to_line("<p>Senior   iOS</p>\n<p>Developer &amp;amp; Lead</p>");
    CHECK_STR(line, "Senior iOS Developer &amp; Lead");
    free(line);
    /* DOU escapes titles twice; after the XML layer is removed this is what is left. */
    line = html_to_line("Unreal Engine Developer (macOS Porting &amp; Support), від&nbsp;$1200");
    CHECK_STR(line, "Unreal Engine Developer (macOS Porting & Support), від $1200");
    free(line);
}

static void test_sanitize(void)
{
    char ok[] = "Привіт, iOS — \xF0\x9F\x9A\x80 ok";
    char *copy = xstrdup(ok);
    text_sanitize(copy);
    CHECK_STR(copy, ok);
    free(copy);

    char bad[] = "a\xFFz \xC3( \xE2\x82 end\x01\ttab\nnl\x7F";
    text_sanitize(bad);
    CHECK_STR(bad, "a?z ?( ?? end?\ttab\nnl?");

    char overlong[] = "x\xC0\xAFy"; /* overlong encoding of '/' */
    text_sanitize(overlong);
    CHECK_STR(overlong, "x??y");

    char surrogate[] = "x\xED\xA0\x80y"; /* UTF-16 surrogate, not valid UTF-8 */
    text_sanitize(surrogate);
    CHECK_STR(surrogate, "x???y");

    char truncated[] = "abc\xD0"; /* sequence cut off by the end of the string */
    text_sanitize(truncated);
    CHECK_STR(truncated, "abc?");
}

static void test_squash(void)
{
    char a[] = "  Senior \t iOS\n\nDeveloper\xC2\xA0(Kyiv)  ";
    text_squash(a);
    CHECK_STR(a, "Senior iOS Developer (Kyiv)");

    char b[] = " \n\t ";
    text_squash(b);
    CHECK_STR(b, "");

    char c[] = "Київ,  Львів";
    text_squash(c);
    CHECK_STR(c, "Київ, Львів");
}

static void test_truncate(void)
{
    char a[] = "hello";
    CHECK_INT(text_truncate(a, 10), 0);
    CHECK_STR(a, "hello");
    CHECK_INT(text_truncate(a, 5), 0);
    CHECK_INT(text_truncate(a, 3), 1);
    CHECK_STR(a, "hel");

    /* "Київ" is four 2-byte characters; a cut inside one backs up to its start. */
    char b[] = "Київ";
    CHECK_INT(text_truncate(b, 5), 1);
    CHECK_STR(b, "Ки");

    char c[] = "a\xF0\x9F\x9A\x80";
    CHECK_INT(text_truncate(c, 3), 1);
    CHECK_STR(c, "a");

    char d[] = "abc";
    CHECK_INT(text_truncate(d, 0), 1);
    CHECK_STR(d, "");
}

static void check_fold(const char *in, const char *expected)
{
    char *f = text_fold(in);
    CHECK_STR(f, expected);
    free(f);
}

static void test_fold(void)
{
    check_fold("iOS Developer", "ios developer");
    check_fold("РОЗРОБНИК Їжак Ґанок ЄВРО", "розробник їжак ґанок євро");
    check_fold("INŻYNIER ŁÓDŹ Ąę ŚĆ", "inżynier łódź ąę ść");
    check_fold("Straße × ÷ ÿ", "straße × ÷ ÿ");
    check_fold("a\xFFZ", "a\xFFz"); /* invalid bytes pass through */
}

static void test_has_word(void)
{
    CHECK(text_has_word("senior ios developer", "ios"));
    CHECK(text_has_word("ios", "ios"));
    CHECK(text_has_word("native ios/android", "ios"));
    CHECK(text_has_word("developer (ios)", "ios"));
    CHECK(text_has_word("ios-розробник", "ios"));
    CHECK(text_has_word("ios–android", "ios")); /* en dash is not a letter */
    CHECK(!text_has_word("kiosk developer", "ios"));
    CHECK(!text_has_word("test scenarios", "ios"));
    CHECK(!text_has_word("ios26", "ios"));
    CHECK(!text_has_word("розробникios", "ios")); /* glued to a Cyrillic letter */
    CHECK(!text_has_word("hlasm / z/os", "ios"));
    CHECK(!text_has_word("swiftui engineer", "swift"));
    CHECK(text_has_word("swiftui engineer", "swiftui"));
    CHECK(text_has_word("senior react native developer", "react native"));
    CHECK(!text_has_word("reactnative", "react native"));
    CHECK(text_has_word("старший розробник ios", "розробник"));
    CHECK(!text_has_word("розробники", "розробник"));
    /* A keyword that ends in punctuation needs no boundary on that side. */
    CHECK(text_has_word("c++ developer", "c++"));
    CHECK(text_has_word("senior c++/qt", "c++"));
    CHECK(!text_has_word("objc++", "c++"));
    CHECK(text_has_word("asp.net developer", ".net"));
    /* A later occurrence can match after an earlier one fails. */
    CHECK(text_has_word("kiosk and ios", "ios"));
    CHECK(!text_has_word("anything", ""));
    CHECK(!text_has_word("", "ios"));
}

int main(void)
{
    test_html_to_text();
    test_sanitize();
    test_squash();
    test_truncate();
    test_fold();
    test_has_word();
    return test_report();
}
