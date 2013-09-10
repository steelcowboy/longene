/*
 * Server-side thread management
 *
 * Copyright (C) 1998 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#ifdef HAVE_SCHED_H
#include <sched.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "file.h"
#include "handle.h"
#include "process.h"
#include "thread.h"
#include "request.h"
#include "user.h"
#include "security.h"
#include "log.h"


#ifdef __i386__
static const unsigned int supported_cpus = CPU_FLAG(CPU_x86);
#elif defined(__x86_64__)
static const unsigned int supported_cpus = CPU_FLAG(CPU_x86_64) | CPU_FLAG(CPU_x86);
#elif defined(__powerpc__)
static const unsigned int supported_cpus = CPU_FLAG(CPU_POWERPC);
#elif defined(__arm__)
static const unsigned int supported_cpus = CPU_FLAG(CPU_ARM);
#elif defined(__aarch64__)
static const unsigned int supported_cpus = CPU_FLAG(CPU_ARM64);
#else
#error Unsupported CPU
#endif

#ifdef CONFIG_UNIFIED_KERNEL

#include <linux/spinlock.h>
#include <linux/rwlock.h>
#include <linux/sched.h>
#include <linux/completion.h>

/* for find_thread_by_pid() */
static DEFINE_RWLOCK(thread_hash_lock);

#define THREAD_HASH_BITS 8
#define THREAD_HASH_SIZE (1<<THREAD_HASH_BITS)
#define thread_hashfn(nr) \
	((nr) % THREAD_HASH_SIZE)

static struct hlist_head thread_hash_table[THREAD_HASH_SIZE];

void init_thread_hash_table(void)
{
	int i=0;

	for (i=0; i<THREAD_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&thread_hash_table[i]);
}

void add_thread_by_pid(struct thread *thread, pid_t pid)
{
	int slot;

	if (thread==NULL) return;

	slot = thread_hashfn(pid);
	thread->pid = pid;

	write_lock(&thread_hash_lock);
	hlist_add_head(&thread->hash_entry, &thread_hash_table[slot]);
	write_unlock(&thread_hash_lock);
}

static void remove_thread_by_pid(struct thread *thread, pid_t pid)
{
	int slot;
	struct hlist_node *pos;
	struct thread *tmp;

	if (thread==NULL) return;

	if (thread->pid != pid)
	{
		printk("%s %d : pid is bad \n",__func__,__LINE__);
		return;
	}

	slot = thread_hashfn(pid);

	write_lock(&thread_hash_lock);
	hlist_for_each_entry(tmp, pos, &thread_hash_table[slot], hash_entry)
	{
		if ( tmp && tmp==thread )
		{
			tmp->pid = -1;
			hlist_del(&tmp->hash_entry);
		}
	}
	write_unlock(&thread_hash_lock);
}

struct thread* find_thread_by_pid(pid_t pid)
{
	struct hlist_node *pos;
	struct thread *thread = NULL;
	int slot = thread_hashfn(pid);

	read_lock(&thread_hash_lock);
	hlist_for_each_entry(thread, pos, &thread_hash_table[slot], hash_entry)
	{
		if ( thread && thread->pid == pid)
		{
			read_unlock(&thread_hash_lock);
			return thread;
		}
	}
	read_unlock(&thread_hash_lock);

	return NULL;
}

struct thread* get_thread_by_task(struct task_struct *task)
{
	if (task && task->pid)
	{
		return find_thread_by_pid(task->pid);
	}
	else
	{
		return NULL;
	}
}

struct thread* get_current_thread(void)
{
	return get_thread_by_task(current);
}
#endif

/* thread queues */

struct thread_wait
{
    struct thread_wait     *next;       /* next wait structure for this thread */
    struct thread          *thread;     /* owner thread */
    int                     count;      /* count of objects */
    int                     flags;
    client_ptr_t            cookie;     /* magic cookie to return to client */
    timeout_t               timeout;
    struct timeout_user    *user;
    struct wait_queue_entry queues[1];
};

/* asynchronous procedure calls */

struct thread_apc
{
    struct object       obj;      /* object header */
    struct list_head         entry;    /* queue linked list */
    struct thread      *caller;   /* thread that queued this apc */
    struct object      *owner;    /* object that queued this apc */
    int                 executed; /* has it been executed by the client? */
    apc_call_t          call;     /* call arguments */
    apc_result_t        result;   /* call results once executed */
};

static void dump_thread_apc( struct object *obj, int verbose );
static int thread_apc_signaled( struct object *obj, struct thread *thread );
static void thread_apc_destroy( struct object *obj );
static void clear_apc_queue( struct list_head *queue );

static const struct object_ops thread_apc_ops =
{
    sizeof(struct thread_apc),  /* size */
    dump_thread_apc,            /* dump */
    no_get_type,                /* get_type */
    add_queue,                  /* add_queue */
    remove_queue,               /* remove_queue */
    thread_apc_signaled,        /* signaled */
    no_satisfied,               /* satisfied */
    no_signal,                  /* signal */
    no_get_fd,                  /* get_fd */
    no_map_access,              /* map_access */
    default_get_sd,             /* get_sd */
    default_set_sd,             /* set_sd */
    no_lookup_name,             /* lookup_name */
    no_open_file,               /* open_file */
    no_close_handle,            /* close_handle */
    thread_apc_destroy          /* destroy */
};


/* thread operations */

static void dump_thread( struct object *obj, int verbose );
static int thread_signaled( struct object *obj, struct thread *thread );
static unsigned int thread_map_access( struct object *obj, unsigned int access );
static void thread_poll_event( struct fd *fd, int event );
static void destroy_thread( struct object *obj );

static const struct object_ops thread_ops =
{
    sizeof(struct thread),      /* size */
    dump_thread,                /* dump */
    no_get_type,                /* get_type */
    add_queue,                  /* add_queue */
    remove_queue,               /* remove_queue */
    thread_signaled,            /* signaled */
    no_satisfied,               /* satisfied */
    no_signal,                  /* signal */
    no_get_fd,                  /* get_fd */
    thread_map_access,          /* map_access */
    default_get_sd,             /* get_sd */
    default_set_sd,             /* set_sd */
    no_lookup_name,             /* lookup_name */
    no_open_file,               /* open_file */
    no_close_handle,            /* close_handle */
    destroy_thread              /* destroy */
};

static const struct fd_ops thread_fd_ops =
{
    NULL,                       /* get_poll_events */
    thread_poll_event,          /* poll_event */
    NULL,                       /* flush */
    NULL,                       /* get_fd_type */
    NULL,                       /* ioctl */
    NULL,                       /* queue_async */
    NULL,                       /* reselect_async */
    NULL                        /* cancel_async */
};

static struct list_head thread_list = LIST_INIT(thread_list);

