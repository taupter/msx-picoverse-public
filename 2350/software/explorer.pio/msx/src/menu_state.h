#ifndef MENU_STATE_H
#define MENU_STATE_H

#define SCREEN_WIDTH 40

#define MENU_SHORTCUT_NONE       0
#define MENU_SHORTCUT_FLASH      1
#define MENU_SHORTCUT_MICROSD    2
#define MENU_SHORTCUT_FILEHUNTER 3

extern int paging_enabled;
extern int use_80_columns;
extern unsigned char name_col_width;
extern int frame_rendered;
extern unsigned char menu_shortcut_selection;

#endif
