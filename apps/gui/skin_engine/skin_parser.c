/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2007 Nicolas Pennequin, Dan Everton, Matthias Mohr
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "file.h"
#include "misc.h"
#include "plugin.h"
#include "viewport.h"

#ifdef __PCTOOL__
#ifdef WPSEDITOR
#include "proxy.h"
#include "sysfont.h"
#else
#include "action.h"
#include "checkwps.h"
#include "audio.h"
#define DEBUGF printf
#endif /*WPSEDITOR*/
#else
#include "debug.h"
#include "language.h"
#endif /*__PCTOOL__*/

#include <ctype.h>
#include <stdbool.h>
#include "font.h"

#include "wps_internals.h"
#include "skin_engine.h"
#include "settings.h"
#include "settings_list.h"

#ifdef HAVE_LCD_BITMAP
#include "bmp.h"
#endif

#ifdef HAVE_ALBUMART
#include "playback.h"
#endif

#include "backdrop.h"

#define WPS_ERROR_INVALID_PARAM         -1

/* level of current conditional.
   -1 means we're not in a conditional. */
static int level = -1;

/* index of the last WPS_TOKEN_CONDITIONAL_OPTION
    or WPS_TOKEN_CONDITIONAL_START in current level */
static int lastcond[WPS_MAX_COND_LEVEL];

/* index of the WPS_TOKEN_CONDITIONAL in current level */
static int condindex[WPS_MAX_COND_LEVEL];

/* number of condtional options in current level */
static int numoptions[WPS_MAX_COND_LEVEL];

/* line number, debug only */
static int line_number;

/* the current viewport */
static struct skin_viewport *curr_vp;
/* the current line, linked to the above viewport */
static struct skin_line *curr_line;

#ifdef HAVE_LCD_BITMAP

#if LCD_DEPTH > 1
#define MAX_BITMAPS (MAX_IMAGES+MAX_PROGRESSBARS+1) /* WPS images + pbar bitmap + backdrop */
#else
#define MAX_BITMAPS (MAX_IMAGES+MAX_PROGRESSBARS) /* WPS images + pbar bitmap */
#endif

#define PROGRESSBAR_BMP MAX_IMAGES
#define BACKDROP_BMP    (MAX_BITMAPS-1)

/* pointers to the bitmap filenames in the WPS source */
static const char *bmp_names[MAX_BITMAPS];

#endif /* HAVE_LCD_BITMAP */

#if defined(DEBUG) || defined(SIMULATOR)
/* debugging function */
extern void print_debug_info(struct wps_data *data, int fail, int line);
extern void debug_skin_usage(void);
#endif

/* Function for parsing of details for a token. At the moment the
   function is called, the token type has already been set. The
   function must fill in the details and possibly add more tokens
   to the token array. It should return the number of chars that
   has been consumed.

   wps_bufptr points to the char following the tag (i.e. where
   details begin).
   token is the pointer to the 'main' token being parsed
   */
typedef int (*wps_tag_parse_func)(const char *wps_bufptr,
                struct wps_token *token, struct wps_data *wps_data);

struct wps_tag {
    enum wps_token_type type;
    const char name[3];
    unsigned char refresh_type;
    const wps_tag_parse_func parse_func;
};
static int skip_end_of_line(const char *wps_bufptr);
/* prototypes of all special parse functions : */
static int parse_timeout(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data);
static int parse_progressbar(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data);
static int parse_dir_level(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data);
static int parse_setting_and_lang(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data);

#ifdef HAVE_LCD_BITMAP
static int parse_viewport_display(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data);
static int parse_viewport(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data);
static int parse_statusbar_enable(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data);
static int parse_statusbar_disable(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data);
static int parse_image_display(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data);
static int parse_image_load(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data);
#endif /*HAVE_LCD_BITMAP */
#if (LCD_DEPTH > 1) || (defined(HAVE_REMOTE_LCD) && (LCD_REMOTE_DEPTH > 1))
static int parse_image_special(const char *wps_bufptr,
       struct wps_token *token, struct wps_data *wps_data);
#endif
#ifdef HAVE_ALBUMART
static int parse_albumart_load(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data);
static int parse_albumart_display(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data);
#endif /* HAVE_ALBUMART */
#ifdef HAVE_TOUCHSCREEN
static int parse_touchregion(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data);
#else
static int fulline_tag_not_supported(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data)
{
    (void)token; (void)wps_data;
    return skip_end_of_line(wps_bufptr);
}
#define parse_touchregion fulline_tag_not_supported
#endif
#ifdef CONFIG_RTC
#define WPS_RTC_REFRESH WPS_REFRESH_DYNAMIC
#else
#define WPS_RTC_REFRESH WPS_REFRESH_STATIC
#endif

/* array of available tags - those with more characters have to go first
   (e.g. "xl" and "xd" before "x"). It needs to end with the unknown token. */
