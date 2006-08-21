/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2006 Jonathan Gordon
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

#include "config.h"
#include "lang.h"

#include "button.h"
#include "action.h"
#include "kernel.h"
#include "debug.h"
#include "splash.h"

bool ignore_until_release = false;
int last_button = BUTTON_NONE;

/* software keylock stuff */
#ifndef HAS_BUTTON_HOLD
bool keys_locked = false;
int unlock_combo = BUTTON_NONE;
bool screen_has_lock = false;
#endif /* HAVE_SOFTWARE_KEYLOCK */

/*
 * do_button_check is the worker function for get_default_action.
 * returns ACTION_UNKNOWN or the requested return value from the list.
 */
inline int do_button_check(const struct button_mapping *items, 
                           int button, int last_button, int *start)
{
    int i = 0;
    int ret = ACTION_UNKNOWN;
    if (items == NULL)
        return ACTION_UNKNOWN;

    while (items[i].button_code != BUTTON_NONE)
    {
        if (items[i].button_code == button) 
        {
            if ((items[i].pre_button_code == BUTTON_NONE)
                || (items[i].pre_button_code == last_button))
            {
                ret = items[i].action_code;
                break;
            }
        }
        i++;
    }
    *start = i;
    return ret;
}

inline int get_next_context(const struct button_mapping *items, int i)
{
    while (items[i].button_code != BUTTON_NONE)
        i++;
    return (items[i].action_code == ACTION_NONE ) ? 
            CONTEXT_STD : 
            items[i].action_code;
}
/*
 * int get_action_worker(int context, struct button_mapping *user_mappings,
   int timeout)
   This function searches the button list for the given context for the just
   pressed button.
   If there is a match it returns the value from the list.
   If there is no match..
        the last item in the list "points" to the next context in a chain
        so the "chain" is followed until the button is found.
        putting ACTION_NONE will get CONTEXT_STD which is always the last list checked.
        
   Timeout can be TIMEOUT_NOBLOCK to return immediatly
                  TIMEOUT_BLOCK   to wait for a button press
                  Any number >0   to wait that many ticks for a press
                  
 */
int get_action_worker(int context, int timeout,
                      const struct button_mapping* (*get_context_map)(int) )
{
    const struct button_mapping *items = NULL;
    int button;
    int i=0;
    int ret = ACTION_UNKNOWN;
    
    if (timeout == TIMEOUT_NOBLOCK)
        button = button_get(false);
    else if  (timeout == TIMEOUT_BLOCK)
        button = button_get(true);
    else
        button = button_get_w_tmo(timeout);

    
    if (button == BUTTON_NONE || button&SYS_EVENT)
    {
        return button;
    }
    
    if (ignore_until_release == true)
    {
        if (button&BUTTON_REL)
        {
            ignore_until_release = false;
        }
        return ACTION_NONE; /* "safest" return value */
    }
    
#ifndef HAS_BUTTON_HOLD
    screen_has_lock = ((context&ALLOW_SOFTLOCK)==ALLOW_SOFTLOCK);
    if (screen_has_lock && (keys_locked == true))
    {
        if (button == unlock_combo) 
        {
            last_button = BUTTON_NONE;
            keys_locked = false;
            gui_syncsplash(HZ/2, true, str(LANG_KEYLOCK_OFF_PLAYER));
            return ACTION_REDRAW;
        } 
        else 
#if (BUTTON_REMOTE != 0)   
            if ((button&BUTTON_REMOTE) == 0) 
#endif
        {
            if ((button&BUTTON_REL))
                gui_syncsplash(HZ/2, true, str(LANG_KEYLOCK_ON_PLAYER));
            return ACTION_REDRAW;
        }
    }
    context &= ~ALLOW_SOFTLOCK;
#endif /* HAS_BUTTON_HOLD */
    
    /*   logf("%x,%x",last_button,button); */
    do 
    {
        /*     logf("context = %x",context); */
#if (BUTTON_REMOTE != 0)
        if (button&BUTTON_REMOTE)
            context |= CONTEXT_REMOTE;
#endif
        if ((context&CONTEXT_CUSTOM) && get_context_map)
            items = get_context_map(context);
        else
            items = get_context_mapping(context);
        
        ret = do_button_check(items,button,last_button,&i);
        
        if (context == CONTEXT_STOPSEARCHING)
            break;
        
        if (ret == ACTION_UNKNOWN )
        {
            context = get_next_context(items,i);
            i = 0;
        }
        else break;
    } while (1);
    /* DEBUGF("ret = %x\n",ret); */
#ifndef HAS_BUTTON_HOLD
    if (screen_has_lock && (ret == ACTION_STD_KEYLOCK))
    {       
        unlock_combo = button;
        keys_locked = true;
        action_signalscreenchange();
        gui_syncsplash(HZ/2, true, str(LANG_KEYLOCK_ON_PLAYER));
        
        button_clear_queue();
        return ACTION_REDRAW;
    }
#endif
    last_button = button;
    return ret;
}

int get_action(int context, int timeout)
{
    return get_action_worker(context,timeout,NULL);
}

int get_custom_action(int context,int timeout,
                      const struct button_mapping* (*get_context_map)(int))
{
    return get_action_worker(context,timeout,get_context_map);
}

bool action_userabort(int timeout)
{
    action_signalscreenchange();
    int action = get_action_worker(CONTEXT_STD,timeout,NULL);
    bool ret = (action == ACTION_STD_CANCEL);
    action_signalscreenchange();
    return ret;
}

void action_signalscreenchange(void)
{
    if ((last_button != BUTTON_NONE) && 
         !(last_button&BUTTON_REL))
    {
        ignore_until_release = true;
    }
    last_button = BUTTON_NONE;
}
#ifndef HAS_BUTTON_HOLD
bool is_keys_locked(void)
{
    return (screen_has_lock && (keys_locked == true));
}
#endif