/* initialize the structure for a newly allocated thread */
static inline void init_thread_structure( struct thread *thread )
{
    int i;

    thread->unix_pid        = -1;  /* not known yet */
    thread->unix_tid        = -1;  /* not known yet */
    thread->context         = NULL;
    thread->suspend_context = NULL;
    thread->teb             = 0;
    thread->debug_ctx       = NULL;
    thread->debug_event     = NULL;
    thread->debug_break     = 0;
    thread->queue           = NULL;
    thread->wait            = NULL;
    thread->error           = 0;
    thread->req_data        = NULL;
    thread->req_toread      = 0;
    thread->reply_data      = NULL;
    thread->reply_towrite   = 0;
    thread->request_fd      = NULL;
    thread->reply_fd        = NULL;
    thread->wait_fd         = NULL;
    thread->state           = RUNNING;
    thread->exit_code       = 0;
    thread->priority        = 0;
    thread->suspend         = 0;
    thread->desktop_users   = 0;
    thread->token           = NULL;
#ifdef CONFIG_UNIFIED_KERNEL
    thread->pid             = -1;  /* not known yet */
    INIT_HLIST_NODE( &thread->hash_entry );
    thread->unix_errno      = 0;
    init_completion( &thread->completion );
    memset( &thread->wake_info, 0, sizeof(thread->wake_info));
#endif

    thread->creation_time = current_time;
    thread->exit_time     = 0;

    list_init( &thread->mutex_list );
    list_init( &thread->system_apc );
    list_init( &thread->user_apc );

    for (i = 0; i < MAX_INFLIGHT_FDS; i++)
        thread->inflight[i].server = thread->inflight[i].client = -1;
}

/* check if address looks valid for a client-side data structure (TEB etc.) */
static inline int is_valid_address( client_ptr_t addr )
{
    return addr && !(addr % sizeof(int));
}

/* create a new thread */
struct thread *create_thread( int fd, struct process *process )
{
    struct thread *thread;

    if (process->is_terminating)
    {
        set_error( STATUS_PROCESS_IS_TERMINATING );
        return NULL;
    }

    if (!(thread = alloc_object( &thread_ops ))) return NULL;

    init_thread_structure( thread );

    thread->process = (struct process *)grab_object( process );
    thread->desktop = process->desktop;
    thread->affinity = process->affinity;
#ifdef CONFIG_UNIFIED_KERNEL
#else
    if (!current_thread) current_thread = thread;
#endif

    wine_list_add_head( &thread_list, &thread->entry );

    if (!(thread->id = alloc_ptid( thread )))
    {
        release_object( thread );
        return NULL;
    }
#ifdef CONFIG_UNIFIED_KERNEL
    thread->request_fd = NULL;
#else
    if (!(thread->request_fd = create_anonymous_fd( &thread_fd_ops, fd, &thread->obj, 0 )))
    {
        release_object( thread );
        return NULL;
    }

    set_fd_events( thread->request_fd, POLLIN );  /* start listening to events */
#endif
    add_process_thread( thread->process, thread );
    return thread;
}

/* handle a client event */
static void thread_poll_event( struct fd *fd, int event )
{
    struct thread *thread = get_fd_user( fd );
    assert( thread->obj.ops == &thread_ops );

    grab_object( thread );
    if (event & (POLLERR | POLLHUP)) kill_thread( thread, 0 );
    else if (event & POLLIN) read_request( thread );
    else if (event & POLLOUT) write_reply( thread );
    release_object( thread );
}

/* cleanup everything that is no longer needed by a dead thread */
/* used by destroy_thread and kill_thread */
static void cleanup_thread( struct thread *thread )
{
    int i;

    clear_apc_queue( &thread->system_apc );
    clear_apc_queue( &thread->user_apc );
    free( thread->req_data );
    free( thread->reply_data );
    if (thread->request_fd) release_object( thread->request_fd );
    if (thread->reply_fd) release_object( thread->reply_fd );
    if (thread->wait_fd) release_object( thread->wait_fd );
    free( thread->suspend_context );
    cleanup_clipboard_thread(thread);
    destroy_thread_windows( thread );
    free_msg_queue( thread );
    close_thread_desktop( thread );
    for (i = 0; i < MAX_INFLIGHT_FDS; i++)
    {
        if (thread->inflight[i].client != -1)
        {
            close( thread->inflight[i].server );
            thread->inflight[i].client = thread->inflight[i].server = -1;
        }
    }
    thread->req_data = NULL;
    thread->reply_data = NULL;
    thread->request_fd = NULL;
    thread->reply_fd = NULL;
    thread->wait_fd = NULL;
    thread->context = NULL;
    thread->suspend_context = NULL;
    thread->desktop = 0;
}

/* destroy a thread when its refcount is 0 */
static void destroy_thread( struct object *obj )
{
    struct thread *thread = (struct thread *)obj;
    assert( obj->ops == &thread_ops );

    assert( !thread->debug_ctx );  /* cannot still be debugging something */
    list_remove( &thread->entry );
#ifdef CONFIG_UNIFIED_KERNEL
    remove_thread_by_pid( thread, current->pid );
#endif
    cleanup_thread( thread );
    release_object( thread->process );
    if (thread->id) free_ptid( thread->id );
    if (thread->token) release_object( thread->token );
}

/* dump a thread on stdout for debugging purposes */
static void dump_thread( struct object *obj, int verbose )
{
    struct thread *thread = (struct thread *)obj;
    assert( obj->ops == &thread_ops );

    fprintf( stderr, "Thread id=%04x unix pid=%d unix tid=%d state=%d\n",
             thread->id, thread->unix_pid, thread->unix_tid, thread->state );
}

static int thread_signaled( struct object *obj, struct thread *thread )
{
    struct thread *mythread = (struct thread *)obj;
    return (mythread->state == TERMINATED);
}

static unsigned int thread_map_access( struct object *obj, unsigned int access )
{
    if (access & GENERIC_READ)    access |= STANDARD_RIGHTS_READ | SYNCHRONIZE;
    if (access & GENERIC_WRITE)   access |= STANDARD_RIGHTS_WRITE | SYNCHRONIZE;
    if (access & GENERIC_EXECUTE) access |= STANDARD_RIGHTS_EXECUTE;
    if (access & GENERIC_ALL)     access |= THREAD_ALL_ACCESS;
    return access & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL);
}

static void dump_thread_apc( struct object *obj, int verbose )
{
    struct thread_apc *apc = (struct thread_apc *)obj;
    assert( obj->ops == &thread_apc_ops );

    fprintf( stderr, "APC owner=%p type=%u\n", apc->owner, apc->call.type );
}

static int thread_apc_signaled( struct object *obj, struct thread *thread )
{
    struct thread_apc *apc = (struct thread_apc *)obj;
    return apc->executed;
}

static void thread_apc_destroy( struct object *obj )
{
    struct thread_apc *apc = (struct thread_apc *)obj;
    if (apc->caller) release_object( apc->caller );
    if (apc->owner) release_object( apc->owner );
}