static const struct wps_tag all_tags[] = {

    { WPS_TOKEN_ALIGN_CENTER,             "ac",  0,                   NULL },
    { WPS_TOKEN_ALIGN_LEFT,               "al",  0,                   NULL },
    { WPS_TOKEN_ALIGN_RIGHT,              "ar",  0,                   NULL },

    { WPS_TOKEN_BATTERY_PERCENT,          "bl",  WPS_REFRESH_DYNAMIC, NULL },
    { WPS_TOKEN_BATTERY_VOLTS,            "bv",  WPS_REFRESH_DYNAMIC, NULL },
    { WPS_TOKEN_BATTERY_TIME,             "bt",  WPS_REFRESH_DYNAMIC, NULL },
    { WPS_TOKEN_BATTERY_SLEEPTIME,        "bs",  WPS_REFRESH_DYNAMIC, NULL },
#if CONFIG_CHARGING >= CHARGING_MONITOR
    { WPS_TOKEN_BATTERY_CHARGING,         "bc",  WPS_REFRESH_DYNAMIC, NULL },
#endif
#if CONFIG_CHARGING
    { WPS_TOKEN_BATTERY_CHARGER_CONNECTED,"bp",  WPS_REFRESH_DYNAMIC, NULL },
#endif

    { WPS_TOKEN_RTC_PRESENT     ,             "cc", WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_RTC_DAY_OF_MONTH,             "cd", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_DAY_OF_MONTH_BLANK_PADDED,"ce", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_12HOUR_CFG,               "cf", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_HOUR_24_ZERO_PADDED,      "cH", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_HOUR_24,                  "ck", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_HOUR_12_ZERO_PADDED,      "cI", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_HOUR_12,                  "cl", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_MONTH,                    "cm", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_MINUTE,                   "cM", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_SECOND,                   "cS", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_YEAR_2_DIGITS,            "cy", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_YEAR_4_DIGITS,            "cY", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_AM_PM_UPPER,              "cP", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_AM_PM_LOWER,              "cp", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_WEEKDAY_NAME,             "ca", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_MONTH_NAME,               "cb", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_DAY_OF_WEEK_START_MON,    "cu", WPS_RTC_REFRESH, NULL },
    { WPS_TOKEN_RTC_DAY_OF_WEEK_START_SUN,    "cw", WPS_RTC_REFRESH, NULL },

    /* current file */
    { WPS_TOKEN_FILE_BITRATE,             "fb",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_FILE_CODEC,               "fc",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_FILE_FREQUENCY,           "ff",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_FILE_FREQUENCY_KHZ,       "fk",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_FILE_NAME_WITH_EXTENSION, "fm",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_FILE_NAME,                "fn",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_FILE_PATH,                "fp",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_FILE_SIZE,                "fs",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_FILE_VBR,                 "fv",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_FILE_DIRECTORY,           "d",   WPS_REFRESH_STATIC,
                                                           parse_dir_level },

    /* next file */
    { WPS_TOKEN_FILE_BITRATE,             "Fb",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_FILE_CODEC,               "Fc",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_FILE_FREQUENCY,           "Ff",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_FILE_FREQUENCY_KHZ,       "Fk",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_FILE_NAME_WITH_EXTENSION, "Fm",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_FILE_NAME,                "Fn",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_FILE_PATH,                "Fp",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_FILE_SIZE,                "Fs",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_FILE_VBR,                 "Fv",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_FILE_DIRECTORY,           "D",   WPS_REFRESH_STATIC,
                                                           parse_dir_level },

    /* current metadata */
    { WPS_TOKEN_METADATA_ARTIST,          "ia",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_METADATA_COMPOSER,        "ic",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_METADATA_ALBUM,           "id",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_METADATA_ALBUM_ARTIST,    "iA",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_METADATA_GROUPING,        "iG",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_METADATA_GENRE,           "ig",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_METADATA_DISC_NUMBER,     "ik",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_METADATA_TRACK_NUMBER,    "in",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_METADATA_TRACK_TITLE,     "it",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_METADATA_VERSION,         "iv",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_METADATA_YEAR,            "iy",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_METADATA_COMMENT,         "iC",  WPS_REFRESH_STATIC,  NULL },

    /* next metadata */
    { WPS_TOKEN_METADATA_ARTIST,          "Ia",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_METADATA_COMPOSER,        "Ic",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_METADATA_ALBUM,           "Id",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_METADATA_ALBUM_ARTIST,    "IA",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_METADATA_GROUPING,        "IG",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_METADATA_GENRE,           "Ig",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_METADATA_DISC_NUMBER,     "Ik",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_METADATA_TRACK_NUMBER,    "In",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_METADATA_TRACK_TITLE,     "It",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_METADATA_VERSION,         "Iv",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_METADATA_YEAR,            "Iy",  WPS_REFRESH_STATIC, NULL },
    { WPS_TOKEN_METADATA_COMMENT,         "IC",  WPS_REFRESH_STATIC, NULL },

#if (CONFIG_CODEC != MAS3507D)
    { WPS_TOKEN_SOUND_PITCH,              "Sp",  WPS_REFRESH_DYNAMIC, NULL },
#endif
#if (CONFIG_LED == LED_VIRTUAL) || defined(HAVE_REMOTE_LCD)
    { WPS_TOKEN_VLED_HDD,                 "lh",  WPS_REFRESH_DYNAMIC, NULL },
#endif

    { WPS_TOKEN_MAIN_HOLD,                "mh",  WPS_REFRESH_DYNAMIC, NULL },

#ifdef HAS_REMOTE_BUTTON_HOLD
    { WPS_TOKEN_REMOTE_HOLD,              "mr",  WPS_REFRESH_DYNAMIC, NULL },
#else
    { WPS_TOKEN_UNKNOWN,                  "mr",  0,                   NULL },
#endif

    { WPS_TOKEN_REPEAT_MODE,              "mm",  WPS_REFRESH_DYNAMIC, NULL },
    { WPS_TOKEN_PLAYBACK_STATUS,          "mp",  WPS_REFRESH_DYNAMIC, NULL },
    { WPS_TOKEN_BUTTON_VOLUME,            "mv",  WPS_REFRESH_DYNAMIC,
                                                             parse_timeout },

#ifdef HAVE_LCD_BITMAP
    { WPS_TOKEN_PEAKMETER,                "pm", WPS_REFRESH_PEAK_METER, NULL },
#else
    { WPS_TOKEN_PLAYER_PROGRESSBAR,       "pf",
      WPS_REFRESH_DYNAMIC | WPS_REFRESH_PLAYER_PROGRESS, parse_progressbar },
#endif
    { WPS_TOKEN_PROGRESSBAR,              "pb",  WPS_REFRESH_PLAYER_PROGRESS,
                                                         parse_progressbar },

    { WPS_TOKEN_VOLUME,                   "pv",  WPS_REFRESH_DYNAMIC, NULL },

    { WPS_TOKEN_TRACK_ELAPSED_PERCENT,    "px",  WPS_REFRESH_DYNAMIC, NULL },
    { WPS_TOKEN_TRACK_TIME_ELAPSED,       "pc",  WPS_REFRESH_DYNAMIC, NULL },
    { WPS_TOKEN_TRACK_TIME_REMAINING,     "pr",  WPS_REFRESH_DYNAMIC, NULL },
    { WPS_TOKEN_TRACK_LENGTH,             "pt",  WPS_REFRESH_STATIC,  NULL },

    { WPS_TOKEN_PLAYLIST_POSITION,        "pp",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_PLAYLIST_ENTRIES,         "pe",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_PLAYLIST_NAME,            "pn",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_PLAYLIST_SHUFFLE,         "ps",  WPS_REFRESH_DYNAMIC, NULL },

#ifdef HAVE_TAGCACHE
    { WPS_TOKEN_DATABASE_PLAYCOUNT,       "rp",  WPS_REFRESH_DYNAMIC, NULL },
    { WPS_TOKEN_DATABASE_RATING,          "rr",  WPS_REFRESH_DYNAMIC, NULL },
    { WPS_TOKEN_DATABASE_AUTOSCORE,       "ra",  WPS_REFRESH_DYNAMIC, NULL },
#endif

#if CONFIG_CODEC == SWCODEC
    { WPS_TOKEN_REPLAYGAIN,               "rg",  WPS_REFRESH_STATIC,  NULL },
    { WPS_TOKEN_CROSSFADE,                "xf",  WPS_REFRESH_DYNAMIC, NULL },
#endif

    { WPS_NO_TOKEN,                       "s",   WPS_REFRESH_SCROLL,  NULL },
    { WPS_TOKEN_SUBLINE_TIMEOUT,          "t",   0,  parse_timeout },

#ifdef HAVE_LCD_BITMAP
    { WPS_NO_TOKEN,                       "we",  0, parse_statusbar_enable },
    { WPS_NO_TOKEN,                       "wd",  0, parse_statusbar_disable },

    { WPS_NO_TOKEN,                       "xl",  0,       parse_image_load },

    { WPS_TOKEN_IMAGE_PRELOAD_DISPLAY,    "xd",  WPS_REFRESH_STATIC,
                                                       parse_image_display },

    { WPS_TOKEN_IMAGE_DISPLAY,            "x",   0,       parse_image_load },
#ifdef HAVE_ALBUMART
    { WPS_NO_TOKEN,                       "Cl",  0,    parse_albumart_load },
    { WPS_TOKEN_ALBUMART_DISPLAY,         "C",   WPS_REFRESH_STATIC,  parse_albumart_display },
#endif

    { WPS_VIEWPORT_ENABLE,                "Vd",  WPS_REFRESH_DYNAMIC,
                                                    parse_viewport_display },
    { WPS_NO_TOKEN,                       "V",   0,    parse_viewport      },

#if (LCD_DEPTH > 1) || (defined(HAVE_REMOTE_LCD) && (LCD_REMOTE_DEPTH > 1))
    { WPS_TOKEN_IMAGE_BACKDROP,           "X",   0,    parse_image_special },
#endif
#endif

    { WPS_TOKEN_SETTING,                  "St",  WPS_REFRESH_DYNAMIC,
                                                    parse_setting_and_lang },    
    { WPS_TOKEN_TRANSLATEDSTRING,         "Sx",  WPS_REFRESH_STATIC,
                                                    parse_setting_and_lang },
                                                    
    { WPS_TOKEN_LASTTOUCH,                "Tl",  WPS_REFRESH_DYNAMIC, parse_timeout },
    { WPS_TOKEN_CURRENT_SCREEN,           "cs",  WPS_REFRESH_DYNAMIC, NULL },
    { WPS_NO_TOKEN,                       "T",   0,    parse_touchregion      },

    { WPS_TOKEN_UNKNOWN,                  "",    0, NULL }
    /* the array MUST end with an empty string (first char is \0) */
};


