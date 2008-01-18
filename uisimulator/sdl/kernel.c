/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 by Felix Arends
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <stdlib.h>
#include <SDL.h>
#include <SDL_thread.h>
#include "memory.h"
#include "system-sdl.h"
#include "uisdl.h"
#include "kernel.h"
#include "thread-sdl.h"
#include "thread.h"
#include "debug.h"

/* Condition to signal that "interrupts" may proceed */
static SDL_cond *sim_thread_cond;
/* Mutex to serialize changing levels and exclude other threads while
 * inside a handler */
static SDL_mutex *sim_irq_mtx;
static int interrupt_level = HIGHEST_IRQ_LEVEL;
static int handlers_pending = 0;
static int status_reg = 0;

extern struct core_entry cores[NUM_CORES];

/* Nescessary logic:
 * 1) All threads must pass unblocked
 * 2) Current handler must always pass unblocked
 * 3) Threads must be excluded when irq routine is running
 * 4) No more than one handler routine should execute at a time
 */
int set_irq_level(int level)
{
    SDL_LockMutex(sim_irq_mtx);

    int oldlevel = interrupt_level;

    if (status_reg == 0 && level == 0 && oldlevel != 0)
    {
        /* Not in a handler and "interrupts" are being reenabled */
        if (handlers_pending > 0)
            SDL_CondSignal(sim_thread_cond);
    }

    interrupt_level = level; /* save new level */

    SDL_UnlockMutex(sim_irq_mtx);
    return oldlevel;
}

void sim_enter_irq_handler(void)
{
    SDL_LockMutex(sim_irq_mtx);
    handlers_pending++;

    if(interrupt_level != 0)
    {
        /* "Interrupts" are disabled. Wait for reenable */
        SDL_CondWait(sim_thread_cond, sim_irq_mtx);
    }

    status_reg = 1;
}

void sim_exit_irq_handler(void)
{
    if (--handlers_pending > 0)
        SDL_CondSignal(sim_thread_cond);

    status_reg = 0;
    SDL_UnlockMutex(sim_irq_mtx);
}

bool sim_kernel_init(void)
{
    sim_irq_mtx = SDL_CreateMutex();
    if (sim_irq_mtx == NULL)
    {
        fprintf(stderr, "Cannot create sim_handler_mtx\n");
        return false;
    }

    sim_thread_cond = SDL_CreateCond();
    if (sim_thread_cond == NULL)
    {
        fprintf(stderr, "Cannot create sim_thread_cond\n");
        return false;
    }

    return true;
}

void sim_kernel_shutdown(void)
{
    SDL_DestroyMutex(sim_irq_mtx);
    SDL_DestroyCond(sim_thread_cond);
}

volatile long current_tick = 0;
static void (*tick_funcs[MAX_NUM_TICK_TASKS])(void);

/* This array holds all queues that are initiated. It is used for broadcast. */
static struct event_queue *all_queues[MAX_NUM_QUEUES];
static int num_queues = 0;

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
/* Moves waiting thread's descriptor to the current sender when a
   message is dequeued */
static void queue_fetch_sender(struct queue_sender_list *send,
                               unsigned int i)
{
    struct thread_entry **spp = &send->senders[i];

    if(*spp)
    {
        send->curr_sender = *spp;
        *spp = NULL;
    }
}

/* Puts the specified return value in the waiting thread's return value
   and wakes the thread  - a sender should be confirmed to exist first */
static void queue_release_sender(struct thread_entry **sender,
                                 intptr_t retval)
{
    (*sender)->retval = retval;
    wakeup_thread_no_listlock(sender);
    if(*sender != NULL)
    {
        fprintf(stderr, "queue->send slot ovf: %p\n", *sender);
        exit(-1);
    }
}

/* Releases any waiting threads that are queued with queue_send -
   reply with NULL */
static void queue_release_all_senders(struct event_queue *q)
{
    if(q->send)
    {
        unsigned int i;
        for(i = q->read; i != q->write; i++)
        {
            struct thread_entry **spp =
                &q->send->senders[i & QUEUE_LENGTH_MASK];
            if(*spp)
            {
                queue_release_sender(spp, 0);
            }
        }
    }
}

/* Enables queue_send on the specified queue - caller allocates the extra
   data structure */
