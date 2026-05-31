#include <string.h>
#include <stdint.h>
#include "menu_ui.h"
#include "menu.h"
#include "menu_input.h"

__at (BIOS_HWVER) unsigned char msx_version;
__at (BIOS_LINL40) unsigned char text_columns;
__at (BIOS_RG4SAV) unsigned char vdp_reg4;

#define MENU_UI_HLINE_SOURCE_CHAR 0x17
#define MENU_UI_HLINE_PRINT_CHAR  0x7E

static unsigned char menu_ui_render_shortcuts(unsigned char content_width);
static unsigned char menu_ui_shortcuts_length(void);
static void menu_ui_wait_key_with_blinking_status(const char *text);

static void clear_rows_vram(unsigned char start_row, unsigned char end_row, unsigned char width) __naked
{
    (void)start_row;
    (void)end_row;
    (void)width;
    __asm
    ld      hl, #2
    add     hl, sp
    ld      e, (hl)
    inc     hl
    ld      d, (hl)
    inc     hl
    ld      c, (hl)

    ld      a, d
    sub     e
    jr      z, clear_rows_done
    ld      b, a

    ld      hl, (#BIOS_TXTNAM)
    ld      a, e
    ld      e, c
    ld      d, #0
    or      a
    jr      z, clear_rows_calc_done

clear_rows_calc_loop:
    add     hl, de
    dec     a
    jr      nz, clear_rows_calc_loop

clear_rows_calc_done:
clear_rows_loop:
    push    bc
    push    de
    push    hl
    ld      a, #32
    ld      b, #0
    ld      iy,(#BIOS_EXPTBL-1)
    push    ix
    ld      ix,#BIOS_FILVRM
    call    BIOS_CALSLT
    pop     ix
    pop     hl
    pop     de
    pop     bc
    add     hl, de
    djnz    clear_rows_loop

clear_rows_done:
    ret
    __endasm;
}

int menu_ui_supports_80_column_mode(void) {
    return msx_version >= 1;
}

const char *menu_ui_status_text(const char *text_40, const char *text_80) {
    return use_80_columns ? text_80 : text_40;
}

static void menu_ui_copy_char_pattern(unsigned char source_char, unsigned char target_char)
{
    unsigned int base = ((unsigned int)vdp_reg4) << 11;
    unsigned int source_address = base + ((unsigned int)source_char * 8);
    unsigned int target_address = base + ((unsigned int)target_char * 8);

    for (unsigned char i = 0; i < 8; i++) {
        Vpoke(target_address + i, Vpeek(source_address + i));
    }
}

void menu_ui_set_text_mode(int enable_80) {
    if (enable_80) {
        text_columns = 80;
        use_80_columns = 1;
        name_col_width = NAME_COL_WIDTH_80;
    } else {
        text_columns = 40;
        use_80_columns = 0;
        name_col_width = NAME_COL_WIDTH;
    }
    __asm
    ld     iy,(#BIOS_EXPTBL-1)
    push   ix
    ld     ix,#BIOS_INITXT
    call   BIOS_CALSLT
    pop    ix
    __endasm;

    menu_ui_copy_char_pattern(MENU_UI_HLINE_SOURCE_CHAR, MENU_UI_HLINE_PRINT_CHAR);
}

void menu_ui_init_text_mode(void) {
    int enable_80 = (!MENU_FORCE_40_COLUMNS && menu_ui_supports_80_column_mode());
    menu_ui_set_text_mode(enable_80);
}

void invert_chars(unsigned char startChar, unsigned char endChar)
{
    unsigned int srcAddress, dstAddress;
    unsigned char patternByte;
    unsigned char i, c;
    unsigned int base = ((unsigned int)vdp_reg4) << 11;

    for (c = startChar; c <= endChar; c++)
    {
        srcAddress  = base + ((unsigned int)c * 8);
        dstAddress = srcAddress + (96 * 8);

        for (i = 0; i < 8; i++)
        {
            patternByte = Vpeek(srcAddress + i);
            patternByte = ~patternByte;
            Vpoke(dstAddress  + i, patternByte);
        }
    }
}

int menu_ui_try_toggle_columns(void) {
    int target_80 = !use_80_columns;
    if (target_80) {
        if (MENU_FORCE_40_COLUMNS) {
            menu_ui_wait_key_with_blinking_status("80-col off");
            menu_ui_clear_last_line();
            return 0;
        }
        if (!menu_ui_supports_80_column_mode()) {
            menu_ui_wait_key_with_blinking_status("No 80-column");
            menu_ui_clear_last_line();
            return 0;
        }
    }
    menu_ui_set_text_mode(target_80);
    invert_chars(32, 126);
    return 1;
}

unsigned char menu_ui_row_width(void) {
    return (unsigned char)(use_80_columns ? 80 : SCREEN_WIDTH);
}

void menu_ui_clear_rows(unsigned char start_row, unsigned char end_row) {
    if (start_row >= end_row) {
        return;
    }
    clear_rows_vram(start_row, end_row, menu_ui_row_width());
}

void menu_ui_print_title_line(void) {
    if (use_80_columns) {
        printf("MSX PICOVERSE 2350%44s[EXPLORER %s]", "", EXPLORER_VERSION);
    } else {
        printf("MSX PICOVERSE 2350%4s[EXPLORER %s]", "", EXPLORER_VERSION);
    }
}

void menu_ui_print_delimiter_line(void) {
    unsigned char width = use_80_columns ? 78 : 38;
    for (unsigned char i = 0; i < width; i++) {
        PrintChar(MENU_UI_HLINE_PRINT_CHAR);
    }
}

void menu_ui_print_footer_line(void) {
    unsigned char width = menu_ui_row_width();
    unsigned char content_width = (unsigned char)(width - 2);
    unsigned char footer_len = menu_ui_shortcuts_length();
    unsigned char col;

    if (use_80_columns) {
        printf("Page: %02d/%02d", currentPage, totalPages);
    } else {
        printf("Page: %02d/%02d", currentPage, totalPages);
    }

    col = 11;
    while (col < content_width && (unsigned char)(content_width - col) > footer_len) {
        PrintChar(' ');
        col++;
    }

    col = (unsigned char)(col + menu_ui_render_shortcuts((unsigned char)(content_width - col)));

    while (col < content_width) {
        PrintChar(' ');
        col++;
    }
}

static unsigned char menu_ui_print_shortcut_token(const char *token, int selected) {
    unsigned char len = (unsigned char)strlen(token);

    for (unsigned char i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)token[i];
        if (selected) {
            ch = (unsigned char)(ch + 96);
        }
        PrintChar(ch);
    }

    return len;
}

static unsigned char menu_ui_shortcuts_length(void) {
    const char *tokens_80[] = {
        "[F1 - Flash]",
        "[F2 - MicroSD]",
        "[F3 - File Hunter]"
    };
    const char *tokens_40[] = {
        "[F1-FL]",
        "[F2-SD]",
        "[F3-FH]"
    };
    const char **tokens = use_80_columns ? tokens_80 : tokens_40;
    unsigned char total_len = 0;

    for (unsigned char i = 0; i < 3; i++) {
        total_len = (unsigned char)(total_len + (unsigned char)strlen(tokens[i]));
    }

    return (unsigned char)(total_len + 2);
}

static unsigned char menu_ui_render_shortcuts(unsigned char content_width) {
    const char *tokens_80[] = {
        "[F1 - Flash]",
        "[F2 - MicroSD]",
        "[F3 - File Hunter]"
    };
    const char *tokens_40[] = {
        "[F1-FL]",
        "[F2-SD]",
        "[F3-FH]"
    };
    const char **tokens = use_80_columns ? tokens_80 : tokens_40;
    unsigned char total_len = 0;
    unsigned char col = 0;

    total_len = menu_ui_shortcuts_length();

    if (total_len > content_width) {
        total_len = content_width;
    }

    for (unsigned char i = 0; i < 3 && col < content_width; i++) {
        unsigned char token_len = menu_ui_print_shortcut_token(tokens[i], menu_shortcut_selection == (unsigned char)(i + 1));
        col = (unsigned char)(col + token_len);
        if (i < 2 && col < content_width) {
            PrintChar(' ');
            col++;
        }
    }

    return total_len;
}

static void menu_ui_render_last_line(const char *left_text) {
    unsigned char width = menu_ui_row_width();
    unsigned char content_width = (unsigned char)(width - 2);
    const char *actions;
    unsigned char right_len;
    unsigned char right_start = 0;
    unsigned char col = 0;

 
    actions = use_80_columns ?
            "[/ - Search] [  H - Help  ] [F4 - WiFi Config]" :
            "[/-Fnd] [H-Hlp] [F4-WF]";
    

    right_len = (unsigned char)strlen(actions);

    Locate(0, 23);

    if (right_len < content_width) {
        right_start = (unsigned char)(content_width - right_len);
    }

    while (left_text && *left_text && col < right_start) {
        PrintChar((unsigned char)*left_text++);
        col++;
    }

    while (col < right_start) {
        PrintChar(' ');
        col++;
    }

    while (*actions && col < content_width) {
        PrintChar((unsigned char)*actions++);
        col++;
    }

    while (col < content_width) {
        PrintChar(' ');
        col++;
    }
}

void menu_ui_render_menu_frame(void) {
    Locate(0, 0);
    menu_ui_print_title_line();
    Locate(0, 1);
    menu_ui_print_delimiter_line();
    Locate(0, 21);
    menu_ui_print_delimiter_line();
    Locate(0, 22);
    menu_ui_print_footer_line();
    menu_ui_render_last_line(0);
    frame_rendered = 1;
}

void menu_ui_update_footer_page(void) {
    Locate(0, 22);
    menu_ui_print_footer_line();
}

void menu_ui_clear_last_line(void) {
    menu_ui_render_last_line(0);
}

void menu_ui_print_last_line_text(const char *text) {
    menu_ui_render_last_line(text);
}

void menu_ui_blink_last_line(const char *text, unsigned char *visible, unsigned char *tick, unsigned char period) {
    if (++(*tick) >= period) {
        *tick = 0;
        *visible = !(*visible);
        if (*visible) {
            menu_ui_print_last_line_text(text);
        } else {
            menu_ui_clear_last_line();
        }
    }
}

static void menu_ui_wait_key_with_blinking_status(const char *text) {
    unsigned char visible = 1;
    unsigned char tick = 0;

    menu_ui_print_last_line_text(text);
    while (!bios_chsns()) {
        delay_ms(20);
        menu_ui_blink_last_line(text, &visible, &tick, 8);
    }
    (void)bios_chget();
}

void menu_ui_print_str_inverted_width(const char *str, unsigned char width)
{
    if (width == 0) {
        return;
    }
    unsigned char capped_width = width > 80 ? 80 : width;
    unsigned char i = 0;

    while (i < capped_width && str[i]) {
        PrintChar((unsigned char)(str[i] + 96));
        i++;
    }
    while (i < capped_width) {
        PrintChar((unsigned char)(' ' + 96));
        i++;
    }
}

void menu_ui_render_selectable_line(unsigned char row, const char *text, int selected) {
    unsigned char width = menu_ui_row_width();
    unsigned char content_width = width > 3 ? (unsigned char)(width - 3) : 0;
    char buffer[81];
    unsigned char len = 0;

    if (content_width > 80) {
        content_width = 80;
    }

    if (text) {
        len = (unsigned char)strlen(text);
        if (len > content_width) {
            len = content_width;
        }
        memcpy(buffer, text, len);
    }
    for (unsigned char i = len; i < content_width; i++) {
        buffer[i] = ' ';
    }
    buffer[content_width] = '\0';

    Locate(0, row);
    if (selected) {
        PrintChar('>');
        if (content_width > 0) {
            menu_ui_print_str_inverted_width(buffer, content_width);
        }
    } else {
        PrintChar(' ');
        for (unsigned char i = 0; i < content_width; i++) {
            PrintChar((unsigned char)buffer[i]);
        }
    }
}
