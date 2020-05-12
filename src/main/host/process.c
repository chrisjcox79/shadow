/*
 * The Shadow Simulator
 * Copyright (c) 2010-2011, Rob Jansen
 * See LICENSE for licensing information
 */
#include "main/host/process.h"

#include <bits/stdint-intn.h>
#include <bits/stdint-uintn.h>
#include <bits/types/clockid_t.h>
#include <bits/types/struct_timespec.h>
#include <bits/types/struct_timeval.h>
#include <bits/types/struct_tm.h>
#include <bits/types/time_t.h>
#include <errno.h>
#include <fcntl.h>
#include <features.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/file.h>
#include <sys/un.h>
#include <syscall.h>
#include <time.h>
#include <unistd.h>

#include "glib/gprintf.h"
#include "main/core/support/definitions.h"
#include "main/core/support/object_counter.h"
#include "main/core/work/task.h"
#include "main/core/worker.h"
#include "main/host/cpu.h"
#include "main/host/descriptor/channel.h"
#include "main/host/descriptor/descriptor.h"
#include "main/host/descriptor/socket.h"
#include "main/host/descriptor/tcp.h"
#include "main/host/descriptor/timer.h"
#include "main/host/host.h"
#include "main/host/process.h"
#include "main/host/thread.h"
#include "main/host/thread_preload.h"
#include "main/host/tracker.h"
#include "main/routing/address.h"
#include "main/routing/dns.h"
#include "main/utility/random.h"
#include "main/utility/utility.h"
#include "support/logger/logger.h"

#include "main/host/thread_ptrace.h"

struct _Process {
    /* Host owning this process */
    Host* host;

    /* unique id of the program that this process should run */
    guint processID;
    GString* processName;

    /* Which InterposeMethod to use for this process's threads */
    InterposeMethod interposeMethod;

    /* the shadow plugin executable */
    struct {
        /* the name and path to the executable that we will exec */
        GString* exeName;
        GString* exePath;

        /* TRUE from when we've called into plug-in code until the call completes.
         * Note that the plug-in may get back into shadow code during execution, by
         * calling a function that we intercept. */
        gboolean isExecuting;
    } plugin;

    /* timer that tracks the amount of CPU time we spend on plugin execution and processing */
    GTimer* cpuDelayTimer;
    gdouble totalRunTime;

    /* process boot and shutdown variables */
    SimulationTime startTime;
    SimulationTime stopTime;

    /* vector of argument strings passed to exec */
    gchar** argv;
    /* vector of environment variables passed to exec */
    gchar** envv;

    gint returnCode;
    gboolean didLogReturnCode;

    /* the main execution unit for the plugin */
    Thread* mainThread;
    gint threadIDCounter;

    // TODO add spawned threads

    int stderrFD;
    int stdoutFD;

    gint referenceCount;
    MAGIC_DECLARE;
};

const gchar* process_getName(Process* proc) {
    MAGIC_ASSERT(proc);
    utility_assert(proc->processName->str);
    return proc->processName->str;
}

static void _process_handleTimerResult(Process* proc, gdouble elapsedTimeSec) {
    SimulationTime delay = (SimulationTime) (elapsedTimeSec * SIMTIME_ONE_SECOND);
    Host* currentHost = worker_getActiveHost();
    cpu_addDelay(host_getCPU(currentHost), delay);
    tracker_addProcessingTime(host_getTracker(currentHost), delay);
    proc->totalRunTime += elapsedTimeSec;
}

static void _process_logReturnCode(Process* proc, gint code) {
    if(!proc->didLogReturnCode) {
        GString* mainResultString = g_string_new(NULL);
        g_string_printf(mainResultString, "main %s code '%i' for process '%s'",
                        ((code == 0) ? "success" : "error"), code,
                        process_getName(proc));

        if(code == 0) {
            message("%s", mainResultString->str);
        } else {
            warning("%s", mainResultString->str);
            worker_incrementPluginError();
        }

        g_string_free(mainResultString, TRUE);

        proc->didLogReturnCode = TRUE;
    }
}