void queue_enable_queue_send(struct event_queue *q,
                             struct queue_sender_list *send)
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);
    q->send = NULL;
    if(send)
    {
        q->send = send;
        memset(send, 0, sizeof(*send));
    }
    set_irq_level(oldlevel);
}
#endif /* HAVE_EXTENDED_MESSAGING_AND_NAME */

void queue_init(struct event_queue *q, bool register_queue)
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

    q->read   = 0;
    q->write  = 0;
    thread_queue_init(&q->queue);
#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
    q->send   = NULL; /* No message sending by default */
#endif

    if(register_queue)
    {
        if(num_queues >= MAX_NUM_QUEUES)
        {
            fprintf(stderr, "queue_init->out of queues");
            exit(-1);
        }
        /* Add it to the all_queues array */
        all_queues[num_queues++] = q;
    }

    set_irq_level(oldlevel);
}

void queue_delete(struct event_queue *q)
{
    int i;
    bool found = false;

    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

    /* Find the queue to be deleted */
    for(i = 0;i < num_queues;i++)
    {
        if(all_queues[i] == q)
        {
            found = true;
            break;
        }
    }

    if(found)
    {
        /* Move the following queues up in the list */
        for(;i < num_queues-1;i++)
        {
            all_queues[i] = all_queues[i+1];
        }
        
        num_queues--;
    }

    /* Release threads waiting on queue head */
    thread_queue_wake(&q->queue);

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
    /* Release waiting threads and reply to any dequeued message
       waiting for one. */
    queue_release_all_senders(q);
    queue_reply(q, 0);
#endif

    q->read = 0;
    q->write = 0;

    set_irq_level(oldlevel);
}

void queue_wait(struct event_queue *q, struct queue_event *ev)
{
    unsigned int rd;
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
    if (q->send && q->send->curr_sender)
    {
        /* auto-reply */
        queue_release_sender(&q->send->curr_sender, 0);
    }
#endif

    if (q->read == q->write)
    {
        do
        {
            cores[CURRENT_CORE].irq_level = oldlevel;
            block_thread(&q->queue);
            oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);
        }
        while (q->read == q->write);
    }

    rd = q->read++ & QUEUE_LENGTH_MASK;
    *ev = q->events[rd];

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
    if(q->send && q->send->senders[rd])
    {
        /* Get data for a waiting thread if one */
        queue_fetch_sender(q->send, rd);
    }
#endif

    set_irq_level(oldlevel);
}

void queue_wait_w_tmo(struct event_queue *q, struct queue_event *ev, int ticks)
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
    if (q->send && q->send->curr_sender)
    {
        /* auto-reply */
        queue_release_sender(&q->send->curr_sender, 0);
    }
#endif

    if (q->read == q->write && ticks > 0)
    {
        cores[CURRENT_CORE].irq_level = oldlevel;
        block_thread_w_tmo(&q->queue, ticks);
        oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);
    }

    if(q->read != q->write)
    {
        unsigned int rd = q->read++ & QUEUE_LENGTH_MASK;
        *ev = q->events[rd];

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
        if(q->send && q->send->senders[rd])
        {
            /* Get data for a waiting thread if one */
            queue_fetch_sender(q->send, rd);
        }
#endif
    }
    else
    {
        ev->id = SYS_TIMEOUT;
    }

    set_irq_level(oldlevel);
}

void queue_post(struct event_queue *q, long id, intptr_t data)
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

    unsigned int wr = q->write++ & QUEUE_LENGTH_MASK;

    q->events[wr].id   = id;
    q->events[wr].data = data;

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
    if(q->send)
    {
        struct thread_entry **spp = &q->send->senders[wr];

        if(*spp)
        {
            /* overflow protect - unblock any thread waiting at this index */
            queue_release_sender(spp, 0);
        }
    }
#endif

    wakeup_thread(&q->queue);

    set_irq_level(oldlevel);
}

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
intptr_t queue_send(struct event_queue *q, long id, intptr_t data)
{
    int oldlevel = set_irq_level(oldlevel);

    unsigned int wr = q->write++ & QUEUE_LENGTH_MASK;

    q->events[wr].id   = id;
    q->events[wr].data = data;

    if(q->send)
    {
        struct thread_entry **spp = &q->send->senders[wr];

        if(*spp)
        {
            /* overflow protect - unblock any thread waiting at this index */
            queue_release_sender(spp, 0);
        }

        wakeup_thread(&q->queue);

        cores[CURRENT_CORE].irq_level = oldlevel;
        block_thread_no_listlock(spp);
        return thread_get_current()->retval;
    }

    /* Function as queue_post if sending is not enabled */
    wakeup_thread(&q->queue);
    set_irq_level(oldlevel);
    return 0;
}