/* queue an async procedure call */
static struct thread_apc *create_apc( struct object *owner, const apc_call_t *call_data )
{
    struct thread_apc *apc;

    if ((apc = alloc_object( &thread_apc_ops )))
    {
        apc->call        = *call_data;
        apc->caller      = NULL;
        apc->owner       = owner;
        apc->executed    = 0;
        apc->result.type = APC_NONE;
        if (owner) grab_object( owner );
    }
    return apc;
}

/* get a thread pointer from a thread id (and increment the refcount) */
struct thread *get_thread_from_id( thread_id_t id )
{
    struct object *obj = get_ptid_entry( id );

    if (obj && obj->ops == &thread_ops) return (struct thread *)grab_object( obj );
    set_error( STATUS_INVALID_CID );
    return NULL;
}

/* get a thread from a handle (and increment the refcount) */
struct thread *get_thread_from_handle( obj_handle_t handle, unsigned int access )
{
    return (struct thread *)get_handle_obj( current_thread->process, handle,
                                            access, &thread_ops );
}

/* find a thread from a Unix tid */
struct thread *get_thread_from_tid( int tid )
{
    struct thread *thread;

    LIST_FOR_EACH_ENTRY( thread, &thread_list, struct thread, entry )
    {
        if (thread->unix_tid == tid) return thread;
    }
    return NULL;
}

#ifdef CONFIG_UNIFIED_KERNEL
static int uk_sched_setaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask)
{
	return ENOSYS;
}

static int uk_sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask)
{
	return ENOSYS;
}
#endif

/* find a thread from a Unix pid */
struct thread *get_thread_from_pid( int pid )
{
    struct thread *thread;

    LIST_FOR_EACH_ENTRY( thread, &thread_list, struct thread, entry )
    {
        if (thread->unix_pid == pid) return thread;
    }
    return NULL;
}

int set_thread_affinity( struct thread *thread, affinity_t affinity )
{
    int ret = 0;
#ifdef HAVE_SCHED_SETAFFINITY
    if (thread->unix_tid != -1)
    {
        cpu_set_t set;
        int i;
        affinity_t mask;

        CPU_ZERO( &set );
        for (i = 0, mask = 1; mask; i++, mask <<= 1)
            if (affinity & mask) CPU_SET( i, &set );

#ifdef CONFIG_UNIFIED_KERNEL
	ret = uk_sched_setaffinity( thread->unix_tid, sizeof(set), &set );
#else
	ret = sched_setaffinity( thread->unix_tid, sizeof(set), &set );
#endif
    }
#endif
    if (!ret) thread->affinity = affinity;
    return ret;
}

affinity_t get_thread_affinity( struct thread *thread )
{
    affinity_t mask = 0;
#ifdef HAVE_SCHED_SETAFFINITY
    if (thread->unix_tid != -1)
    {
        cpu_set_t set;
        unsigned int i;

#ifdef CONFIG_UNIFIED_KERNEL
        if (!uk_sched_getaffinity( thread->unix_tid, sizeof(set), &set ))
#else
        if (!sched_getaffinity( thread->unix_tid, sizeof(set), &set ))
#endif
            for (i = 0; i < 8 * sizeof(mask); i++)
                if (CPU_ISSET( i, &set )) mask |= 1 << i;
    }
#endif
    if (!mask) mask = ~0;
    return mask;
}

#define THREAD_PRIORITY_REALTIME_HIGHEST 6
#define THREAD_PRIORITY_REALTIME_LOWEST -7

/* set all information about a thread */
static void set_thread_info( struct thread *thread,
                             const struct set_thread_info_request *req )
{
    if (req->mask & SET_THREAD_INFO_PRIORITY)
    {
        int max = THREAD_PRIORITY_HIGHEST;
        int min = THREAD_PRIORITY_LOWEST;
        if (thread->process->priority == PROCESS_PRIOCLASS_REALTIME)
        {
            max = THREAD_PRIORITY_REALTIME_HIGHEST;
            min = THREAD_PRIORITY_REALTIME_LOWEST;
        }
        if ((req->priority >= min && req->priority <= max) ||
            req->priority == THREAD_PRIORITY_IDLE ||
            req->priority == THREAD_PRIORITY_TIME_CRITICAL)
            thread->priority = req->priority;
        else
            set_error( STATUS_INVALID_PARAMETER );
    }
    if (req->mask & SET_THREAD_INFO_AFFINITY)
    {
        if ((req->affinity & thread->process->affinity) != req->affinity)
            set_error( STATUS_INVALID_PARAMETER );
        else if (thread->state == TERMINATED)
            set_error( STATUS_THREAD_IS_TERMINATING );
        else if (set_thread_affinity( thread, req->affinity ))
            file_set_error();
    }
    if (req->mask & SET_THREAD_INFO_TOKEN)
        security_set_thread_token( thread, req->token );
}

/* stop a thread (at the Unix level) */
void stop_thread( struct thread *thread )
{
    if (thread->context) return;  /* already inside a debug event, no need for a signal */
    /* can't stop a thread while initialisation is in progress */
    if (is_process_init_done(thread->process)) send_thread_signal( thread, SIGUSR1 );
}

/* stop a thread if it's supposed to be suspended */
void stop_thread_if_suspended( struct thread *thread )
{
    if (thread->suspend + thread->process->suspend > 0) stop_thread( thread );
}

/* suspend a thread */
static int suspend_thread( struct thread *thread )
{
    int old_count = thread->suspend;
    if (thread->suspend < MAXIMUM_SUSPEND_COUNT)
    {
        if (!(thread->process->suspend + thread->suspend++)) stop_thread( thread );
    }
    else set_error( STATUS_SUSPEND_COUNT_EXCEEDED );
    return old_count;
}

/* resume a thread */
static int resume_thread( struct thread *thread )
{
    int old_count = thread->suspend;
    if (thread->suspend > 0)
    {
        if (!(--thread->suspend + thread->process->suspend)) wake_thread( thread );
    }
    return old_count;
}

/* add a thread to an object wait queue; return 1 if OK, 0 on error */
int add_queue( struct object *obj, struct wait_queue_entry *entry )
{
    grab_object( obj );
    entry->obj = obj;
    wine_list_add_tail( &obj->wait_queue, &entry->entry );
    return 1;
}

/* remove a thread from an object wait queue */
void remove_queue( struct object *obj, struct wait_queue_entry *entry )
{
    list_remove( &entry->entry );
    release_object( obj );
}

/* finish waiting */
static void end_wait( struct thread *thread )
{
    struct thread_wait *wait = thread->wait;
    struct wait_queue_entry *entry;
    int i;

    assert( wait );
    thread->wait = wait->next;
    for (i = 0, entry = wait->queues; i < wait->count; i++, entry++)
        entry->obj->ops->remove_queue( entry->obj, entry );
    if (wait->user) remove_timeout_user( wait->user );
    free( wait );
}