/* add a skin_token_list item to the list chain. ALWAYS appended because some of the
 * chains require the order to be kept.
 */
static void add_to_ll_chain(struct skin_token_list **list, struct skin_token_list *item)
{
    if (*list == NULL)
        *list = item;
    else
    {
        struct skin_token_list *t = *list;
        while (t->next)
            t = t->next;
        t->next = item;
    }
}
/* create and init a new wpsll item.
 * passing NULL to token will alloc a new one.
 * You should only pass NULL for the token when the token type (table above)
 * is WPS_NO_TOKEN which means it is not stored automatically in the skins token array
 */
static struct skin_token_list *new_skin_token_list_item(struct wps_token *token,
                                                        void* token_data)
{
    struct skin_token_list *llitem = skin_buffer_alloc(sizeof(struct skin_token_list));
    if (!token)
        token = skin_buffer_alloc(sizeof(struct wps_token));
    if (!llitem || !token)
        return NULL;
    llitem->next = NULL;
    llitem->token = token;
	if (token_data)
	    llitem->token->value.data = token_data;
    return llitem;
}

/* Returns the number of chars that should be skipped to jump
   immediately after the first eol, i.e. to the start of the next line */
static int skip_end_of_line(const char *wps_bufptr)
{
    line_number++;
    int skip = 0;
    while(*(wps_bufptr + skip) != '\n')
        skip++;
    return ++skip;
}

/* Starts a new subline in the current line during parsing */
static bool skin_start_new_subline(struct skin_line *line, int curr_token)
{
    struct skin_subline *subline = skin_buffer_alloc(sizeof(struct skin_subline));
    if (!subline)
        return false;

    subline->first_token_idx = curr_token;
    subline->next = NULL;

    subline->line_type = 0;
    subline->time_mult = 0;

    line->curr_subline->last_token_idx = curr_token-1;
    line->curr_subline->next = subline;
    line->curr_subline = subline;
    return true;
}

static bool skin_start_new_line(struct skin_viewport *vp, int curr_token)
{
    struct skin_line *line = skin_buffer_alloc(sizeof(struct skin_line));
    struct skin_subline *subline = NULL;
    if (!line)
        return false;

    /* init the subline */
    subline = &line->sublines;
    subline->first_token_idx = curr_token;
    subline->next = NULL;
    subline->line_type = 0;
    subline->time_mult = 0;

    /* init the new line */
    line->curr_subline  = &line->sublines;
    line->next          = NULL;
    line->subline_expire_time = 0;

    /* connect to curr_line and vp pointers.
     * 1) close the previous lines subline
     * 2) connect to vp pointer
     * 3) connect to curr_line global pointer
     */
    if (curr_line)
    {
        curr_line->curr_subline->last_token_idx = curr_token - 1;
        curr_line->next = line;
        curr_line->curr_subline = NULL;
    }
    curr_line = line;
    if (!vp->lines)
        vp->lines = line;
    line_number++;
    return true;
}

#ifdef HAVE_LCD_BITMAP

static int parse_statusbar_enable(const char *wps_bufptr,
                                  struct wps_token *token,
                                  struct wps_data *wps_data)
{
    (void)token; /* Kill warnings */
    wps_data->wps_sb_tag = true;
    wps_data->show_sb_on_wps = true;
    struct skin_viewport *default_vp = find_viewport(VP_DEFAULT_LABEL, wps_data);
    if (default_vp->vp.y == 0)
    {
        default_vp->vp.y = STATUSBAR_HEIGHT;
        default_vp->vp.height -= STATUSBAR_HEIGHT;
    }
    return skip_end_of_line(wps_bufptr);
}

static int parse_statusbar_disable(const char *wps_bufptr,
                                   struct wps_token *token,
                                   struct wps_data *wps_data)
{
    (void)token; /* Kill warnings */
    wps_data->wps_sb_tag = true;
    wps_data->show_sb_on_wps = false;
    struct skin_viewport *default_vp = find_viewport(VP_DEFAULT_LABEL, wps_data);
    if (default_vp->vp.y == STATUSBAR_HEIGHT)
    {
        default_vp->vp.y = 0;
        default_vp->vp.height += STATUSBAR_HEIGHT;
    }
    return skip_end_of_line(wps_bufptr);
}

static int get_image_id(int c)
{
    if(c >= 'a' && c <= 'z')
        return c - 'a';
    else if(c >= 'A' && c <= 'Z')
        return c - 'A' + 26;
    else
        return -1;
}

static char *get_image_filename(const char *start, const char* bmpdir,
                                char *buf, int buf_size)
{
    const char *end = strchr(start, '|');

    if ( !end || (end - start) >= (buf_size - (int)ROCKBOX_DIR_LEN - 2) )
    {
        buf = "\0";
        return NULL;
    }

    int bmpdirlen = strlen(bmpdir);

    strcpy(buf, bmpdir);
    buf[bmpdirlen] = '/';
    memcpy( &buf[bmpdirlen + 1], start, end - start);
    buf[bmpdirlen + 1 + end - start] = 0;

    return buf;
}

static int parse_image_display(const char *wps_bufptr,
                               struct wps_token *token,
                               struct wps_data *wps_data)
{
    char label = wps_bufptr[0];
    int subimage;
	struct gui_img *img;;

    /* sanity check */
    img = find_image(label, wps_data);
    if (!img)
    {
        token->value.i = label; /* so debug works */
        return WPS_ERROR_INVALID_PARAM;
    }

    if ((subimage = get_image_id(wps_bufptr[1])) != -1)
    {
        if (subimage >= img->num_subimages)
            return WPS_ERROR_INVALID_PARAM;

        /* Store sub-image number to display in high bits */
        token->value.i = label | (subimage << 8);
        return 2; /* We have consumed 2 bytes */
    } else {
        token->value.i = label;
        return 1; /* We have consumed 1 byte */
    }
}

static int parse_image_load(const char *wps_bufptr,
                            struct wps_token *token,
                            struct wps_data *wps_data)
{
    const char *ptr = wps_bufptr;
    const char *pos;
    const char* filename;
    const char* id;
    const char *newline;
    int x,y;
	struct gui_img *img;

    /* format: %x|n|filename.bmp|x|y|
       or %xl|n|filename.bmp|x|y|
       or %xl|n|filename.bmp|x|y|num_subimages|
    */

    if (*ptr != '|')
        return WPS_ERROR_INVALID_PARAM;

    ptr++;

    if (!(ptr = parse_list("ssdd", NULL, '|', ptr, &id, &filename, &x, &y)))
        return WPS_ERROR_INVALID_PARAM;

    /* Check there is a terminating | */
    if (*ptr != '|')
        return WPS_ERROR_INVALID_PARAM;

    /* check the image number and load state */
    if(find_image(*id, wps_data))
    {
        /* Invalid image ID */
        return WPS_ERROR_INVALID_PARAM;
    }
    img = skin_buffer_alloc(sizeof(struct gui_img));
	if (!img)
	    return WPS_ERROR_INVALID_PARAM;
    /* save a pointer to the filename */
    img->bm.data = (char*)filename;
    if (wps_data->debug)
        DEBUGF("%s >> image parsed (label: %c)\n", __func__, *id);
    img->label = *id;
    img->x = x;
    img->y = y;
	img->num_subimages = 1;
    img->always_display = false;

    /* save current viewport */
    img->vp = &curr_vp->vp;

    if (token->type == WPS_TOKEN_IMAGE_DISPLAY)
    {
        img->always_display = true;
    }
    else
    {
        /* Parse the (optional) number of sub-images */
        ptr++;
        newline = strchr(ptr, '\n');
        pos = strchr(ptr, '|');
        if (pos && pos < newline)
            img->num_subimages = atoi(ptr);

        if (img->num_subimages <= 0)
            return WPS_ERROR_INVALID_PARAM;
    }
	struct skin_token_list *item = new_skin_token_list_item(NULL, img);
	if (!item)
	    return WPS_ERROR_INVALID_PARAM;
	add_to_ll_chain(&wps_data->images, item);

