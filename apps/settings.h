/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 by wavey@wavey.org
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#ifndef __SETTINGS_H__
#define __SETTINGS_H__

#include <stdbool.h>

/* data structures */

#define RESUME_NONE     0
#define RESUME_SONG     1 /* resume song at startup     */
#define RESUME_PLAYLIST 2 /* resume playlist at startup */

struct user_settings
{
    /* audio settings */

    int volume;     /* audio output volume:  0-100 0=off   100=max           */
    int balance;    /* stereo balance:       0-100 0=left  50=bal 100=right  */
    int bass;       /* bass eq:              0-100 0=off   100=max           */
    int treble;     /* treble eq:            0-100 0=low   100=high          */
    int loudness;   /* loudness eq:          0-100 0=off   100=max           */
    int bass_boost; /* bass boost eq:        0-100 0=off   100=max           */

    /* device settings */

    int contrast;   /* lcd contrast:         0-100 0=low 100=high            */
    int poweroff;   /* power off timer:      0-100 0=never:each 1% = 60 secs */
    int backlight;  /* backlight off timer:  0-100 0=never:each 1% = 10 secs */

    /* resume settings */

    int resume;     /* power-on song resume: 0=no. 1=yes song. 2=yes pl   */
    int track_time; /* number of seconds into the track to resume         */

    /* misc options */

    int loop_playlist; /* do we return to top of playlist at end?            */
    bool mp3filter;
    int scroll_speed;
    bool playlist_shuffle;

    /* while playing screen settings  */
    int wps_display;

};

/* prototypes */

int persist_all_settings( void );
void reload_all_settings( struct user_settings *settings );
void reset_settings( struct user_settings *settings );
void display_current_settings( struct user_settings *settings );

void set_bool(char* string, bool* variable );
void set_option(char* string, int* variable, char* options[], int numoptions );
void set_int(char* string, 
             char* unit,
             int* variable,
             void (*function)(int),
             int step,
             int min,
             int max );

/* global settings */
extern struct user_settings global_settings;

/* system defines */

#define DEFAULT_VOLUME_SETTING     70/2
#define DEFAULT_BALANCE_SETTING    50
#define DEFAULT_BASS_SETTING       50/2
#define DEFAULT_TREBLE_SETTING     50/2
#define DEFAULT_LOUDNESS_SETTING    0
#define DEFAULT_BASS_BOOST_SETTING  0
#define DEFAULT_CONTRAST_SETTING    0
#define DEFAULT_POWEROFF_SETTING    0
#define DEFAULT_BACKLIGHT_SETTING   5
#define DEFAULT_WPS_DISPLAY         0 

#endif /* __SETTINGS_H__ */