/* build the thread wait structure */
static int wait_on( unsigned int count, struct object *objects[], int flags, timeout_t timeout )
{
    struct thread_wait *wait;
    struct wait_queue_entry *entry;
    unsigned int i;

    if (!(wait = mem_alloc( FIELD_OFFSET(struct thread_wait, queues[count]) ))) return 0;
    wait->next    = current_thread->wait;
    wait->thread  = current_thread;
    wait->count   = count;
    wait->flags   = flags;
    wait->user    = NULL;
    wait->timeout = timeout;
    current_thread->wait = wait;

    for (i = 0, entry = wait->queues; i < count; i++, entry++)
    {
        struct object *obj = objects[i];
        entry->thread = current_thread;
        if (!obj->ops->add_queue( obj, entry ))
        {
            wait->count = i;
            end_wait( current_thread );
            return 0;
        }
    }
    return 1;
}

/* check if the thread waiting condition is satisfied */
static int check_wait( struct thread *thread )
{
    int i, signaled;
    struct thread_wait *wait = thread->wait;
    struct wait_queue_entry *entry;

    assert( wait );

    if ((wait->flags & SELECT_INTERRUPTIBLE) && !list_empty( &thread->system_apc ))
        return STATUS_USER_APC;

    /* Suspended threads may not acquire locks, but they can run system APCs */
    if (thread->process->suspend + thread->suspend > 0) return -1;

    if (wait->flags & SELECT_ALL)
    {
        int not_ok = 0;
        /* Note: we must check them all anyway, as some objects may
         * want to do something when signaled, even if others are not */
        for (i = 0, entry = wait->queues; i < wait->count; i++, entry++)
            not_ok |= !entry->obj->ops->signaled( entry->obj, thread );
        if (not_ok) goto other_checks;
        /* Wait satisfied: tell it to all objects */
        signaled = 0;
        for (i = 0, entry = wait->queues; i < wait->count; i++, entry++)
            if (entry->obj->ops->satisfied( entry->obj, thread ))
                signaled = STATUS_ABANDONED_WAIT_0;
        return signaled;
    }
    else
    {
        for (i = 0, entry = wait->queues; i < wait->count; i++, entry++)
        {
            if (!entry->obj->ops->signaled( entry->obj, thread )) continue;
            /* Wait satisfied: tell it to the object */
            signaled = i;
            if (entry->obj->ops->satisfied( entry->obj, thread ))
                signaled = i + STATUS_ABANDONED_WAIT_0;
            return signaled;
        }
    }

 other_checks:
    if ((wait->flags & SELECT_ALERTABLE) && !list_empty(&thread->user_apc)) return STATUS_USER_APC;
    if (wait->timeout <= current_time) return STATUS_TIMEOUT;
    return -1;
}

/* send the wakeup signal to a thread */
static int send_thread_wakeup( struct thread *thread, client_ptr_t cookie, int signaled )
{
    struct wake_up_reply reply;
    int ret;

    memset( &reply, 0, sizeof(reply) );
    reply.cookie   = cookie;
    reply.signaled = signaled;
#ifdef CONFIG_UNIFIED_KERNEL
    complete( &thread->completion );
    thread->wake_info.cookie   = cookie;
    thread->wake_info.signaled = signaled;
    return 0;
#else
    if ((ret = write( get_unix_fd( thread->wait_fd ), &reply, sizeof(reply) )) == sizeof(reply))
        return 0;
#endif
    if (ret >= 0)
        fatal_protocol_error( thread, "partial wakeup write %d\n", ret );
    else if (errno == EPIPE)
        kill_thread( thread, 0 );  /* normal death */
    else
        fatal_protocol_error( thread, "write: %s\n", strerror( errno ));
    return -1;
}

/* attempt to wake up a thread */
/* return >0 if OK, 0 if the wait condition is still not satisfied */
int wake_thread( struct thread *thread )
{
    int signaled, count;
    client_ptr_t cookie;

    for (count = 0; thread->wait; count++)
    {
        if ((signaled = check_wait( thread )) == -1) break;

        cookie = thread->wait->cookie;
        if (debug_level) fprintf( stderr, "%04x: *wakeup* signaled=%d\n", thread->id, signaled );
        end_wait( thread );
        if (send_thread_wakeup( thread, cookie, signaled ) == -1) /* error */
	    break;
    }
    return count;
}

/* thread wait timeout */
static void thread_timeout( void *ptr )
{
    struct thread_wait *wait = ptr;
    struct thread *thread = wait->thread;
    client_ptr_t cookie = wait->cookie;

    wait->user = NULL;
    if (thread->wait != wait) return; /* not the top-level wait, ignore it */
    if (thread->suspend + thread->process->suspend > 0) return;  /* suspended, ignore it */

    if (debug_level) fprintf( stderr, "%04x: *wakeup* signaled=TIMEOUT\n", thread->id );
    end_wait( thread );
    if (send_thread_wakeup( thread, cookie, STATUS_TIMEOUT ) == -1) return;
    /* check if other objects have become signaled in the meantime */
    wake_thread( thread );
}

/* try signaling an event flag, a semaphore or a mutex */
static int signal_object( obj_handle_t handle )
{
    struct object *obj;
    int ret = 0;

    obj = get_handle_obj( current_thread->process, handle, 0, NULL );
    if (obj)
    {
        ret = obj->ops->signal( obj, get_handle_access( current_thread->process, handle ));
        release_object( obj );
    }
    return ret;
}

/* select on a list of handles */
static timeout_t select_on( unsigned int count, client_ptr_t cookie, const obj_handle_t *handles,
                            int flags, timeout_t timeout, obj_handle_t signal_obj )
{
    int ret;
    unsigned int i;
    struct object *objects[MAXIMUM_WAIT_OBJECTS];

    if (timeout <= 0) timeout = current_time - timeout;

    if (count > MAXIMUM_WAIT_OBJECTS)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return 0;
    }
    for (i = 0; i < count; i++)
    {
        if (!(objects[i] = get_handle_obj( current_thread->process, handles[i], SYNCHRONIZE, NULL )))
            break;
    }

    if (i < count) goto done;
    if (!wait_on( count, objects, flags, timeout )) goto done;

    /* signal the object */
    if (signal_obj)
    {
        if (!signal_object( signal_obj ))
        {
            end_wait( current_thread );
            goto done;
        }
        /* check if we woke ourselves up */
        if (!current_thread->wait) goto done;
    }

    if ((ret = check_wait( current_thread )) != -1)
    {
        /* condition is already satisfied */
        end_wait( current_thread );
        set_error( ret );
        goto done;
    }

    /* now we need to wait */
    if (current_thread->wait->timeout != TIMEOUT_INFINITE)
    {
        if (!(current_thread->wait->user = add_timeout_user( current_thread->wait->timeout,
                                                      thread_timeout, current_thread->wait )))
        {
            end_wait( current_thread );
            goto done;
        }
    }
    current_thread->wait->cookie = cookie;