    /* Skip the rest of the line */
    return skip_end_of_line(wps_bufptr);
}

static int parse_viewport_display(const char *wps_bufptr,
                                  struct wps_token *token,
                                  struct wps_data *wps_data)
{
    (void)wps_data;
    char letter = wps_bufptr[0];

    if (letter < 'a' || letter > 'z')
    {
        /* invalid viewport tag */
        return WPS_ERROR_INVALID_PARAM;
    }
    token->value.i = letter;
    return 1;
}

static int parse_viewport(const char *wps_bufptr,
                          struct wps_token *token,
                          struct wps_data *wps_data)
{
    (void)token; /* Kill warnings */
    const char *ptr = wps_bufptr;

    const int screen =
#ifdef HAVE_REMOTE_LCD
            wps_data->remote_wps ? SCREEN_REMOTE :
#endif
                                                    SCREEN_MAIN;

    struct skin_viewport *skin_vp = skin_buffer_alloc(sizeof(struct skin_viewport));

    /* check for the optional letter to signify its a hideable viewport */
    /* %Vl|<label>|<rest of tags>| */
    skin_vp->hidden_flags = 0;
    skin_vp->label = VP_NO_LABEL;
    skin_vp->pb = NULL;
    skin_vp->lines  = NULL;
    if (curr_line)
    {
        curr_line->curr_subline->last_token_idx = wps_data->num_tokens
                        - (wps_data->num_tokens > 0 ? 1 : 0);
    }

    curr_line = NULL;
    if (!skin_start_new_line(skin_vp, wps_data->num_tokens))
        return WPS_ERROR_INVALID_PARAM;

    if (*ptr == 'l')
    {
        if (*(ptr+1) == '|')
        {
            char label = *(ptr+2);
            if (label >= 'a' && label <= 'z')
            {
                skin_vp->hidden_flags = VP_DRAW_HIDEABLE;
                skin_vp->label = label;
            }
            else
                return WPS_ERROR_INVALID_PARAM; /* malformed token: e.g. %Cl7  */
            ptr += 3;
        }
    }
    if (*ptr != '|')
        return WPS_ERROR_INVALID_PARAM;

    ptr++;
    struct viewport *vp = &skin_vp->vp;
    /* format: %V|x|y|width|height|font|fg_pattern|bg_pattern| */

    if (!(ptr = viewport_parse_viewport(vp, screen, ptr, '|')))
        return WPS_ERROR_INVALID_PARAM;

    vp->flags &= ~VP_FLAG_ALIGN_RIGHT; /* ignore right-to-left languages */
    /* Check for trailing | */
    if (*ptr != '|')
        return WPS_ERROR_INVALID_PARAM;


    struct skin_token_list *list = new_skin_token_list_item(NULL, skin_vp);
    if (!list)
        return WPS_ERROR_INVALID_PARAM;
    add_to_ll_chain(&wps_data->viewports, list);
    curr_vp = skin_vp;
    /* Skip the rest of the line */
    return skip_end_of_line(wps_bufptr);
}

#if (LCD_DEPTH > 1) || (defined(HAVE_REMOTE_LCD) && (LCD_REMOTE_DEPTH > 1))
static int parse_image_special(const char *wps_bufptr,
                               struct wps_token *token,
                               struct wps_data *wps_data)
{
    (void)wps_data; /* kill warning */
    (void)token;
    const char *pos = NULL;
    const char *newline;

    pos = strchr(wps_bufptr + 1, '|');
    newline = strchr(wps_bufptr, '\n');

    if (pos > newline)
        return WPS_ERROR_INVALID_PARAM;
#if LCD_DEPTH > 1
    if (token->type == WPS_TOKEN_IMAGE_BACKDROP)
    {
        /* format: %X|filename.bmp| */
        bmp_names[BACKDROP_BMP] = wps_bufptr + 1;
    }
#endif

    /* Skip the rest of the line */
    return skip_end_of_line(wps_bufptr);
}
#endif

#endif /* HAVE_LCD_BITMAP */

static int parse_setting_and_lang(const char *wps_bufptr,
                                 struct wps_token *token,
                                 struct wps_data *wps_data)
{
    /* NOTE: both the string validations that happen in here will
     * automatically PASS on checkwps because its too hard to get
     * settings_list.c and englinsh.lang built for it. 
     * If that ever changes remove the #ifndef __PCTOOL__'s here 
     */
    (void)wps_data;
    const char *ptr = wps_bufptr;
    const char *end;
    int i = 0;
    char temp[64];

    /* Find the setting's cfg_name */
    if (*ptr != '|')
        return WPS_ERROR_INVALID_PARAM;
    ptr++;
    end = strchr(ptr,'|');
    if (!end)
        return WPS_ERROR_INVALID_PARAM;
    strlcpy(temp, ptr,end-ptr+1);
    
    if (token->type == WPS_TOKEN_TRANSLATEDSTRING)
    {
#ifndef __PCTOOL__
        i = lang_english_to_id(temp);
        if (i < 0)
            return WPS_ERROR_INVALID_PARAM;
#endif
    }
    else
    {
        /* Find the setting */
        for (i=0; i<nb_settings; i++)
            if (settings[i].cfg_name &&
                !strncmp(settings[i].cfg_name,ptr,end-ptr) &&
                /* prevent matches on cfg_name prefixes */
                strlen(settings[i].cfg_name)==(size_t)(end-ptr))
                break;
#ifndef __PCTOOL__
        if (i == nb_settings)
            return WPS_ERROR_INVALID_PARAM;
#endif
    }
    /* Store the setting number */
    token->value.i = i;

    /* Skip the rest of the line */
    return end-ptr+2;
}


static int parse_dir_level(const char *wps_bufptr,
                           struct wps_token *token,
                           struct wps_data *wps_data)
{
    char val[] = { *wps_bufptr, '\0' };
    token->value.i = atoi(val);
    (void)wps_data; /* Kill warnings */
    return 1;
}

static int parse_timeout(const char *wps_bufptr,
                         struct wps_token *token,
                         struct wps_data *wps_data)
{
    int skip = 0;
    int val = 0;
    bool have_point = false;
    bool have_tenth = false;

    (void)wps_data; /* Kill the warning */

    while ( isdigit(*wps_bufptr) || *wps_bufptr == '.' )
    {
        if (*wps_bufptr != '.')
        {
            val *= 10;
            val += *wps_bufptr - '0';
            if (have_point)
            {
                have_tenth = true;
                wps_bufptr++;
                skip++;
                break;
            }
        }
        else
            have_point = true;

        wps_bufptr++;
        skip++;
    }

    if (have_tenth == false)
        val *= 10;

    if (val == 0 && skip == 0)
    {
        /* decide what to do if no value was specified */
        switch (token->type)
        {
            case WPS_TOKEN_SUBLINE_TIMEOUT:
                return -1;
            case WPS_TOKEN_BUTTON_VOLUME:
                val = 10;
                break;
        }
    }
    token->value.i = val;

    return skip;
}

static int parse_progressbar(const char *wps_bufptr,
                             struct wps_token *token,
                             struct wps_data *wps_data)
{
    /* %pb or %pb|filename|x|y|width|height|
    using - for any of the params uses "sane" values */
#ifdef HAVE_LCD_BITMAP
    enum {
        PB_FILENAME = 0,
        PB_X,
        PB_Y,
        PB_WIDTH,
        PB_HEIGHT
    };
    const char *filename;
    int x, y, height, width;
    uint32_t set = 0;
    const char *ptr = wps_bufptr;
    struct progressbar *pb = skin_buffer_alloc(sizeof(struct progressbar));
    struct skin_token_list *item = new_skin_token_list_item(token, pb);