#if 0 /* not used now but probably will be later */
/* Query if the last message dequeued was added by queue_send or not */
bool queue_in_queue_send(struct event_queue *q)
{
    return q->send && q->send->curr_sender;
}
#endif

/* Replies with retval to any dequeued message sent with queue_send */
void queue_reply(struct event_queue *q, intptr_t retval)
{
    if(q->send && q->send->curr_sender)
    {
        queue_release_sender(&q->send->curr_sender, retval);
    }
}
#endif /* HAVE_EXTENDED_MESSAGING_AND_NAME */

bool queue_empty(const struct event_queue* q)
{
    return ( q->read == q->write );
}

bool queue_peek(struct event_queue *q, struct queue_event *ev)
{
    if (q->read == q->write)
         return false;

    bool have_msg = false;

    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

    if (q->read != q->write)
    {
        *ev = q->events[q->read & QUEUE_LENGTH_MASK];
        have_msg = true;
    }

    set_irq_level(oldlevel);

    return have_msg;
}

void queue_clear(struct event_queue* q)
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

    /* fixme: This is potentially unsafe in case we do interrupt-like processing */
#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
    /* Release all thread waiting in the queue for a reply -
       dequeued sent message will be handled by owning thread */
    queue_release_all_senders(q);
#endif
    q->read = 0;
    q->write = 0;

    set_irq_level(oldlevel);
}

void queue_remove_from_head(struct event_queue *q, long id)
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

    while(q->read != q->write)
    {
        unsigned int rd = q->read & QUEUE_LENGTH_MASK;

        if(q->events[rd].id != id)
        {
            break;
        }

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
        if(q->send)
        {
            struct thread_entry **spp = &q->send->senders[rd];

            if(*spp)
            {
                /* Release any thread waiting on this message */
                queue_release_sender(spp, 0);
            }
        }
#endif
        q->read++;
    }

    set_irq_level(oldlevel);
}

int queue_count(const struct event_queue *q)
{
    return q->write - q->read;
}

int queue_broadcast(long id, intptr_t data)
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);
    int i;
    
    for(i = 0;i < num_queues;i++)
    {
        queue_post(all_queues[i], id, data);
    }

    set_irq_level(oldlevel);   
    return num_queues;
}

void yield(void)
{
    switch_thread(NULL);
}

void sleep(int ticks)
{
    sleep_thread(ticks);
}

void sim_tick_tasks(void)
{
    int i;

    /* Run through the list of tick tasks */
    for(i = 0;i < MAX_NUM_TICK_TASKS;i++)
    {
        if(tick_funcs[i])
        {
            tick_funcs[i]();
        }
    }
}

int tick_add_task(void (*f)(void))
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);
    int i;

    /* Add a task if there is room */
    for(i = 0;i < MAX_NUM_TICK_TASKS;i++)
    {
        if(tick_funcs[i] == NULL)
        {
            tick_funcs[i] = f;
            set_irq_level(oldlevel);
            return 0;
        }
    }
    fprintf(stderr, "Error! tick_add_task(): out of tasks");
    exit(-1);
    return -1;
}

int tick_remove_task(void (*f)(void))
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);
    int i;

    /* Remove a task if it is there */
    for(i = 0;i < MAX_NUM_TICK_TASKS;i++)
    {
        if(tick_funcs[i] == f)
        {
            tick_funcs[i] = NULL;
            set_irq_level(oldlevel);
            return 0;
        }
    }

    set_irq_level(oldlevel);
    return -1;
}

/* Very simple mutex simulation - won't work with pre-emptive
   multitasking, but is better than nothing at all */
void mutex_init(struct mutex *m)
{
    m->queue = NULL;
    m->thread = NULL;
    m->count = 0;
    m->locked = 0;
}