#ifdef CONFIG_UNIFIED_KERNEL
    wait_for_completion( &current_thread->completion );
    if (cookie == current_thread->wake_info.cookie)
    {
	set_error( current_thread->wake_info.signaled );
    }
    else
    {
	klog(0,"cookie is broken \n");
    }
#else
    set_error( STATUS_PENDING );
#endif

done:
    while (i > 0) release_object( objects[--i] );
    return timeout;
}

/* attempt to wake threads sleeping on the object wait queue */
void uk_wake_up( struct object *obj, int max )
{
    struct list_head *ptr;

    LIST_FOR_EACH( ptr, &obj->wait_queue )
    {
        struct wait_queue_entry *entry = LIST_ENTRY( ptr, struct wait_queue_entry, entry );
        if (!wake_thread( entry->thread )) continue;
        if (max && !--max) break;
        /* restart at the head of the list since a wake up can change the object wait queue */
        ptr = &obj->wait_queue;
    }
}

/* return the apc queue to use for a given apc type */
static inline struct list_head *get_apc_queue( struct thread *thread, enum apc_type type )
{
    switch(type)
    {
    case APC_NONE:
    case APC_USER:
    case APC_TIMER:
        return &thread->user_apc;
    default:
        return &thread->system_apc;
    }
}

/* check if thread is currently waiting for a (system) apc */
static inline int is_in_apc_wait( struct thread *thread )
{
    return (thread->process->suspend || thread->suspend ||
            (thread->wait && (thread->wait->flags & SELECT_INTERRUPTIBLE)));
}

/* queue an existing APC to a given thread */
static int queue_apc( struct process *process, struct thread *thread, struct thread_apc *apc )
{
    struct list_head *queue;

    if (!thread)  /* find a suitable thread inside the process */
    {
        struct thread *candidate;

        /* first try to find a waiting thread */
        LIST_FOR_EACH_ENTRY( candidate, &process->thread_list, struct thread, proc_entry )
        {
            if (candidate->state == TERMINATED) continue;
            if (is_in_apc_wait( candidate ))
            {
                thread = candidate;
                break;
            }
        }
        if (!thread)
        {
            /* then use the first one that accepts a signal */
            LIST_FOR_EACH_ENTRY( candidate, &process->thread_list, struct thread, proc_entry )
            {
                if (send_thread_signal( candidate, SIGUSR1 ))
                {
                    thread = candidate;
                    break;
                }
            }
        }
        if (!thread) return 0;  /* nothing found */
        queue = get_apc_queue( thread, apc->call.type );
    }
    else
    {
        if (thread->state == TERMINATED) return 0;
        queue = get_apc_queue( thread, apc->call.type );
        /* send signal for system APCs if needed */
        if (queue == &thread->system_apc && list_empty( queue ) && !is_in_apc_wait( thread ))
        {
            if (!send_thread_signal( thread, SIGUSR1 )) return 0;
        }
        /* cancel a possible previous APC with the same owner */
        if (apc->owner) thread_cancel_apc( thread, apc->owner, apc->call.type );
    }

    grab_object( apc );
    wine_list_add_tail( queue, &apc->entry );
    if (!list_prev( queue, &apc->entry ))  /* first one */
        wake_thread( thread );

    return 1;
}

/* queue an async procedure call */
int thread_queue_apc( struct thread *thread, struct object *owner, const apc_call_t *call_data )
{
    struct thread_apc *apc;
    int ret = 0;

    if ((apc = create_apc( owner, call_data )))
    {
        ret = queue_apc( NULL, thread, apc );
        release_object( apc );
    }
    return ret;
}

/* cancel the async procedure call owned by a specific object */
void thread_cancel_apc( struct thread *thread, struct object *owner, enum apc_type type )
{
    struct thread_apc *apc;
    struct list_head *queue = get_apc_queue( thread, type );

    LIST_FOR_EACH_ENTRY( apc, queue, struct thread_apc, entry )
    {
        if (apc->owner != owner) continue;
        list_remove( &apc->entry );
        apc->executed = 1;
	uk_wake_up( &apc->obj, 0 );
        release_object( apc );
        return;
    }
}

/* remove the head apc from the queue; the returned object must be released by the caller */
static struct thread_apc *thread_dequeue_apc( struct thread *thread, int system_only )
{
    struct thread_apc *apc = NULL;
    struct list_head *ptr = list_head( &thread->system_apc );

    if (!ptr && !system_only) ptr = list_head( &thread->user_apc );
    if (ptr)
    {
        apc = LIST_ENTRY( ptr, struct thread_apc, entry );
        list_remove( ptr );
    }
    return apc;
}

/* clear an APC queue, cancelling all the APCs on it */
static void clear_apc_queue( struct list_head *queue )
{
    struct list_head *ptr;

    while ((ptr = list_head( queue )))
    {
        struct thread_apc *apc = LIST_ENTRY( ptr, struct thread_apc, entry );
        list_remove( &apc->entry );
        apc->executed = 1;
	uk_wake_up( &apc->obj, 0 );
        release_object( apc );
    }
}

/* add an fd to the inflight list */
/* return list index, or -1 on error */
int thread_add_inflight_fd( struct thread *thread, int client, int server )
{
    int i;

    if (server == -1) return -1;
    if (client == -1)
    {
        close( server );
        return -1;
    }

    /* first check if we already have an entry for this fd */
    for (i = 0; i < MAX_INFLIGHT_FDS; i++)
        if (thread->inflight[i].client == client)
        {
            close( thread->inflight[i].server );
            thread->inflight[i].server = server;
            return i;
        }

    /* now find a free spot to store it */
    for (i = 0; i < MAX_INFLIGHT_FDS; i++)
        if (thread->inflight[i].client == -1)
        {
            thread->inflight[i].client = client;
            thread->inflight[i].server = server;
            return i;
        }
    return -1;
}

#ifdef CONFIG_UNIFIED_KERNEL
int thread_get_inflight_fd( struct thread *thread, int client )
{
    int i, ret;
    int new_fd;

    if (client == -1) return -1;

    for (i = 0; i < MAX_INFLIGHT_FDS; i++)
    {
        if (thread->inflight[i].client == client)
        {
            ret = thread->inflight[i].server;
            thread->inflight[i].server = thread->inflight[i].client = -1;
            return ret;
        }
    }

    /* not found , need add to cache */
    new_fd = dup( client );
    if (new_fd >= 0)
    {
        thread_add_inflight_fd( thread, client, new_fd );
        return new_fd;
    }
    else
    {
        klog(0,"dup fd error \n");
        return -1;
    }
}
#else
/* get an inflight fd and purge it from the list */
/* the fd must be closed when no longer used */
int thread_get_inflight_fd( struct thread *thread, int client )
{
    int i, ret;

    if (client == -1) return -1;

    do
    {
        for (i = 0; i < MAX_INFLIGHT_FDS; i++)
        {
            if (thread->inflight[i].client == client)
            {
                ret = thread->inflight[i].server;
                thread->inflight[i].server = thread->inflight[i].client = -1;
                return ret;
            }
        }
    } while (!receive_fd( thread->process ));  /* in case it is still in the socket buffer */
    return -1;
}
#endif