	if (!pb || !item)
	    return WPS_ERROR_INVALID_PARAM;

    struct viewport *vp = &curr_vp->vp;
#ifndef __PCTOOL__
    int font_height = font_get(vp->font)->height;
#else
    int font_height = 8;
#endif
    /* we need to know what line number (viewport relative) this pb is,
     * so count them... */
    int line_num = -1;
    struct skin_line *line = curr_vp->lines;
    while (line)
    {
        line_num++;
        line = line->next;
    }
    pb->have_bitmap_pb = false;
	pb->bm.data = NULL; /* no bitmap specified */

    if (*wps_bufptr != '|') /* regular old style */
    {
        pb->x = 0;
        pb->width = vp->width;
        pb->height = SYSFONT_HEIGHT-2;
        pb->y = -line_num - 1; /* Will be computed during the rendering */

        curr_vp->pb = pb;
        add_to_ll_chain(&wps_data->progressbars, item);
        return 0;
    }
    ptr = wps_bufptr + 1;

    if (!(ptr = parse_list("sdddd", &set, '|', ptr, &filename,
                                                 &x, &y, &width, &height)))
        return WPS_ERROR_INVALID_PARAM;

    if (LIST_VALUE_PARSED(set, PB_FILENAME)) /* filename */
        pb->bm.data = (char*)filename;

    if (LIST_VALUE_PARSED(set, PB_X)) /* x */
        pb->x = x;
    else
        pb->x = vp->x;

    if (LIST_VALUE_PARSED(set, PB_WIDTH)) /* width */
    {
        /* A zero width causes a divide-by-zero error later, so reject it */
        if (width == 0)
            return WPS_ERROR_INVALID_PARAM;

        pb->width = width;
    }
    else
        pb->width = vp->width - pb->x;

    if (LIST_VALUE_PARSED(set, PB_HEIGHT)) /* height, default to font height */
    {
        /* A zero height makes no sense - reject it */
        if (height == 0)
            return WPS_ERROR_INVALID_PARAM;

        pb->height = height;
    }
    else
        pb->height = font_height;

    if (LIST_VALUE_PARSED(set, PB_Y)) /* y */
        pb->y = y;
    else
        pb->y = -line_num - 1; /* Will be computed during the rendering */

    curr_vp->pb = pb;
    add_to_ll_chain(&wps_data->progressbars, item);

    /* Skip the rest of the line */
    return skip_end_of_line(wps_bufptr)-1;
#else
    (void)token;

    if (*(wps_bufptr-1) == 'f')
        wps_data->full_line_progressbar = true;
    else
        wps_data->full_line_progressbar = false;

    return 0;

#endif
}

#ifdef HAVE_ALBUMART
static int parse_albumart_load(const char *wps_bufptr,
                               struct wps_token *token,
                               struct wps_data *wps_data)
{
    const char *_pos, *newline;
    bool parsing;
    struct dim dimensions;
    int albumart_slot;
    struct skin_albumart *aa = skin_buffer_alloc(sizeof(struct skin_albumart));
    (void)token; /* silence warning */
    if (!aa)
        return skip_end_of_line(wps_bufptr);

    /* reset albumart info in wps */
    aa->width = -1;
    aa->height = -1;
    aa->xalign = WPS_ALBUMART_ALIGN_CENTER; /* default */
    aa->yalign = WPS_ALBUMART_ALIGN_CENTER; /* default */
    aa->vp = &curr_vp->vp;

    /* format: %Cl|x|y|[[l|c|r]mwidth]|[[t|c|b]mheight]| */

    newline = strchr(wps_bufptr, '\n');

    /* initial validation and parsing of x and y components */
    if (*wps_bufptr != '|')
        return WPS_ERROR_INVALID_PARAM; /* malformed token: e.g. %Cl7  */

    _pos = wps_bufptr + 1;
    _pos = parse_list("dd", NULL, '|', _pos, &aa->x, &aa->y);

    if (!_pos || _pos > newline || *_pos != '|')
        return WPS_ERROR_INVALID_PARAM;  /* malformed token: no | after y coordinate
                                            e.g. %Cl|7|59\n */

    /* parsing width field */
    parsing = true;
    while (parsing)
    {
        /* apply each modifier in turn */
        ++_pos;
        switch (*_pos)
        {
            case 'l':
            case 'L':
            case '+':
                aa->xalign = WPS_ALBUMART_ALIGN_LEFT;
                break;
            case 'c':
            case 'C':
                aa->xalign = WPS_ALBUMART_ALIGN_CENTER;
                break;
            case 'r':
            case 'R':
            case '-':
                aa->xalign = WPS_ALBUMART_ALIGN_RIGHT;
                break;
            case 'd':
            case 'D':
            case 'i':
            case 'I':
            case 's':
            case 'S':
                /* simply ignored */
                break;
            default:
                parsing = false;
                break;
        }
    }
    /* extract max width data */
    if (*_pos != '|')
    {
        if (!isdigit(*_pos))   /* malformed token: e.g. %Cl|7|59|# */
            return WPS_ERROR_INVALID_PARAM;

        aa->width = atoi(_pos);

        _pos = strchr(_pos, '|');
        if (!_pos || _pos > newline)
            return WPS_ERROR_INVALID_PARAM; /* malformed token: no | after width field
                                               e.g. %Cl|7|59|200\n */
    }

    /* parsing height field */
    parsing = true;
    while (parsing)
    {
        /* apply each modifier in turn */
        ++_pos;
        switch (*_pos)
        {
            case 't':
            case 'T':
            case '-':
                aa->yalign = WPS_ALBUMART_ALIGN_TOP;
                break;
            case 'c':
            case 'C':
                aa->yalign = WPS_ALBUMART_ALIGN_CENTER;
                break;
            case 'b':
            case 'B':
            case '+':
                aa->yalign = WPS_ALBUMART_ALIGN_BOTTOM;
                break;
            case 'd':
            case 'D':
            case 'i':
            case 'I':
            case 's':
            case 'S':
                /* simply ignored */
                break;
            default:
                parsing = false;
                break;
        }
    }
    /* extract max height data */
    if (*_pos != '|')
    {
        if (!isdigit(*_pos))
            return WPS_ERROR_INVALID_PARAM; /* malformed token  e.g. %Cl|7|59|200|@  */

        aa->height = atoi(_pos);

        _pos = strchr(_pos, '|');
        if (!_pos || _pos > newline)
            return WPS_ERROR_INVALID_PARAM; /* malformed token: no closing |
                                               e.g. %Cl|7|59|200|200\n */
    }

    /* if we got here, we parsed everything ok .. ! */
    if (aa->width < 0)
        aa->width = 0;
    else if (aa->width > LCD_WIDTH)
        aa->width = LCD_WIDTH;

    if (aa->height < 0)
        aa->height = 0;
    else if (aa->height > LCD_HEIGHT)
        aa->height = LCD_HEIGHT;

    aa->state = WPS_ALBUMART_LOAD;
    aa->draw = false;
    wps_data->albumart = aa;

    dimensions.width = aa->width;
    dimensions.height = aa->height;

    albumart_slot = playback_claim_aa_slot(&dimensions);

    if (albumart_slot < 0) /* didn't get a slot ? */
        return skip_end_of_line(wps_bufptr);
    else
        wps_data->playback_aa_slot = albumart_slot;

    /* Skip the rest of the line */
    return skip_end_of_line(wps_bufptr);
}