void mutex_lock(struct mutex *m)
{
    struct thread_entry *const thread = thread_get_current();

    if(thread == m->thread)
    {
        m->count++;
        return;
    }

    if (!test_and_set(&m->locked, 1))
    {
        m->thread = thread;
        return;
    }

    block_thread_no_listlock(&m->queue);
}

void mutex_unlock(struct mutex *m)
{
    /* unlocker not being the owner is an unlocking violation */
    if(m->thread != thread_get_current())
    {
        fprintf(stderr, "mutex_unlock->wrong thread");
        exit(-1);
    }    

    if (m->count > 0)
    {
        /* this thread still owns lock */
        m->count--;
        return;
    }

    m->thread = wakeup_thread_no_listlock(&m->queue);

    if (m->thread == NULL)
    {
        /* release lock */
        m->locked = 0;
    }
}

#ifdef HAVE_SEMAPHORE_OBJECTS
void semaphore_init(struct semaphore *s, int max, int start)
{
    if(max <= 0 || start < 0 || start > max)
    {
        fprintf(stderr, "semaphore_init->inv arg");
        exit(-1);
    }
    s->queue = NULL;
    s->max = max;
    s->count = start;
}

void semaphore_wait(struct semaphore *s)
{
    if(--s->count >= 0)
        return;
    block_thread_no_listlock(&s->queue);
}

void semaphore_release(struct semaphore *s)
{
    if(s->count < s->max)
    {
        if(++s->count <= 0)
        {
            if(s->queue == NULL)
            {
                /* there should be threads in this queue */
                fprintf(stderr, "semaphore->wakeup");
                exit(-1);
            }
            /* a thread was queued - wake it up */
            wakeup_thread_no_listlock(&s->queue);
        }
    }
}
#endif /* HAVE_SEMAPHORE_OBJECTS */

#ifdef HAVE_EVENT_OBJECTS
void event_init(struct event *e, unsigned int flags)
{
    e->queues[STATE_NONSIGNALED] = NULL;
    e->queues[STATE_SIGNALED] = NULL;
    e->state = flags & STATE_SIGNALED;
    e->automatic = (flags & EVENT_AUTOMATIC) ? 1 : 0;
}

void event_wait(struct event *e, unsigned int for_state)
{
    unsigned int last_state = e->state;

    if(e->automatic != 0)
    {
        /* wait for false always satisfied by definition
           or if it just changed to false */
        if(last_state == STATE_SIGNALED || for_state == STATE_NONSIGNALED)
        {
            /* automatic - unsignal */
            e->state = STATE_NONSIGNALED;
            return;
        }
        /* block until state matches */
    }
    else if(for_state == last_state)
    {
        /* the state being waited for is the current state */
        return;
    }

    /* current state does not match wait-for state */
    block_thread_no_listlock(&e->queues[for_state]);
}

void event_set_state(struct event *e, unsigned int state)
{
    unsigned int last_state = e->state;

    if(last_state == state)
    {
        /* no change */
        return;
    }

    if(state == STATE_SIGNALED)
    {
        if(e->automatic != 0)
        {
            struct thread_entry *thread;

            if(e->queues[STATE_NONSIGNALED] != NULL)
            {
                /* no thread should have ever blocked for nonsignaled */
                fprintf(stderr, "set_event_state->queue[NS]:S");
                exit(-1);
            }

            /* pass to next thread and keep unsignaled - "pulse" */
            thread = wakeup_thread_no_listlock(&e->queues[STATE_SIGNALED]);
            e->state = thread != NULL ? STATE_NONSIGNALED : STATE_SIGNALED;
        }
        else
        {
            /* release all threads waiting for signaled */
            thread_queue_wake_no_listlock(&e->queues[STATE_SIGNALED]);
            e->state = STATE_SIGNALED;
        }
    }
    else
    {
        /* release all threads waiting for unsignaled */
        if(e->queues[STATE_NONSIGNALED] != NULL && e->automatic != 0)
        {
            /* no thread should have ever blocked */
            fprintf(stderr, "set_event_state->queue[NS]:NS");
            exit(-1);
        }

        thread_queue_wake_no_listlock(&e->queues[STATE_NONSIGNALED]);
        e->state = STATE_NONSIGNALED;
    }
}
#endif /* HAVE_EVENT_OBJECTS */