/* kill a thread on the spot */
void kill_thread( struct thread *thread, int violent_death )
{
    if (thread->state == TERMINATED) return;  /* already killed */
    thread->state = TERMINATED;
    thread->exit_time = current_time;
#ifndef CONFIG_UNIFIED_KERNEL
    if (current_thread == thread) current_thread = NULL;
#endif
    if (debug_level)
        fprintf( stderr,"%04x: *killed* exit_code=%d\n",
                 thread->id, thread->exit_code );
    if (thread->wait)
    {
        while (thread->wait) end_wait( thread );
        send_thread_wakeup( thread, 0, thread->exit_code );
        /* if it is waiting on the socket, we don't need to send a SIGQUIT */
        violent_death = 0;
    }
    kill_console_processes( thread, 0 );
    debug_exit_thread( thread );
    abandon_mutexes( thread );
    uk_wake_up( &thread->obj, 0 );
    if (violent_death) send_thread_signal( thread, SIGQUIT );
    cleanup_thread( thread );
    remove_process_thread( thread->process, thread );
    release_object( thread );
}

/* copy parts of a context structure */
static void copy_context( context_t *to, const context_t *from, unsigned int flags )
{
    assert( to->cpu == from->cpu );
    to->flags |= flags;
    if (flags & SERVER_CTX_CONTROL) to->ctl = from->ctl;
    if (flags & SERVER_CTX_INTEGER) to->integer = from->integer;
    if (flags & SERVER_CTX_SEGMENTS) to->seg = from->seg;
    if (flags & SERVER_CTX_FLOATING_POINT) to->fp = from->fp;
    if (flags & SERVER_CTX_DEBUG_REGISTERS) to->debug = from->debug;
    if (flags & SERVER_CTX_EXTENDED_REGISTERS) to->ext = from->ext;
}

/* return the context flags that correspond to system regs */
/* (system regs are the ones we can't access on the client side) */
static unsigned int get_context_system_regs( enum cpu_type cpu )
{
    switch (cpu)
    {
    case CPU_x86:     return SERVER_CTX_DEBUG_REGISTERS;
    case CPU_x86_64:  return SERVER_CTX_DEBUG_REGISTERS;
    case CPU_POWERPC: return 0;
    case CPU_ARM:     return 0;
    case CPU_ARM64:   return 0;
    }
    return 0;
}

/* trigger a breakpoint event in a given thread */
void break_thread( struct thread *thread )
{
    debug_event_t data;

    assert( thread->context );

    memset( &data, 0, sizeof(data) );
    data.exception.first     = 1;
    data.exception.exc_code  = STATUS_BREAKPOINT;
    data.exception.flags     = EXCEPTION_CONTINUABLE;
    switch (thread->context->cpu)
    {
    case CPU_x86:
        data.exception.address = thread->context->ctl.i386_regs.eip;
        break;
    case CPU_x86_64:
        data.exception.address = thread->context->ctl.x86_64_regs.rip;
        break;
    case CPU_POWERPC:
        data.exception.address = thread->context->ctl.powerpc_regs.iar;
        break;
    case CPU_ARM:
        data.exception.address = thread->context->ctl.arm_regs.pc;
        break;
    case CPU_ARM64:
        data.exception.address = thread->context->ctl.arm64_regs.pc;
        break;
    }
    generate_debug_event( thread, EXCEPTION_DEBUG_EVENT, &data );
    thread->debug_break = 0;
}

/* take a snapshot of currently running threads */
struct thread_snapshot *thread_snap( int *count )
{
    struct thread_snapshot *snapshot, *ptr;
    struct thread *thread;
    int total = 0;

    LIST_FOR_EACH_ENTRY( thread, &thread_list, struct thread, entry )
        if (thread->state != TERMINATED) total++;
    if (!total || !(snapshot = mem_alloc( sizeof(*snapshot) * total ))) return NULL;
    ptr = snapshot;
    LIST_FOR_EACH_ENTRY( thread, &thread_list, struct thread, entry )
    {
        if (thread->state == TERMINATED) continue;
        ptr->thread   = thread;
        ptr->count    = thread->obj.refcount;
        ptr->priority = thread->priority;
        grab_object( thread );
        ptr++;
    }
    *count = total;
    return snapshot;
}

/* gets the current_thread impersonation token */
struct token *thread_get_impersonation_token( struct thread *thread )
{
    if (thread->token)
        return thread->token;
    else
        return thread->process->token;
}

/* create a new thread */
DECL_HANDLER(new_thread)
{
    struct thread *thread;
    int request_fd = thread_get_inflight_fd( current_thread, req->request_fd );

#ifndef CONFIG_UNIFIED_KERNEL
    if (request_fd == -1 || fcntl( request_fd, F_SETFL, O_NONBLOCK ) == -1)
    {
        if (request_fd != -1) close( request_fd );
        set_error( STATUS_INVALID_HANDLE );
        return;
    }
#endif

    if ((thread = create_thread( request_fd, current_thread->process )))
    {
        if (req->suspend) thread->suspend++;
        reply->tid = get_thread_id( thread );
        if ((reply->handle = alloc_handle( current_thread->process, thread, req->access, req->attributes )))
        {
            /* thread object will be released when the thread gets killed */
            return;
        }
        kill_thread( thread, 1 );
    }
}