static int parse_albumart_display(const char *wps_bufptr,
                                      struct wps_token *token,
                                      struct wps_data *wps_data)
{
    (void)wps_bufptr;
    struct wps_token *prev = token-1;
    if ((wps_data->num_tokens >= 1) && (prev->type == WPS_TOKEN_CONDITIONAL))
    {
        token->type = WPS_TOKEN_ALBUMART_FOUND;
    }
    else if (wps_data->albumart)
    {
        wps_data->albumart->vp = &curr_vp->vp;
    }
#if 0
    /* the old code did this so keep it here for now...
     * this is to allow the posibility to showing the next tracks AA! */
    if (wps_bufptr+1 == 'n')
        return 1;
#endif
    return 0;
};
#endif /* HAVE_ALBUMART */

#ifdef HAVE_TOUCHSCREEN

struct touchaction {char* s; int action;};
static struct touchaction touchactions[] = {
    {"play", ACTION_WPS_PLAY }, {"stop", ACTION_WPS_STOP },
    {"prev", ACTION_WPS_SKIPPREV }, {"next", ACTION_WPS_SKIPNEXT },
    {"ffwd", ACTION_WPS_SEEKFWD }, {"rwd", ACTION_WPS_SEEKBACK },
    {"menu", ACTION_WPS_MENU }, {"browse", ACTION_WPS_BROWSE },
    {"shuffle", ACTION_TOUCH_SHUFFLE }, {"repmode", ACTION_TOUCH_REPMODE },
    {"quickscreen", ACTION_WPS_QUICKSCREEN },{"contextmenu", ACTION_WPS_CONTEXT },
    {"playlist", ACTION_WPS_VIEW_PLAYLIST }, {"pitch", ACTION_WPS_PITCHSCREEN},
};
static int parse_touchregion(const char *wps_bufptr,
        struct wps_token *token, struct wps_data *wps_data)
{
    (void)token;
    unsigned i, imax;
    struct touchregion *region = NULL;
    const char *ptr = wps_bufptr;
    const char *action;
    const char pb_string[] = "progressbar";
    const char vol_string[] = "volume";
    int x,y,w,h;

    /* format: %T|x|y|width|height|action|
     * if action starts with & the area must be held to happen
     * action is one of:
     * play  -  play/pause playback
     * stop  -  stop playback, exit the wps
     * prev  -  prev track
     * next  -  next track
     * ffwd  -  seek forward
     * rwd   -  seek backwards
     * menu  -  go back to the main menu
     * browse - go back to the file/db browser
     * shuffle - toggle shuffle mode
     * repmode - cycle the repeat mode
     * quickscreen - go into the quickscreen
     * contextmenu - open the context menu
     * playlist - go into the playlist
     * pitch - go into the pitchscreen
    */


    if (*ptr != '|')
        return WPS_ERROR_INVALID_PARAM;
    ptr++;

    if (!(ptr = parse_list("dddds", NULL, '|', ptr, &x, &y, &w, &h, &action)))
        return WPS_ERROR_INVALID_PARAM;

    /* Check there is a terminating | */
    if (*ptr != '|')
        return WPS_ERROR_INVALID_PARAM;

    region = skin_buffer_alloc(sizeof(struct touchregion));
    if (!region)
        return WPS_ERROR_INVALID_PARAM;

    /* should probably do some bounds checking here with the viewport... but later */
    region->action = ACTION_NONE;
    region->x = x;
    region->y = y;
    region->width = w;
    region->height = h;
    region->wvp = curr_vp;

    if(!strncmp(pb_string, action, sizeof(pb_string)-1)
        && *(action + sizeof(pb_string)-1) == '|')
        region->type = WPS_TOUCHREGION_SCROLLBAR;
    else if(!strncmp(vol_string, action, sizeof(vol_string)-1)
        && *(action + sizeof(vol_string)-1) == '|')
        region->type = WPS_TOUCHREGION_VOLUME;
    else
    {
        region->type = WPS_TOUCHREGION_ACTION;

        if (*action == '&')
        {
            action++;
            region->repeat = true;
        }
        else
            region->repeat = false;

        i = 0;
        imax = ARRAYLEN(touchactions);
        while ((region->action == ACTION_NONE) &&
                (i < imax))
        {
            /* try to match with one of our touchregion screens */
            int len = strlen(touchactions[i].s);
            if (!strncmp(touchactions[i].s, action, len)
                    && *(action+len) == '|')
                region->action = touchactions[i].action;
            i++;
        }
        if (region->action == ACTION_NONE)
            return WPS_ERROR_INVALID_PARAM;
    }
    struct skin_token_list *item = new_skin_token_list_item(NULL, region);
    if (!item)
        return WPS_ERROR_INVALID_PARAM;
    add_to_ll_chain(&wps_data->touchregions, item);
    return skip_end_of_line(wps_bufptr);
}
#endif

/* Parse a generic token from the given string. Return the length read */
static int parse_token(const char *wps_bufptr, struct wps_data *wps_data)
{
    int skip = 0, taglen = 0, ret;
    struct wps_token *token = wps_data->tokens + wps_data->num_tokens;
    const struct wps_tag *tag;
    memset(token, 0, sizeof(*token));

    switch(*wps_bufptr)
    {

        case '%':
        case '<':
        case '|':
        case '>':
        case ';':
        case '#':
            /* escaped characters */
            token->type = WPS_TOKEN_CHARACTER;
            token->value.c = *wps_bufptr;
            taglen = 1;
            wps_data->num_tokens++;
            break;

        case '?':
            /* conditional tag */
            token->type = WPS_TOKEN_CONDITIONAL;
            level++;
            condindex[level] = wps_data->num_tokens;
            numoptions[level] = 1;
            wps_data->num_tokens++;
            ret = parse_token(wps_bufptr + 1, wps_data);
            if (ret < 0) return ret;
            taglen = 1 + ret;
            break;

        default:
            /* find what tag we have */
            for (tag = all_tags;
                 strncmp(wps_bufptr, tag->name, strlen(tag->name)) != 0;
                 tag++) ;

            taglen = (tag->type != WPS_TOKEN_UNKNOWN) ? strlen(tag->name) : 2;
            token->type = tag->type;
            curr_line->curr_subline->line_type |= tag->refresh_type;

            /* if the tag has a special parsing function, we call it */
            if (tag->parse_func)
            {
                ret = tag->parse_func(wps_bufptr + taglen, token, wps_data);
                if (ret < 0) return ret;
                skip += ret;
            }

            /* Some tags we don't want to save as tokens */
            if (tag->type == WPS_NO_TOKEN)
                break;

            /* tags that start with 'F', 'I' or 'D' are for the next file */
            if ( *(tag->name) == 'I' || *(tag->name) == 'F' ||
                 *(tag->name) == 'D')
                token->next = true;

            wps_data->num_tokens++;
            break;
    }

    skip += taglen;
    return skip;
}

/* Parses the WPS.
   data is the pointer to the structure where the parsed WPS should be stored.
        It is initialised.
   wps_bufptr points to the string containing the WPS tags */
