/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 Jerome Kuptz
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "file.h"
#include "lcd.h"
#include "button.h"
#include "kernel.h"
#include "tree.h"
#include "debug.h"
#include "sprintf.h"
#include "settings.h"
#include "wps.h"
#include "mpeg.h"


#define LINE_Y      1 /* initial line */

#define PLAY_DISPLAY_DEFAULT         0 
#define PLAY_DISPLAY_FILENAME_SCROLL 1 
#define PLAY_DISPLAY_TRACK_TITLE     2 

static void draw_screen(struct mp3entry* id3)
{
    lcd_clear_display();
    switch ( global_settings.wps_display ) {
        case PLAY_DISPLAY_TRACK_TITLE:
        {
            char ch = '/';
            char* end;
            char* szTok;
            char* szDelimit;
            char* szPeriod;
            char szArtist[26];
            char szBuff[257];
            szBuff[sizeof(szBuff)-1] = 0;

            strncpy(szBuff, id3->path, sizeof(szBuff));

            szTok = strtok_r(szBuff, "/", &end);
            szTok = strtok_r(NULL, "/", &end);

            // Assume path format of: Genre/Artist/Album/Mp3_file
            strncpy(szArtist,szTok,sizeof(szArtist));
            szArtist[sizeof(szArtist)-1] = 0;
            szDelimit = strrchr(id3->path, ch);
            lcd_puts(0,0, szArtist?szArtist:"<nothing>");

            // removes the .mp3 from the end of the display buffer
            szPeriod = strrchr(szDelimit, '.');
            if (szPeriod != NULL)
                *szPeriod = 0;

            lcd_puts_scroll(0,LINE_Y,(++szDelimit));
            break;
        }
        case PLAY_DISPLAY_FILENAME_SCROLL:
        {
            char ch = '/';
            char* szLast = strrchr(id3->path, ch);

            if (szLast)
                lcd_puts_scroll(0,0, (++szLast));
            else
                lcd_puts_scroll(0,0, id3->path);

            break;
        }
        case PLAY_DISPLAY_DEFAULT:
        {
            int l = 0;
#ifdef HAVE_LCD_BITMAP
            char buffer[64];

            lcd_puts_scroll(0, l++, id3->path);
            lcd_puts(0, l++, id3->title?id3->title:"");
            lcd_puts(0, l++, id3->album?id3->album:"");
            lcd_puts(0, l++, id3->artist?id3->artist:"");

            snprintf(buffer,sizeof(buffer), "Time: %d:%d",
                     id3->length / 60000,
                     id3->length % 60000 / 1000 );
            lcd_puts(0, l++, buffer);

            snprintf(buffer,sizeof(buffer), "%d kbits", id3->bitrate);

            lcd_puts(0, l++, buffer);

            snprintf(buffer,sizeof(buffer), "%d Hz", id3->frequency);
            lcd_puts(0, l++, buffer);
#else

            lcd_puts(0, l++, id3->artist?id3->artist:"<no artist>");
            lcd_puts(0, l++, id3->title?id3->title:"<no title>");
#endif
            break;
        }
    }
    lcd_update();
}

/* demonstrates showing different formats from playtune */
void wps_show(void)
{
    static bool playing = true;
    struct mp3entry* id3 = mpeg_current_track();
    int lastlength=0, lastsize=0, lastrate=0;

    while ( 1 ) {
        int i;

        if ( ( id3->length != lastlength ) ||
             ( id3->filesize != lastsize ) ||
             ( id3->bitrate != lastrate ) ) {
            draw_screen(id3);
            lastlength = id3->length;
            lastsize = id3->filesize;
            lastrate = id3->bitrate;
        }

        for ( i=0;i<20;i++ ) {
            switch ( button_get(false) ) {
                case BUTTON_ON:
                    return;

#ifdef HAVE_RECORDER_KEYPAD
                case BUTTON_PLAY:
#else
                case BUTTON_UP:
#endif
                    if ( playing )
                        mpeg_pause();
                    else
                        mpeg_resume();

                    playing = !playing;
                    break;

#ifdef HAVE_RECORDER_KEYPAD
                case BUTTON_UP:
                    global_settings.volume += 2;
                    if(global_settings.volume > 100)
                        global_settings.volume = 100;
                    mpeg_volume(global_settings.volume);
                    break;

                case BUTTON_DOWN:
                    global_settings.volume -= 2;
                    if(global_settings.volume < 0)
                        global_settings.volume = 0;
                    mpeg_volume(global_settings.volume);
                    break;
#endif

                case BUTTON_LEFT:
                    mpeg_prev();
                    break;

                case BUTTON_RIGHT:
                    mpeg_next();
                    break;

#ifdef HAVE_RECORDER_KEYPAD                
                case BUTTON_OFF:
#else
                case BUTTON_DOWN:
#endif
                    mpeg_stop();
                    break;
            }
            sleep(HZ/20);
        }
    }
}