/* initialize a new thread */
DECL_HANDLER(init_thread)
{
    unsigned int prefix_cpu_mask = get_prefix_cpu_mask();
    struct process *process = current_thread->process;
    int wait_fd, reply_fd;

    if ((reply_fd = thread_get_inflight_fd( current_thread, req->reply_fd )) == -1)
    {
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        return;
    }
    if ((wait_fd = thread_get_inflight_fd( current_thread, req->wait_fd )) == -1)
    {
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        goto error;
    }

    if (current_thread->reply_fd)  /* already initialised */
    {
        set_error( STATUS_INVALID_PARAMETER );
        goto error;
    }

#ifndef CONFIG_UNIFIED_KERNEL
    if (fcntl( reply_fd, F_SETFL, O_NONBLOCK ) == -1) goto error;

    current_thread->reply_fd = create_anonymous_fd( &thread_fd_ops, reply_fd, &current_thread->obj, 0 );
    current_thread->wait_fd  = create_anonymous_fd( &thread_fd_ops, wait_fd, &current_thread->obj, 0 );
    if (!current_thread->reply_fd || !current_thread->wait_fd) return;
#endif

    if (!is_valid_address(req->teb))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    current_thread->unix_pid = req->unix_pid;
    current_thread->unix_tid = req->unix_tid;
    current_thread->teb      = req->teb;

    if (!process->peb)  /* first thread, initialize the process too */
    {
        if (!CPU_FLAG(req->cpu) || !(supported_cpus & prefix_cpu_mask & CPU_FLAG(req->cpu)))
        {
            if (!(supported_cpus & CPU_64BIT_MASK))
                set_error( STATUS_NOT_SUPPORTED );
            else
                set_error( STATUS_NOT_REGISTRY_FILE );  /* server supports it but not the prefix */
            return;
        }
        process->unix_pid = current_thread->unix_pid;
        process->peb      = req->entry;
        process->cpu      = req->cpu;
        reply->info_size  = init_process( current_thread );
        if (!process->parent)
            process->affinity = current_thread->affinity = get_thread_affinity( current_thread );
        else
            set_thread_affinity( current_thread, current_thread->affinity );
    }
    else
    {
        if (req->cpu != process->cpu)
        {
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }
        if (process->unix_pid != current_thread->unix_pid)
            process->unix_pid = -1;  /* can happen with linuxthreads */
        stop_thread_if_suspended( current_thread );
        generate_debug_event( current_thread, CREATE_THREAD_DEBUG_EVENT, &req->entry );
        set_thread_affinity( current_thread, current_thread->affinity );
    }
    debug_level = max( debug_level, req->debug_level );

    reply->pid     = get_process_id( process );
    reply->tid     = get_thread_id( current_thread );
    reply->version = SERVER_PROTOCOL_VERSION;
    reply->server_start = server_start_time;
    reply->all_cpus     = supported_cpus & prefix_cpu_mask;
    return;

 error:
    if (reply_fd != -1) close( reply_fd );
    if (wait_fd != -1) close( wait_fd );
}

/* terminate a thread */
DECL_HANDLER(terminate_thread)
{
    struct thread *thread;

    reply->self = 0;
    reply->last = 0;
    if ((thread = get_thread_from_handle( req->handle, THREAD_TERMINATE )))
    {
        thread->exit_code = req->exit_code;
        if (thread != current_thread) kill_thread( thread, 1 );
        else
        {
            reply->self = 1;
            reply->last = (thread->process->running_threads == 1);
        }
        release_object( thread );
    }
}

/* open a handle to a thread */
DECL_HANDLER(open_thread)
{
    struct thread *thread = get_thread_from_id( req->tid );

    reply->handle = 0;
    if (thread)
    {
        reply->handle = alloc_handle( current_thread->process, thread, req->access, req->attributes );
        release_object( thread );
    }
}

/* fetch information about a thread */
DECL_HANDLER(get_thread_info)
{
    struct thread *thread;
    obj_handle_t handle = req->handle;

    if (!handle) thread = get_thread_from_id( req->tid_in );
    else thread = get_thread_from_handle( req->handle, THREAD_QUERY_INFORMATION );

    if (thread)
    {
        reply->pid            = get_process_id( thread->process );
        reply->tid            = get_thread_id( thread );
        reply->teb            = thread->teb;
        reply->exit_code      = (thread->state == TERMINATED) ? thread->exit_code : STATUS_PENDING;
        reply->priority       = thread->priority;
        reply->affinity       = thread->affinity;
        reply->creation_time  = thread->creation_time;
        reply->exit_time      = thread->exit_time;
        reply->last           = thread->process->running_threads == 1;

        release_object( thread );
    }
}

/* set information about a thread */
DECL_HANDLER(set_thread_info)
{
    struct thread *thread;

    if ((thread = get_thread_from_handle( req->handle, THREAD_SET_INFORMATION )))
    {
        set_thread_info( thread, req );
        release_object( thread );
    }
}

/* suspend a thread */
DECL_HANDLER(suspend_thread)
{
    struct thread *thread;

    if ((thread = get_thread_from_handle( req->handle, THREAD_SUSPEND_RESUME )))
    {
        if (thread->state == TERMINATED) set_error( STATUS_ACCESS_DENIED );
        else reply->count = suspend_thread( thread );
        release_object( thread );
    }
}

/* resume a thread */
DECL_HANDLER(resume_thread)
{
    struct thread *thread;

    if ((thread = get_thread_from_handle( req->handle, THREAD_SUSPEND_RESUME )))
    {
        reply->count = resume_thread( thread );
        release_object( thread );
    }
}

/* select on a handle list */
DECL_HANDLER(select)
{
    struct thread_apc *apc;
    unsigned int count;
    const apc_result_t *result = get_req_data();
    const obj_handle_t *handles = (const obj_handle_t *)(result + 1);

    if (get_req_data_size() < sizeof(*result))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    count = (get_req_data_size() - sizeof(*result)) / sizeof(obj_handle_t);

    /* first store results of previous apc */
    if (req->prev_apc)
    {
        if (!(apc = (struct thread_apc *)get_handle_obj( current_thread->process, req->prev_apc,
                                                         0, &thread_apc_ops ))) return;
        apc->result = *result;
        apc->executed = 1;
        if (apc->result.type == APC_CREATE_THREAD)  /* transfer the handle to the caller process */
        {
            obj_handle_t handle = duplicate_handle( current_thread->process, apc->result.create_thread.handle,
                                                    apc->caller->process, 0, 0, DUP_HANDLE_SAME_ACCESS );
            close_handle( current_thread->process, apc->result.create_thread.handle );
            apc->result.create_thread.handle = handle;
            clear_error();  /* ignore errors from the above calls */
        }
        else if (apc->result.type == APC_ASYNC_IO)
        {
            if (apc->owner)
                async_set_result( apc->owner, apc->result.async_io.status,
                                  apc->result.async_io.total, apc->result.async_io.apc );
        }
	uk_wake_up( &apc->obj, 0 );
        close_handle( current_thread->process, req->prev_apc );
        release_object( apc );
    }

    reply->timeout = select_on( count, req->cookie, handles, req->flags, req->timeout, req->signal );

    if (get_error() == STATUS_USER_APC)
    {
        for (;;)
        {
            if (!(apc = thread_dequeue_apc( current_thread, !(req->flags & SELECT_ALERTABLE) )))
                break;
            /* Optimization: ignore APC_NONE calls, they are only used to
             * wake up a thread, but since we got here the thread woke up already.
             */
            if (apc->call.type != APC_NONE)
            {
                if ((reply->apc_handle = alloc_handle( current_thread->process, apc, SYNCHRONIZE, 0 )))
                    reply->call = apc->call;
                release_object( apc );
                break;
            }
            apc->executed = 1;
	    uk_wake_up( &apc->obj, 0 );
            release_object( apc );
        }
    }
}