#define TOKEN_BLOCK_SIZE 128
static bool wps_parse(struct wps_data *data, const char *wps_bufptr, bool debug)
{
    if (!data || !wps_bufptr || !*wps_bufptr)
        return false;
    enum wps_parse_error fail = PARSE_OK;
    int ret;
    int max_tokens = TOKEN_BLOCK_SIZE;
    size_t buf_free = 0;
    line_number = 1;
    level = -1;

    /* allocate enough RAM for a reasonable skin, grow as needed.
     * Free any used RAM before loading the images to be 100% RAM efficient */
    data->tokens = (struct wps_token *)skin_buffer_grab(&buf_free);
    if (sizeof(struct wps_token)*max_tokens >= buf_free)
        return false;
    skin_buffer_increment(max_tokens * sizeof(struct wps_token), false);
    data->num_tokens = 0;

    while (*wps_bufptr && !fail)
    {
        /* first make sure there is enough room for tokens */
        if (max_tokens <= data->num_tokens + 5)
        {
            int extra_tokens = TOKEN_BLOCK_SIZE;
            size_t needed = extra_tokens * sizeof(struct wps_token);
            /* do some smarts here to grow the array a bit */
            if (skin_buffer_freespace() < needed)
            {
                fail = PARSE_FAIL_LIMITS_EXCEEDED;
                break;
            }
            skin_buffer_increment(needed, false);
            max_tokens += extra_tokens;
        }

        switch(*wps_bufptr++)
        {

            /* Regular tag */
            case '%':
                if ((ret = parse_token(wps_bufptr, data)) < 0)
                {
                    fail = PARSE_FAIL_COND_INVALID_PARAM;
                    break;
                }
                else if (level >= WPS_MAX_COND_LEVEL - 1)
                {
                    fail = PARSE_FAIL_LIMITS_EXCEEDED;
                    break;
                }
                wps_bufptr += ret;
                break;

            /* Alternating sublines separator */
            case ';':
                if (level >= 0) /* there are unclosed conditionals */
                {
                    fail = PARSE_FAIL_UNCLOSED_COND;
                    break;
                }

                if (!skin_start_new_subline(curr_line, data->num_tokens))
                    fail = PARSE_FAIL_LIMITS_EXCEEDED;

                break;

            /* Conditional list start */
            case '<':
                if (data->tokens[data->num_tokens-2].type != WPS_TOKEN_CONDITIONAL)
                {
                    fail = PARSE_FAIL_COND_SYNTAX_ERROR;
                    break;
                }

                data->tokens[data->num_tokens].type = WPS_TOKEN_CONDITIONAL_START;
                lastcond[level] = data->num_tokens++;
                break;

            /* Conditional list end */
            case '>':
                if (level < 0) /* not in a conditional, invalid char */
                {
                    fail = PARSE_FAIL_INVALID_CHAR;
                    break;
                }

                data->tokens[data->num_tokens].type = WPS_TOKEN_CONDITIONAL_END;
                if (lastcond[level])
                    data->tokens[lastcond[level]].value.i = data->num_tokens;
                else
                {
                    fail = PARSE_FAIL_COND_SYNTAX_ERROR;
                    break;
                }

                lastcond[level] = 0;
                data->num_tokens++;
                data->tokens[condindex[level]].value.i = numoptions[level];
                level--;
                break;

            /* Conditional list option */
            case '|':
                if (level < 0) /* not in a conditional, invalid char */
                {
                    fail = PARSE_FAIL_INVALID_CHAR;
                    break;
                }

                data->tokens[data->num_tokens].type = WPS_TOKEN_CONDITIONAL_OPTION;
                if (lastcond[level])
                    data->tokens[lastcond[level]].value.i = data->num_tokens;
                else
                {
                    fail = PARSE_FAIL_COND_SYNTAX_ERROR;
                    break;
                }

                lastcond[level] = data->num_tokens;
                numoptions[level]++;
                data->num_tokens++;
                break;

            /* Comment */
            case '#':
                if (level >= 0) /* there are unclosed conditionals */
                {
                    fail = PARSE_FAIL_UNCLOSED_COND;
                    break;
                }

                wps_bufptr += skip_end_of_line(wps_bufptr);
                break;

            /* End of this line */
            case '\n':
                if (level >= 0) /* there are unclosed conditionals */
                {
                    fail = PARSE_FAIL_UNCLOSED_COND;
                    break;
                }
                /* add a new token for the \n so empty lines are correct */
                data->tokens[data->num_tokens].type = WPS_TOKEN_CHARACTER;
                data->tokens[data->num_tokens].value.c = '\n';
                data->tokens[data->num_tokens].next = false;
                data->num_tokens++;

                if (!skin_start_new_line(curr_vp, data->num_tokens))
                {
                    fail = PARSE_FAIL_LIMITS_EXCEEDED;
                    break;
                }

                break;

            /* String */
            default:
                {
                    unsigned int len = 1;
                    const char *string_start = wps_bufptr - 1;

                    /* find the length of the string */
                    while (*wps_bufptr && *wps_bufptr != '#' &&
                           *wps_bufptr != '%' && *wps_bufptr != ';' &&
                           *wps_bufptr != '<' && *wps_bufptr != '>' &&
                           *wps_bufptr != '|' && *wps_bufptr != '\n')
                    {
                        wps_bufptr++;
                        len++;
                    }

                    /* look if we already have that string */
                    char *str;
                    bool found = false;
					struct skin_token_list *list = data->strings;
					while (list)
					{
						str = (char*)list->token->value.data;
						found = (strlen(str) == len &&
                                    strncmp(string_start, str, len) == 0);
						if (found)
						    break; /* break here because the list item is
							          used if its found */
                        list = list->next;
                    }
                    /* If a matching string is found, found is true and i is
                       the index of the string. If not, found is false */

                    if (!found)
                    {
                        /* new string */
						str = (char*)skin_buffer_alloc(len+1);
						if (!str)
						{
							fail = PARSE_FAIL_LIMITS_EXCEEDED;
							break;
                        }
                        strlcpy(str, string_start, len+1);
						struct skin_token_list *item =
						    new_skin_token_list_item(&data->tokens[data->num_tokens], str);
						if(!item)
						{
							fail = PARSE_FAIL_LIMITS_EXCEEDED;
							break;
                        }
						add_to_ll_chain(&data->strings, item);
                    }
                    else
                    {
                        /* another occurrence of an existing string */
                        data->tokens[data->num_tokens].value.data = list->token->value.data;
                    }
                    data->tokens[data->num_tokens].type = WPS_TOKEN_STRING;
                    data->num_tokens++;
                }
                break;
        }
    }

    if (!fail && level >= 0) /* there are unclosed conditionals */
        fail = PARSE_FAIL_UNCLOSED_COND;

    if (*wps_bufptr && !fail)
        /* one of the limits of the while loop was exceeded */
        fail = PARSE_FAIL_LIMITS_EXCEEDED;

    /* Success! */
    curr_line->curr_subline->last_token_idx = data->num_tokens;
    data->tokens[data->num_tokens++].type = WPS_NO_TOKEN;
    /* freeup unused tokens */
    skin_buffer_free_from_front(sizeof(struct wps_token)
                                * (max_tokens - data->num_tokens));

#if defined(DEBUG) || defined(SIMULATOR)
    if (debug)
        print_debug_info(data, fail, line_number);
#else
    (void)debug;
#endif

    return (fail == 0);
}


/*
 * initial setup of wps_data; does reset everything
 * except fields which need to survive, i.e.
 * 
 *  wps_data->remote_wps
 **/
void skin_data_reset(struct wps_data *wps_data)
{
#ifdef HAVE_LCD_BITMAP
    wps_data->images = NULL;
    wps_data->progressbars = NULL;
#endif
#ifdef HAVE_TOUCHSCREEN
    wps_data->touchregions = NULL;
#endif
    wps_data->viewports = NULL;
    wps_data->strings = NULL;
#ifdef HAVE_ALBUMART
    wps_data->albumart = NULL;
    if (wps_data->playback_aa_slot >= 0)
    {
        playback_release_aa_slot(wps_data->playback_aa_slot);
        wps_data->playback_aa_slot = -1;
    }
#endif
    wps_data->tokens = NULL;
    wps_data->num_tokens = 0;
    wps_data->button_time_volume = 0;

#ifdef HAVE_LCD_BITMAP
    wps_data->peak_meter_enabled = false;
    wps_data->wps_sb_tag = false;
    wps_data->show_sb_on_wps = false;
#else /* HAVE_LCD_CHARCELLS */
    /* progress bars */
    int i;
    for (i = 0; i < 8; i++)
    {
        wps_data->wps_progress_pat[i] = 0;
    }
    wps_data->full_line_progressbar = false;

#ifdef HAVE_REMOTE_LCD
    data->remote_wps = rwps;
#endif
    wps_data->wps_loaded = false;
    if (wps_data->debug)
        DEBUGF("%s >> wps_data trashed (%p)\n", __func__, data);
}

