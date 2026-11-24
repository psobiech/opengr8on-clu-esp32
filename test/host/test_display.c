// Host tests for the OLED text rows (display_text.c): glyph placement in the SSD1306 page,
// cut-off at DISPLAY_TEXT_COLUMNS, replacement of non-printable characters.
#include <stdio.h>
#include <string.h>

#include "display_text.h"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

/** @brief All bytes of page_bytes[first..last_exclusive) are zero. */
static int blank(const uint8_t *page_bytes, int first, int last_exclusive)
{
    for (int i = first; i < last_exclusive; i++) {
        if (page_bytes[i]) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    uint8_t page_bytes[DISPLAY_TEXT_WIDTH];
    static const uint8_t glyph_a[5] = {0x7e, 0x11, 0x11, 0x11, 0x7e};
    static const uint8_t glyph_question[5] = {0x02, 0x01, 0x51, 0x09, 0x06};

    memset(page_bytes, 0xff, sizeof page_bytes);
    display_text_render_row("", page_bytes);
    check("empty row is blank", blank(page_bytes, 0, DISPLAY_TEXT_WIDTH));

    display_text_render_row("A", page_bytes);
    check("first glyph in columns 0-4", memcmp(page_bytes, glyph_a, 5) == 0);
    check("rest of the row blank", blank(page_bytes, 5, DISPLAY_TEXT_WIDTH));

    display_text_render_row(" A", page_bytes);
    check("second glyph in the next 6-pixel cell", blank(page_bytes, 0, 6) && memcmp(page_bytes + 6, glyph_a, 5) == 0);

    display_text_render_row("\x01\xc3", page_bytes);
    check("non-printable shown as '?'", memcmp(page_bytes, glyph_question, 5) == 0 &&
                                            memcmp(page_bytes + 6, glyph_question, 5) == 0);

    char long_row[DISPLAY_TEXT_COLUMNS + 5];
    memset(long_row, 'A', sizeof long_row - 1);
    long_row[sizeof long_row - 1] = '\0';
    display_text_render_row(long_row, page_bytes);
    check("last cell drawn", memcmp(page_bytes + (DISPLAY_TEXT_COLUMNS - 1) * 6, glyph_a, 5) == 0);
    check("cut after DISPLAY_TEXT_COLUMNS", blank(page_bytes, DISPLAY_TEXT_COLUMNS * 6, DISPLAY_TEXT_WIDTH));

    display_text_render_row("~", page_bytes);
    check("last glyph '~' drawn", !blank(page_bytes, 0, 5));

    return failures ? 1 : 0;
}