static void _process_check(Process* proc) {
    MAGIC_ASSERT(proc);

    if(!proc->mainThread) {
        return;
    }

    if(thread_isRunning(proc->mainThread)) {
        info("process '%s' is running, but threads are blocked waiting for "
             "events",
             process_getName(proc));
    } else {
        /* collect return code */
        int returnCode = thread_getReturnCode(proc->mainThread);

        message("process '%s' has completed or is otherwise no longer running",
                process_getName(proc));
        _process_logReturnCode(proc, returnCode);

        thread_terminate(proc->mainThread);
        thread_unref(proc->mainThread);
        proc->mainThread = NULL;

        message("total runtime for process '%s' was %f seconds",
                process_getName(proc), proc->totalRunTime);
    }
}

static void _process_start(Process* proc) {
    MAGIC_ASSERT(proc);

    /* dont do anything if we are already running */
    if(process_isRunning(proc)) {
        return;
    }

    // Set up stdout
    {
        gchar* stdoutFileName =
            g_strdup_printf("%s/%s.stdout", host_getDataPath(proc->host),
                            proc->processName->str);
        proc->stdoutFD = open(stdoutFileName, O_WRONLY | O_CREAT | O_TRUNC,
                              S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (proc->stdoutFD < 0) {
            error("Opening %s: %s", stdoutFileName, strerror(errno));
        }
        g_free(stdoutFileName);
    }

    // Set up stderr
    {
        gchar* stderrFileName =
            g_strdup_printf("%s/%s.stderr", host_getDataPath(proc->host),
                            proc->processName->str);
        proc->stderrFD = open(stderrFileName, O_WRONLY | O_CREAT | O_TRUNC,
                              S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (proc->stderrFD < 0) {
            error("Opening %s: %s", stderrFileName, strerror(errno));
        }
        g_free(stderrFileName);
    }

    utility_assert(proc->mainThread == NULL);
    if (proc->interposeMethod == INTERPOSE_PTRACE) {
        proc->mainThread =
            threadptrace_new(proc->host, proc, proc->threadIDCounter++);
    } else if (proc->interposeMethod == INTERPOSE_PRELOAD) {
        proc->mainThread =
            threadpreload_new(proc->host, proc, proc->threadIDCounter++);
    } else {
        error("Bad interposeMethod %d", proc->interposeMethod);
    }

    message("starting process '%s'", process_getName(proc));

    /* now we will execute in the pth/plugin context, so we need to load the state */
    worker_setActiveProcess(proc);

    /* time how long we execute the program */
    g_timer_start(proc->cpuDelayTimer);

    proc->plugin.isExecuting = TRUE;
    /* exec the process and call main to start it */
    thread_run(proc->mainThread, proc->argv, proc->envv, proc->stderrFD, proc->stdoutFD);
    gdouble elapsed = g_timer_elapsed(proc->cpuDelayTimer, NULL);
    _process_handleTimerResult(proc, elapsed);

    worker_setActiveProcess(NULL);

    message(
        "process '%s' started in %f seconds", process_getName(proc), elapsed);

    _process_check(proc);
}

InterposeMethod process_getInterposeMethod(Process* proc) { return proc->interposeMethod; }

void process_continue(Process* proc, Thread* thread) {
    MAGIC_ASSERT(proc);

    /* if we are not running, no need to notify anyone */
    if(!process_isRunning(proc)) {
        return;
    }

    info("switching to thread controller to continue executing process '%s'",
         process_getName(proc));

    worker_setActiveProcess(proc);

    /* time how long we execute the program */
    g_timer_start(proc->cpuDelayTimer);

    proc->plugin.isExecuting = TRUE;
    if (thread) {
        thread_resume(thread);
    } else {
        thread_resume(proc->mainThread);
    }
    proc->plugin.isExecuting = FALSE;

    gdouble elapsed = g_timer_elapsed(proc->cpuDelayTimer, NULL);
    _process_handleTimerResult(proc, elapsed);

    worker_setActiveProcess(NULL);

    info("process '%s' ran for %f seconds", process_getName(proc), elapsed);

    _process_check(proc);
}

void process_stop(Process* proc) {
    MAGIC_ASSERT(proc);

    message("terminating process '%s'", process_getName(proc));

    worker_setActiveProcess(proc);

    /* time how long we execute the program */
    g_timer_start(proc->cpuDelayTimer);

    proc->plugin.isExecuting = TRUE;
    if (proc->mainThread) {
        thread_terminate(proc->mainThread);
        thread_unref(proc->mainThread);
        proc->mainThread = NULL;
    }
    proc->plugin.isExecuting = FALSE;

    gdouble elapsed = g_timer_elapsed(proc->cpuDelayTimer, NULL);
    _process_handleTimerResult(proc, elapsed);

    worker_setActiveProcess(NULL);

    message(
        "process '%s' stopped in %f seconds", process_getName(proc), elapsed);

    _process_check(proc);
}

static void _process_runStartTask(Process* proc, gpointer nothing) {
    _process_start(proc);
}

static void _process_runStopTask(Process* proc, gpointer nothing) {
    process_stop(proc);
}

void process_schedule(Process* proc, gpointer nothing) {
    MAGIC_ASSERT(proc);

    SimulationTime now = worker_getCurrentTime();

    if(proc->stopTime == 0 || proc->startTime < proc->stopTime) {
        SimulationTime startDelay = proc->startTime <= now ? 1 : proc->startTime - now;
        process_ref(proc);
        Task* startProcessTask = task_new((TaskCallbackFunc)_process_runStartTask,
                proc, NULL, (TaskObjectFreeFunc)process_unref, NULL);
        worker_scheduleTask(startProcessTask, startDelay);
        task_unref(startProcessTask);
    }

    if(proc->stopTime > 0 && proc->stopTime > proc->startTime) {
        SimulationTime stopDelay = proc->stopTime <= now ? 1 : proc->stopTime - now;
        process_ref(proc);
        Task* stopProcessTask = task_new((TaskCallbackFunc)_process_runStopTask,
                proc, NULL, (TaskObjectFreeFunc)process_unref, NULL);
        worker_scheduleTask(stopProcessTask, stopDelay);
        task_unref(stopProcessTask);
    }
}

gboolean process_isRunning(Process* proc) {
    MAGIC_ASSERT(proc);
    return (proc->mainThread != NULL && thread_isRunning(proc->mainThread)) ? TRUE : FALSE;
}

gboolean process_wantsNotify(Process* proc, gint epollfd) {
    MAGIC_ASSERT(proc);
    // FIXME TODO XXX
    // how do we hook up notifations for epollfds?
    return FALSE;
    // old code:
//    if(process_isRunning(proc) && epollfd == proc->epollfd) {
//        return TRUE;
//    } else {
//        return FALSE;
//    }
}

Process* process_new(Host* host, guint processID, SimulationTime startTime,
                     SimulationTime stopTime, InterposeMethod interposeMethod,
                     const gchar* hostName, const gchar* pluginName,
                     const gchar* pluginPath, const gchar* pluginSymbol,
                     gchar** envv, gchar** argv) {
    Process* proc = g_new0(Process, 1);
    MAGIC_INIT(proc);

    proc->host = host;
    host_ref(proc->host);

    proc->processID = processID;

    /* plugin name and path are required so we know what to execute */
    utility_assert(pluginName);
    utility_assert(pluginPath);
    proc->plugin.exeName = g_string_new(pluginName);
    proc->plugin.exePath = g_string_new(pluginPath);

    proc->processName = g_string_new(NULL);
    g_string_printf(proc->processName, "%s.%s.%u",
            hostName,
            proc->plugin.exeName ? proc->plugin.exeName->str : "NULL",
            proc->processID);

    proc->cpuDelayTimer = g_timer_new();

    proc->startTime = startTime;
    proc->stopTime = stopTime;

    proc->interposeMethod = interposeMethod;

    /* save args and env */
    proc->argv = argv;
    proc->envv = envv;

    // We'll open these when the process starts.
    proc->stderrFD = -1;
    proc->stdoutFD = -1;

    proc->referenceCount = 1;

    worker_countObject(OBJECT_TYPE_PROCESS, COUNTER_TYPE_NEW);

    return proc;
}

static void _process_free(Process* proc) {
    MAGIC_ASSERT(proc);

    /* stop and free plugin memory if we are still running */
    if(proc->mainThread) {
        if(thread_isRunning(proc->mainThread)) {
            thread_terminate(proc->mainThread);
        }
        thread_unref(proc->mainThread);
        proc->mainThread = NULL;
    }

    if(proc->plugin.exePath) {
        g_string_free(proc->plugin.exePath, TRUE);
    }
    if(proc->plugin.exeName) {
        g_string_free(proc->plugin.exeName, TRUE);
    }
    if(proc->processName) {
        g_string_free(proc->processName, TRUE);
    }

    if(proc->argv) {
        g_strfreev(proc->argv);
    }
    if(proc->envv) {
        g_strfreev(proc->envv);
    }

    g_timer_destroy(proc->cpuDelayTimer);

    if (proc->host) {
        host_unref(proc->host);
    }

    if (proc->stderrFD >= 0) {
        close(proc->stderrFD);
    }
    if (proc->stdoutFD >= 0) {
        close(proc->stdoutFD);
    }

    worker_countObject(OBJECT_TYPE_PROCESS, COUNTER_TYPE_FREE);

    MAGIC_CLEAR(proc);
    g_free(proc);
}

void process_ref(Process* proc) {
    MAGIC_ASSERT(proc);
    (proc->referenceCount)++;
}

void process_unref(Process* proc) {
    MAGIC_ASSERT(proc);
    (proc->referenceCount)--;
    utility_assert(proc->referenceCount >= 0);
    if(proc->referenceCount == 0) {
        _process_free(proc);
    }
}

typedef struct _ProcessWaiter ProcessWaiter;
struct _ProcessWaiter {
    Thread* thread;
    Timer* timer;
    DescriptorListener* timerListener;
    Descriptor* descriptor;
    DescriptorListener* descriptorListener;
    gint referenceCount;
};

static void _process_unrefWaiter(ProcessWaiter* waiter) {
    waiter->referenceCount--;
    utility_assert(waiter->referenceCount >= 0);
    if (waiter->referenceCount == 0) {
        if (waiter->thread) {
            thread_unref(waiter->thread);
        }
        if (waiter->timer) {
            descriptor_unref((Descriptor*)waiter->timer);
        }
        if (waiter->descriptor) {
            descriptor_unref(waiter->descriptor);
        }
        free(waiter);
        worker_countObject(OBJECT_TYPE_PROCESS_WAITER, COUNTER_TYPE_FREE);
    }
}

#ifdef DEBUG
static void _process_logListeningState(Process* proc, ProcessWaiter* waiter,
                                       gint started) {
    GString* string = g_string_new(NULL);

    g_string_append_printf(string, "Process %s thread %p %s listening for ",
                           process_getName(proc), waiter->thread,
                           started ? "started" : "stopped");

    if (waiter->descriptor) {
        g_string_append_printf(
            string, "status on descriptor %d%s",
            *descriptor_getHandleReference(waiter->descriptor),
            waiter->timer ? " and " : "");
    }
    if (waiter->timer) {
        struct itimerspec value = {0};
        utility_assert(timer_getTime(waiter->timer, &value) == 0);
        g_string_append_printf(string, "a timeout of %lu.%09lu seconds",
                               (unsigned long)value.it_value.tv_sec,
                               (unsigned long)value.it_value.tv_nsec);
    }

    debug("%s", string->str);

    g_string_free(string, TRUE);
}
#endif

static void _process_notifyStatusChanged(gpointer object, gpointer argument) {
    Process* proc = object;
    ProcessWaiter* waiter = argument;
    MAGIC_ASSERT(proc);

    const gchar* sysname = host_getName(proc->host);

#ifdef DEBUG
    _process_logListeningState(proc, waiter, 0);
#endif

    /* Unregister both listeners whenever either one triggers. */
    if (waiter->timer && waiter->timerListener) {
        descriptor_removeListener(
            (Descriptor*)waiter->timer, waiter->timerListener);
        descriptorlistener_setMonitorStatus(
            waiter->timerListener, DS_NONE, DLF_NEVER);
    }

    if (waiter->descriptor && waiter->descriptorListener) {
        descriptor_removeListener(
            waiter->descriptor, waiter->descriptorListener);
        descriptorlistener_setMonitorStatus(
            waiter->descriptorListener, DS_NONE, DLF_NEVER);
    }

    process_continue(proc, waiter->thread);

    /* Destroy the listeners, which will also unref and free the waiter. */
    if (waiter->timerListener) {
        descriptorlistener_unref(waiter->timerListener);
    }
    if (waiter->descriptorListener) {
        descriptorlistener_unref(waiter->descriptorListener);
    }
}

void process_listenForStatus(Process* proc, Thread* thread, Timer* timeout,
                             Descriptor* descriptor, DescriptorStatus status) {
    MAGIC_ASSERT(proc);

    if (!timeout && !descriptor) {
        return;
    }

    ProcessWaiter* waiter = malloc(sizeof(*waiter));

    *waiter = (ProcessWaiter){
        .thread = thread,
        .timer = timeout,
        .descriptor = descriptor,
    };

    /* The waiter will hold refs to these objects. */
    if (waiter->thread) {
        thread_ref(waiter->thread);
    }
    if (waiter->timer) {
        descriptor_ref(waiter->timer);
    }
    if (waiter->descriptor) {
        descriptor_ref(waiter->descriptor);
    }

    worker_countObject(OBJECT_TYPE_PROCESS_WAITER, COUNTER_TYPE_NEW);

    /* Now set up the listeners. */
    if (waiter->timer) {
        /* The timer is used for timeouts. */
        waiter->timerListener = descriptorlistener_new(
            _process_notifyStatusChanged, proc,
            (DescriptorStatusObjectFreeFunc)process_unref, waiter,
            (DescriptorStatusArgumentFreeFunc)_process_unrefWaiter);

        /* The listener holds refs to the process and waiter. */
        process_ref(proc);
        waiter->referenceCount++;

        /* The timer is readable when it expires */
        descriptorlistener_setMonitorStatus(
            waiter->timerListener, DS_READABLE, DLF_OFF_TO_ON);

        /* Attach the listener to the timer. */
        descriptor_addListener(
            (Descriptor*)waiter->timer, waiter->timerListener);
    }

    if (waiter->descriptor) {
        /* We listen for status change on the descriptor. */
        waiter->descriptorListener = descriptorlistener_new(
            _process_notifyStatusChanged, proc,
            (DescriptorStatusObjectFreeFunc)process_unref, waiter,
            (DescriptorStatusArgumentFreeFunc)_process_unrefWaiter);

        /* The listener holds refs to the process and waiter. */
        process_ref(proc);
        waiter->referenceCount++;

        /* Monitor the requested status. */
        descriptorlistener_setMonitorStatus(
            waiter->descriptorListener, status, DLF_OFF_TO_ON);

        /* Attach the listener to the descriptor. */
        descriptor_addListener(waiter->descriptor, waiter->descriptorListener);
    }

#ifdef DEBUG
    _process_logListeningState(proc, waiter, 1);
#endif
}