#ifdef HAVE_LCD_BITMAP
static bool load_skin_bmp(struct wps_data *wps_data, struct bitmap *bitmap, char* bmpdir)
{
	(void)wps_data; /* only needed for remote targets */
	bool loaded = false;
    char img_path[MAX_PATH];
    get_image_filename(bitmap->data, bmpdir,
                       img_path, sizeof(img_path));

	/* load the image */
    int format;
#ifdef HAVE_REMOTE_LCD
    if (wps_data->remote_wps)
        format = FORMAT_ANY|FORMAT_REMOTE;
    else
#endif
        format = FORMAT_ANY|FORMAT_TRANSPARENT;

	size_t max_buf;
    char* imgbuf = (char*)skin_buffer_grab(&max_buf);
	bitmap->data = imgbuf;
    int ret = read_bmp_file(img_path, bitmap, max_buf, format, NULL);

    if (ret > 0)
    {
		skin_buffer_increment(ret, true);
        loaded = true;
    }
    else
	{
        /* Abort if we can't load an image */
        DEBUGF("ERR: Failed to load image - %s\n",img_path);
		loaded = false;
    }
	return loaded;
}

static bool load_skin_bitmaps(struct wps_data *wps_data, char *bmpdir)
{
    struct skin_token_list *list;
    /* do the progressbars */
    list = wps_data->progressbars;
    while (list)
    {
        struct progressbar *pb = (struct progressbar*)list->token->value.data;
        if (pb->bm.data)
        {
            pb->have_bitmap_pb = load_skin_bmp(wps_data, &pb->bm, bmpdir);
        }
		list = list->next;
    }
    /* regular images */
	list = wps_data->images;
    while (list)
    {
        struct gui_img *img = (struct gui_img*)list->token->value.data;
        if (img->bm.data)
        {
            img->loaded = load_skin_bmp(wps_data, &img->bm, bmpdir);
			if (img->loaded)
			    img->subimage_height = img->bm.height / img->num_subimages;
        }
		list = list->next;
    }

#if (LCD_DEPTH > 1) || (defined(HAVE_REMOTE_LCD) && (LCD_REMOTE_DEPTH > 1))
    if (bmp_names[BACKDROP_BMP])
    {
        int screen = SCREEN_MAIN;
        char img_path[MAX_PATH];
        get_image_filename(bmp_names[BACKDROP_BMP], bmpdir,
                            img_path, sizeof(img_path));
#if defined(HAVE_REMOTE_LCD)
        /* We only need to check LCD type if there is a remote LCD */
        if (wps_data->remote_wps)
            screen = SCREEN_REMOTE;
#endif
        screens[screen].backdrop_load(BACKDROP_SKIN_WPS, img_path);
    }
#endif /* has backdrop support */

    /* If we got here, everything was OK */
    return true;
}

#endif /* HAVE_LCD_BITMAP */

/* to setup up the wps-data from a format-buffer (isfile = false)
   from a (wps-)file (isfile = true)*/
bool skin_data_load(struct wps_data *wps_data,
                   struct screen *display,
                   const char *buf,
                   bool isfile)
{

    if (!wps_data || !buf)
        return false;
#ifdef HAVE_ALBUMART
    int status;
    struct mp3entry *curtrack;
    long offset;
    struct skin_albumart old_aa = {.state = WPS_ALBUMART_NONE};
    if (wps_data->albumart)
    {
        old_aa.state = wps_data->albumart->state;
        old_aa.height = wps_data->albumart->height;
        old_aa.width = wps_data->albumart->width;
    }
#endif

    skin_data_reset(wps_data);

    curr_vp = skin_buffer_alloc(sizeof(struct skin_viewport));
    if (!curr_vp)
        return false;
    struct skin_token_list *list = new_skin_token_list_item(NULL, curr_vp);
    if (!list)
        return false;
    add_to_ll_chain(&wps_data->viewports, list);


    /* Initialise the first (default) viewport */
    curr_vp->label         = VP_DEFAULT_LABEL;
    curr_vp->vp.x          = 0;
    curr_vp->vp.width      = display->getwidth();
    curr_vp->vp.height     = display->getheight();
    curr_vp->pb            = NULL;
    curr_vp->hidden_flags  = 0;
    curr_vp->lines         = NULL;

    curr_line = NULL;
    if (!skin_start_new_line(curr_vp, 0))
        return false;

    switch (statusbar_position(display->screen_type))
    {
        case STATUSBAR_OFF:
            curr_vp->vp.y      = 0;
            break;
        case STATUSBAR_TOP:
            curr_vp->vp.y       = STATUSBAR_HEIGHT;
            curr_vp->vp.height -= STATUSBAR_HEIGHT;
            break;
        case STATUSBAR_BOTTOM:
            curr_vp->vp.y       = 0;
            curr_vp->vp.height -= STATUSBAR_HEIGHT;
            break;
    }
#ifdef HAVE_LCD_BITMAP
    curr_vp->vp.font       = FONT_UI;
    curr_vp->vp.drawmode   = DRMODE_SOLID;
#endif
#if LCD_DEPTH > 1
    if (display->depth > 1)
    {
        curr_vp->vp.fg_pattern = display->get_foreground();
        curr_vp->vp.bg_pattern = display->get_background();
    }
#endif
    if (!isfile)
    {
        return wps_parse(wps_data, buf, false);
    }
    else
    {
        int fd = open_utf8(buf, O_RDONLY);

        if (fd < 0)
            return false;

        /* get buffer space from the plugin buffer */
        size_t buffersize = 0;
        char *wps_buffer = (char *)plugin_get_buffer(&buffersize);

        if (!wps_buffer)
            return false;

        /* copy the file's content to the buffer for parsing,
           ensuring that every line ends with a newline char. */
        unsigned int start = 0;
        while(read_line(fd, wps_buffer + start, buffersize - start) > 0)
        {
            start += strlen(wps_buffer + start);
            if (start < buffersize - 1)
            {
                wps_buffer[start++] = '\n';
                wps_buffer[start] = 0;
            }
        }

        close(fd);

        if (start <= 0)
            return false;

#ifdef HAVE_LCD_BITMAP
        /* Set all filename pointers to NULL */
        memset(bmp_names, 0, sizeof(bmp_names));
#endif

        /* parse the WPS source */
        if (!wps_parse(wps_data, wps_buffer, true)) {
            skin_data_reset(wps_data);
            return false;
        }

        wps_data->wps_loaded = true;

#ifdef HAVE_LCD_BITMAP
        /* get the bitmap dir */
        char bmpdir[MAX_PATH];
        char *dot = strrchr(buf, '.');

        strlcpy(bmpdir, buf, dot - buf + 1);

        /* load the bitmaps that were found by the parsing */
        if (!load_skin_bitmaps(wps_data, bmpdir)) {
            skin_data_reset(wps_data);
            return false;
        }
#endif
#ifdef HAVE_ALBUMART
        status = audio_status();
        if (status & AUDIO_STATUS_PLAY)
        {
            struct skin_albumart *aa = wps_data->albumart;
            if (aa && ((aa->state && !old_aa.state) ||
                (aa->state &&
                (((old_aa.height != aa->height) ||
                (old_aa.width != aa->width))))))
            {
                curtrack = audio_current_track();
                offset = curtrack->offset;
                audio_stop();
                if (!(status & AUDIO_STATUS_PAUSE))
                    audio_play(offset);
            }
        }
#endif
#if defined(DEBUG) || defined(SIMULATOR)
        debug_skin_usage();
#endif
        return true;
    }
}