/* queue an APC for a thread or process */
DECL_HANDLER(queue_apc)
{
    struct thread *thread = NULL;
    struct process *process = NULL;
    struct thread_apc *apc;

    if (!(apc = create_apc( NULL, &req->call ))) return;

    switch (apc->call.type)
    {
    case APC_NONE:
    case APC_USER:
        thread = get_thread_from_handle( req->handle, THREAD_SET_CONTEXT );
        break;
    case APC_VIRTUAL_ALLOC:
    case APC_VIRTUAL_FREE:
    case APC_VIRTUAL_PROTECT:
    case APC_VIRTUAL_FLUSH:
    case APC_VIRTUAL_LOCK:
    case APC_VIRTUAL_UNLOCK:
    case APC_UNMAP_VIEW:
        process = get_process_from_handle( req->handle, PROCESS_VM_OPERATION );
        break;
    case APC_VIRTUAL_QUERY:
        process = get_process_from_handle( req->handle, PROCESS_QUERY_INFORMATION );
        break;
    case APC_MAP_VIEW:
        process = get_process_from_handle( req->handle, PROCESS_VM_OPERATION );
        if (process && process != current_thread->process)
        {
            /* duplicate the handle into the target process */
            obj_handle_t handle = duplicate_handle( current_thread->process, apc->call.map_view.handle,
                                                    process, 0, 0, DUP_HANDLE_SAME_ACCESS );
            if (handle) apc->call.map_view.handle = handle;
            else
            {
                release_object( process );
                process = NULL;
            }
        }
        break;
    case APC_CREATE_THREAD:
        process = get_process_from_handle( req->handle, PROCESS_CREATE_THREAD );
        break;
    default:
        set_error( STATUS_INVALID_PARAMETER );
        break;
    }

    if (thread)
    {
        if (!queue_apc( NULL, thread, apc )) set_error( STATUS_THREAD_IS_TERMINATING );
        release_object( thread );
    }
    else if (process)
    {
        reply->self = (process == current_thread->process);
        if (!reply->self)
        {
            obj_handle_t handle = alloc_handle( current_thread->process, apc, SYNCHRONIZE, 0 );
            if (handle)
            {
                if (queue_apc( process, NULL, apc ))
                {
                    apc->caller = (struct thread *)grab_object( current_thread );
                    reply->handle = handle;
                }
                else
                {
                    close_handle( current_thread->process, handle );
                    set_error( STATUS_PROCESS_IS_TERMINATING );
                }
            }
        }
        release_object( process );
    }

    release_object( apc );
}

/* Get the result of an APC call */
DECL_HANDLER(get_apc_result)
{
    struct thread_apc *apc;

    if (!(apc = (struct thread_apc *)get_handle_obj( current_thread->process, req->handle,
                                                     0, &thread_apc_ops ))) return;
    if (!apc->executed) set_error( STATUS_PENDING );
    else
    {
        reply->result = apc->result;
        /* close the handle directly to avoid an extra round-trip */
        close_handle( current_thread->process, req->handle );
    }
    release_object( apc );
}

/* retrieve the current_thread context of a thread */
DECL_HANDLER(get_thread_context)
{
    struct thread *thread;
    context_t *context;

    if (get_reply_max_size() < sizeof(context_t))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    if (!(thread = get_thread_from_handle( req->handle, THREAD_GET_CONTEXT ))) return;
    reply->self = (thread == current_thread);

    if (thread != current_thread && !thread->context)
    {
        /* thread is not suspended, retry (if it's still running) */
        if (thread->state == RUNNING)
        {
            set_error( STATUS_PENDING );
            if (req->suspend)
            {
                release_object( thread );
                /* make sure we have suspend access */
                if (!(thread = get_thread_from_handle( req->handle, THREAD_SUSPEND_RESUME ))) return;
                suspend_thread( thread );
            }
        }
        else set_error( STATUS_UNSUCCESSFUL );
    }
    else if ((context = set_reply_data_size( sizeof(context_t) )))
    {
        unsigned int flags = get_context_system_regs( thread->process->cpu );

        memset( context, 0, sizeof(context_t) );
        context->cpu = thread->process->cpu;
        if (thread->context) copy_context( context, thread->context, req->flags & ~flags );
        if (flags) get_thread_context( thread, context, flags );
    }
    release_object( thread );
}

/* set the current_thread context of a thread */
DECL_HANDLER(set_thread_context)
{
    struct thread *thread;
    const context_t *context = get_req_data();

    if (get_req_data_size() < sizeof(context_t))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    if (!(thread = get_thread_from_handle( req->handle, THREAD_SET_CONTEXT ))) return;
    reply->self = (thread == current_thread);

    if (thread != current_thread && !thread->context)
    {
        /* thread is not suspended, retry (if it's still running) */
        if (thread->state == RUNNING)
        {
            set_error( STATUS_PENDING );
            if (req->suspend)
            {
                release_object( thread );
                /* make sure we have suspend access */
                if (!(thread = get_thread_from_handle( req->handle, THREAD_SUSPEND_RESUME ))) return;
                suspend_thread( thread );
            }
        }
        else set_error( STATUS_UNSUCCESSFUL );
    }
    else if (context->cpu == thread->process->cpu)
    {
        unsigned int system_flags = get_context_system_regs(context->cpu) & context->flags;
        unsigned int client_flags = context->flags & ~system_flags;

        if (system_flags) set_thread_context( thread, context, system_flags );
        if (thread->context && !get_error()) copy_context( thread->context, context, client_flags );
    }
    else set_error( STATUS_INVALID_PARAMETER );

    release_object( thread );
}

/* retrieve the suspended context of a thread */
DECL_HANDLER(get_suspend_context)
{
    if (get_reply_max_size() < sizeof(context_t))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    if (current_thread->suspend_context)
    {
        set_reply_data_ptr( current_thread->suspend_context, sizeof(context_t) );
        if (current_thread->context == current_thread->suspend_context)
        {
            current_thread->context = NULL;
            stop_thread_if_suspended( current_thread );
        }
        current_thread->suspend_context = NULL;
    }
    else set_error( STATUS_INVALID_PARAMETER );  /* not suspended, shouldn't happen */
}

/* store the suspended context of a thread */
DECL_HANDLER(set_suspend_context)
{
    const context_t *context = get_req_data();

    if (get_req_data_size() < sizeof(context_t))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    if (current_thread->context || context->cpu != current_thread->process->cpu)
    {
        /* nested suspend or exception, shouldn't happen */
        set_error( STATUS_INVALID_PARAMETER );
    }
    else if ((current_thread->suspend_context = mem_alloc( sizeof(context_t) )))
    {
        memcpy( current_thread->suspend_context, get_req_data(), sizeof(context_t) );
        current_thread->context = current_thread->suspend_context;
        if (current_thread->debug_break) break_thread( current_thread );
    }
}

/* fetch a selector entry for a thread */
DECL_HANDLER(get_selector_entry)
{
    struct thread *thread;
    if ((thread = get_thread_from_handle( req->handle, THREAD_QUERY_INFORMATION )))
    {
        get_selector_entry( thread, req->entry, &reply->base, &reply->limit, &reply->flags );
        release_object( thread );
    }
}
