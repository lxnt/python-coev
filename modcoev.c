#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "pythread.h"

#include <sys/types.h>
#include <time.h>

#include "ucoev.h"

#define COEV_MODULE
#include "modcoev.h"


/** version 0.5 - no explicit coroutine type.
    switch, wait and friends operate on thread-ids.
    
    scheduler control functions,
    python bindings for libucoev's cnrbuf_t 
**/

static int debug_flag;

static time_t start_time;

/* require Python between 2.5 and 2.6 for the time being */
#if (PY_VERSION_HEX < 0x02050000)
#error Python version >= 2.5 is required.
#endif
#if (PY_VERSION_HEX >= 0x03000000)
#error Python version < 3.0 is required.
#endif

/* define some 2.6 stuff that is missing from 2.5 */
#ifndef Py_TYPE
#define Py_TYPE(ob)                (((PyObject*)(ob))->ob_type)
#endif
#ifndef PyVarObject_HEAD_INIT
#define PyVarObject_HEAD_INIT(type, size) PyObject_HEAD_INIT(type) size,
#endif


static struct _const_def { 
    const char *name;
    int value;
} _const_tab[] = {
    { "READ", COEV_READ },
    { "WRITE", COEV_WRITE },
    { "CDF_COEV", CDF_COEV },
    { "CDF_COEV_DUMP", CDF_COEV_DUMP},
    { "CDF_RUNQ_DUMP", CDF_RUNQ_DUMP},
    { "CDF_NBUF", CDF_NBUF},
    { "CDF_NBUF_DUMP", CDF_NBUF_DUMP},
    { "CDF_COLOCK", CDF_COLOCK},
    { "CDF_COLOCK_DUMP", CDF_COLOCK_DUMP },
    { "CDF_COLB_DUMP", CDF_COLB_DUMP },
    { "CDF_STACK", CDF_STACK},
    { "CDF_STACK_DUMP", CDF_STACK_DUMP },
    { "CDF_CB_ON_NEW_DUMP", CDF_CB_ON_NEW_DUMP },
    { 0 }
};


static PyObject* PyExc_CoroError;
static PyObject* PyExc_CoroExit;
static PyObject* PyExc_CoroTimeout;

static PyObject* PyExc_CoroWaitAbort;

static PyObject* PyExc_CoroNoScheduler;
static PyObject* PyExc_CoroTargetSelf;
static PyObject* PyExc_CoroTargetDead;
static PyObject* PyExc_CoroTargetBusy;

static PyObject* PyExc_CoroSocketError;

static struct _exc_def {
    PyObject **exc;
    PyObject **parent;
    char *name;
    char *shortname;
    const char *doc;
} _exc_tab[] = {
    {
        &PyExc_CoroError, &PyExc_Exception,
        "coev.Error", "Error",
        "unspecified coroutine error"
    },
    {
        &PyExc_CoroExit, &PyExc_Exception,
        "coev.Exit", "Exit",
        "child coroutine was forced to exit"
    },
    {
        &PyExc_CoroWaitAbort, &PyExc_CoroError,
        "coev.WaitAbort", "WaitAbort",
        "unscheduled switch into waiting or sleeping coroutine (unused as of now)"
    },
    {
        &PyExc_CoroTimeout, &PyExc_CoroError,
        "coev.Timeout", "Timeout",
        "timeout on wait"
    },
    {
        &PyExc_CoroNoScheduler, &PyExc_CoroError,
        "coev.NoScheduler", "NoScheduler",
        "requested operation requires active scheduler"
    },
    {
        &PyExc_CoroTargetSelf, &PyExc_CoroError,
        "coev.TargetSelf", "TargetSelf",
        "switch to self attempted"
    },
    {
        &PyExc_CoroTargetDead, &PyExc_CoroError,
        "coev.TargetDead", "TargetDead",
        "switch to or scheduling of dead coroutine attempted"
    },
    {
        &PyExc_CoroTargetBusy, &PyExc_CoroError,
        "coev.TargetBusy", "TargetBusy",
        "switch to or scheduling of a coroutine with an active event watcher attempted\n"
    },
    {
        &PyExc_CoroSocketError, &PyExc_IOError,
        "coev.SocketError", "SocketError",
        "ask Captain Obvious\n"
    },
    
    { 0 }
};

#define coro_dprintf(fmt, args...) do { if (debug_flag) \
    coev_dmprintf(fmt, ## args); } while(0)

static PyObject *mod_switch_bottom_half(void);

PyDoc_STRVAR(mod_switch_doc,
"switch(thread_id, *args)\n\
\n\
Switch execution to coroutine identified by thread_id.\n\
\n\
If this coroutine has never been run, then it will\n\
start execution.\n\
\n\
If the coroutine is active (has been run, but was switch()'ed\n\
out before leaving its run function), then it will be resumed\n\
and the return value of its switch call will be:\n\
 - None if no arguments are passed\n\
 - the given argument if is single\n\
 - a tuple if multiple arguments are passed\n\
\n\
If the coroutine is dead, or is the current coroutine, appropriate\n\
exception is raised.");

static PyObject* 
mod_switch(PyObject *a, PyObject* args) {
    PyObject *arg = NULL;
    long target_id;
    coev_t *target;
    
    if (!PyArg_ParseTuple(args, "l|O", &target_id, &arg))
	return NULL;
    
    coro_dprintf("coev.switch(): target_id %ld object %p\n", target_id, arg);
    
    target = (coev_t *) target_id;
    
    /* Release old arg, put new one in place. */
    Py_XDECREF(target->A);
    
    if (arg == NULL) {
        arg = Py_None;
        Py_INCREF(arg);
    }
    target->A = arg;

    coro_dprintf("coro_switch: current [%s] target [%s] arg %p \n", 
        coev_treepos(coev_current()),
        coev_treepos(target), arg);

    Py_BEGIN_ALLOW_THREADS
    coev_switch(target);
    Py_END_ALLOW_THREADS
    
    return mod_switch_bottom_half();
}

/* lower part common to mod_switch(), mod_throw(), mod_stall() */
static PyObject *
mod_switch_bottom_half(void) {
    PyObject *result;
    
    coev_t *dead_meat = NULL, *self;
    
    self = coev_current();
    
    coro_dprintf("coro_switch: current [%s] origin [%s] switch() returned\n",
        coev_treepos(self), coev_treepos(self->origin) );
    
    coro_dprintf("coro_switch: current [%s] state=%s status=%s args=%p\n", 
        coev_treepos(self), coev_state(self), 
        coev_status(self), self->A);
    
    if (self->origin)
        coro_dprintf("coro_switch: origin [%s] state=%s\n", 
            coev_treepos(self->origin), coev_state(self->origin));
    
    switch (self->status) {
        case CSW_SIGCHLD:
            /* uncaught exception */
            dead_meat = self->origin;
            if (dead_meat->state != CSTATE_DEAD) {
                coev_dmflush();
                Py_FatalError("CSW_SIGCHLD, but dead_meat->state != CSTATE_DEAD");
            }
            
            /* drop anything previuosly referenced */
            Py_XDECREF(self->A);
            Py_XDECREF(self->X);
            Py_XDECREF(self->Y);
            Py_XDECREF(self->S);
            
            /* steal references */
            self->A = dead_meat->A; dead_meat->A = NULL;
            self->X = dead_meat->X; dead_meat->X = NULL;
            self->Y = dead_meat->Y; dead_meat->Y = NULL;
            self->S = dead_meat->S; dead_meat->S = NULL;
            
        case CSW_VOLUNTARY:
            if (self->A != NULL) {
                /* regular switch or normal return */
                result = self->A; self->A = NULL; /* steal reference */
                return result;
            } else {
                /* exception injection */
                if (self->X == NULL) {
                    /* exit was forced.*/
                    PyObject *thread_id;
                    if (self->origin == NULL)
                        Py_FatalError("death by SystemExit, but origin not set.\n");
                    
                    /* thread-id convention is used here. */
                    thread_id = PyInt_FromLong((long)self->origin);
                    PyErr_SetObject(PyExc_CoroExit, thread_id);
                    Py_DECREF(thread_id);
                    return NULL;
                } else {
                    PyErr_Restore(self->X, self->Y, self->S);
                }
                self->X = self->Y = self->S = NULL; 
                return NULL;
            }
            
        case CSW_SCHEDULER_NEEDED:
        case CSW_YOURTURN:
            Py_CLEAR(self->A);
            Py_RETURN_NONE;
        
        case CSW_TARGET_SELF:
            Py_CLEAR(self->A);
            PyErr_SetString(PyExc_CoroError,
		    "switch(): attempt to switch to self");
            return NULL;
        
        case CSW_NONE:      /* should be unpossible */
        case CSW_EVENT:     /* should only be seen in coev_scheduled_switch(), not here. */
        case CSW_WAKEUP:    /* same. */
        case CSW_TIMEOUT:   /* same. */
        default:
            Py_CLEAR(self->A);
            PyErr_Format(PyExc_CoroError,
		    "switch(): unexpected switchback type %d", self->status);
            return NULL;
    }
}

PyDoc_STRVAR(mod_throw_doc,
"throw(id, typ[,val[,tb]]) -> raise exception in coroutine, return value passed "
"when switching back");
/** This switches to a coroutine with A=NULL, X=type Y=value S=traceback. */
static PyObject* 
mod_throw(PyObject *a, PyObject* args) {
    long target_id;
    coev_t *target;
    PyObject *typ = PyExc_SystemExit;
    PyObject *val = NULL;
    PyObject *tb = NULL;
       
    if (!PyArg_ParseTuple(args, "l|OOO:throw", &target_id, &typ, &val, &tb))
        return NULL;

    target = (coev_t *) target_id;
    
    Py_INCREF(typ);
    Py_XINCREF(val);
    Py_XINCREF(tb);

    coro_dprintf("coro_throw: current [%s] target [%s]\n", 
        coev_treepos(coev_current()),
        coev_treepos(target));

    
    if (PyExceptionClass_Check(typ)) {
        PyErr_NormalizeException(&typ, &val, &tb);
    } else if (PyExceptionInstance_Check(typ)) {
        /* Raising an instance.  The value should be a dummy. */
        if (val && val != Py_None) {
            PyErr_SetString(PyExc_TypeError,
              "instance exception may not have a separate value");
            goto failed_throw;
        } else {
            /* Normalize to raise <class>, <instance> */
            Py_XDECREF(val);
            val = typ;
            typ = PyExceptionInstance_Class(typ);
            Py_INCREF(typ);
        }
    } else {
        /* Not something one can raise. throw() fails. */
        PyErr_Format(PyExc_TypeError,
                     "exceptions must be classes, or instances, not %s",
                     typ->ob_type->tp_name);
        goto failed_throw;
    }

    Py_CLEAR(target->A);
    target->X = typ;
    target->Y = val;
    target->S = tb;
    
    Py_BEGIN_ALLOW_THREADS
    coev_switch(target);
    Py_END_ALLOW_THREADS
    
    return mod_switch_bottom_half();

failed_throw:
    /* Didn't use our arguments, so restore their original refcounts */
    Py_DECREF(typ);
    Py_XDECREF(val);
    return NULL;
}

PyDoc_STRVAR(mod_stall_doc,
"stall()\n\
\n\
Stall execution of current coroutine until next runqueue pass.\n\
");

static PyObject* 
mod_stall(PyObject *a, PyObject* args) {
    int rv;
    coro_dprintf("coev.stall(): current [%s]\n", 
        coev_treepos(coev_current()));

    Py_BEGIN_ALLOW_THREADS
    rv = coev_stall();
    Py_END_ALLOW_THREADS
    
    if ((rv != 0 ) || (coev_current()->status == CSW_SCHEDULER_NEEDED)) {
        PyErr_SetNone(PyExc_CoroNoScheduler);
        return NULL;
    }
    return mod_switch_bottom_half();
}

PyDoc_STRVAR(mod_switch2scheduler_doc,
"switch2scheduler()\n\
\n\
Switch to scheduler.\n\
");

static PyObject* 
mod_switch2scheduler(PyObject *a, PyObject* args) {
    int rv;
    coro_dprintf("coev.switch2scheduler(): current [%s]\n", 
        coev_treepos(coev_current()));

    Py_BEGIN_ALLOW_THREADS
    rv = coev_switch2scheduler();
    Py_END_ALLOW_THREADS
    
    if ((rv != 0 ) || (coev_current()->status == CSW_SCHEDULER_NEEDED)) {
        PyErr_SetNone(PyExc_CoroNoScheduler);
        return NULL;
    }
    return mod_switch_bottom_half();
}

PyDoc_STRVAR(mod_getpos_doc,
"getpos([id]) -> str \n\
  returns treepos for given or current coro.");

static PyObject* 
mod_getpos(PyObject *a, PyObject* args) {
    long target_id = 0;
    coev_t *target = coev_current();
    
    if (!PyArg_ParseTuple(args, "|l", &target_id))
	return NULL;
    if (target_id)
        target = (coev_t *) target_id;
    return PyString_FromString(coev_treepos(target));
}

/** coev.socketfile - file-like interface to a network socket */

typedef struct {
    PyObject_HEAD
    cnrbuf_t dabuf;
    int busy;
    coev_t *owner;
    int eof;
} CoroSocketFile;

PyDoc_STRVAR(socketfile_doc,
"socketfile(fd, timeout, rlim) -> socketfile object\n\n\
Coroutine-aware file-like interface to network sockets.\n\n\
fd -- integer fd to wrap around.\n\
timeout -- float timeout per IO operation.\n\
rlim -- read buffer size soft limit. also limits readline.\n\
        is reset if read or readline explicitly request more space.\n\
        is here to prevent runaway buffer growth due to unfortunate\n\
        readline call without size hint (exception is raised in this case).\n\
");

static PyObject *
socketfile_new(PyTypeObject *type, PyObject *args, PyObject *kw) {
    CoroSocketFile *self;
    static char *kwds[] = {  "fd", "timeout", "rlim", NULL };
    int fd;
    Py_ssize_t rlim;
    double iop_timeout;

    self = (CoroSocketFile *)type->tp_alloc(type, 0);
    if (self == NULL)
        return NULL;
    
    if (!PyArg_ParseTupleAndKeywords(args, kw, "idn", kwds,
	    &fd, &iop_timeout, &rlim)) {
	Py_DECREF(self);
	return NULL;
    }
    if (rlim <= 0) {
	PyErr_SetString(PyExc_ValueError, "Read buffer limit must be positive");
	Py_DECREF(self);
	return NULL;
    }
    
    cnrbuf_init(&self->dabuf, fd, iop_timeout, 4096, rlim);
    self->busy = 0;
    return (PyObject *)self;
}

static void
socketfile_dealloc(CoroSocketFile *self) {
    cnrbuf_fini(&self->dabuf);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *mod_wait_bottom_half(void);
static PyObject *sf_empty_string = NULL;

#define RETURN_EMPTYSTRING_IF(cond) do { if((cond)) { Py_INCREF(sf_empty_string); return sf_empty_string; } } while (0)

PyDoc_STRVAR(socketfile_read_doc,
"read([size]) -> bytestr\n\n\
Read at most size bytes or return whatever there is in buffers (all of in-process and up to 8K from the kernel).\n\
size -- size to read.\n\
");
static PyObject * 
socketfile_read(CoroSocketFile *self, PyObject* args) {
    Py_ssize_t rv, sizehint = 0;
    void *p;
    
    if (self->busy)
        return PyErr_Format(PyExc_CoroError, "socketfile is busy; owner=[%s] accessor=[%s]",
            self->owner ? self->owner->treepos : "(nil?)",
            coev_current()->treepos ), NULL;
    
    if (!PyArg_ParseTuple(args, "|n", &sizehint ))
	return NULL;

    
    RETURN_EMPTYSTRING_IF(self->eof);
    
    self->busy = 1;
    self->owner = coev_current();
    Py_BEGIN_ALLOW_THREADS
    rv = cnrbuf_read(&self->dabuf, &p, sizehint);
    Py_END_ALLOW_THREADS    
    self->busy = 0;
    
    if (rv == -1)
        return PyErr_SetFromErrno(PyExc_CoroSocketError);
    
    if (rv == 0)
        RETURN_EMPTYSTRING_IF((self->eof = 1));
    
    return PyString_FromStringAndSize(p, rv);
}


PyDoc_STRVAR(socketfile_readline_doc,
"readline([sizehint]) -> str\n\n\
Read at most size bytes or until LF or EOF are reached.\n\
When size is not given, initialization-supplied limit is used.\n\
");
static PyObject* 
socketfile_readline(CoroSocketFile *self, PyObject* args) {
    Py_ssize_t rv, sizehint = 0;
    void *p;
    
    if (self->busy)
        return PyErr_Format(PyExc_CoroError, "socketfile is busy; owner=[%s] accessor=[%s]",
            self->owner ? self->owner->treepos : "(nil?)",
            coev_current()->treepos), NULL;
    
    if (!PyArg_ParseTuple(args, "|n", &sizehint ))
	return NULL;

    RETURN_EMPTYSTRING_IF(self->eof);
    
    self->busy = 1;
    self->owner = coev_current();
    Py_BEGIN_ALLOW_THREADS
    rv = cnrbuf_readline(&self->dabuf, &p, sizehint);
    Py_END_ALLOW_THREADS
    self->busy = 0;
    
    if (rv == -1) {
        coro_dprintf("socketfile_readline(): setting exception errno=%s\n", strerror(errno));
        return PyErr_SetFromErrno(PyExc_CoroSocketError);
    }
    
    if (rv == 0) {
        coro_dprintf("socketfile_readline(): EOF, returning empty string.\n");
        RETURN_EMPTYSTRING_IF((self->eof = 1));
    }
    
    coro_dprintf("socketfile_readline(): returning %d bytes\n", rv);
    return PyString_FromStringAndSize(p, rv);
}

PyDoc_STRVAR(socketfile_write_doc,
"write(str) -> None\n\n\
Write the string to the fd. EPIPE results in an exception.\n\
");
static PyObject * 
socketfile_write(CoroSocketFile *self, PyObject* args) {
    const char *str;
    Py_ssize_t rv, len, written;

    if (self->busy)
        return PyErr_Format(PyExc_CoroError, "socketfile is busy; owner=[%s] accessor=[%s]",
            self->owner ? self->owner->treepos : "(nil?)",
            coev_current()->treepos), NULL;
    
    if (!PyArg_ParseTuple(args, "s#", &str, &len))
	return NULL;

    self->busy = 1;
    self->owner = coev_current();
    Py_BEGIN_ALLOW_THREADS
    rv = coev_send(self->dabuf.fd, str, len, &written, self->dabuf.iop_timeout);
    Py_END_ALLOW_THREADS
    self->busy = 0;
    
    if (rv == -1)
        return PyErr_SetFromErrno(PyExc_CoroSocketError);
    
    return PyInt_FromSsize_t(rv);
}

PyDoc_STRVAR(socketfile_flush_doc,
"flush() -> None\n\n\
Noop.\n\
");

PyDoc_STRVAR(socketfile_close_doc,
"close() -> None\n\n\
Noop.\n\
");

static PyObject *
socketfile_noop(PyObject *self) {
    Py_RETURN_NONE;
}

static PyMethodDef socketfile_methods[] = {
    {"read",  (PyCFunction) socketfile_read,  METH_VARARGS, socketfile_read_doc},
    {"readline", (PyCFunction) socketfile_readline, METH_VARARGS, socketfile_readline_doc},
    {"write", (PyCFunction) socketfile_write, METH_VARARGS, socketfile_write_doc},
    {"flush", (PyCFunction) socketfile_noop, METH_NOARGS, socketfile_flush_doc},
    {"close", (PyCFunction) socketfile_noop, METH_NOARGS, socketfile_close_doc},
    { 0 }
};

static PyTypeObject CoroSocketFile_Type = {
    PyObject_HEAD_INIT(NULL)
    /* ob_size           */ 0,
    /* tp_name           */ "coev.socketfile",
    /* tp_basicsize      */ sizeof(CoroSocketFile),
    /* tp_itemsize       */ 0,
    /* tp_dealloc        */ (destructor)socketfile_dealloc,
    /* tp_print          */ 0,
    /* tp_getattr        */ 0,
    /* tp_setattr        */ 0,
    /* tp_compare        */ 0,
    /* tp_repr           */ 0,
    /* tp_as_number      */ 0,
    /* tp_as_sequence    */ 0,
    /* tp_as_mapping     */ 0,
    /* tp_hash           */ 0,
    /* tp_call           */ 0,
    /* tp_str            */ 0,
    /* tp_getattro       */ 0,
    /* tp_setattro       */ 0,
    /* tp_as_buffer      */ 0,
    /* tp_flags          */ Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    /* tp_doc            */ socketfile_doc,
    /* tp_traverse       */ 0,
    /* tp_clear          */ 0,
    /* tp_richcompare    */ 0,
    /* tp_weaklistoffset */ 0,
    /* tp_iter           */ 0,
    /* tp_iternext       */ 0,
    /* tp_methods        */ socketfile_methods,
    /* tp_members        */ 0,
    /* tp_getset         */ 0,
    /* tp_base           */ 0,
    /* tp_dict           */ 0,
    /* tp_descr_get      */ 0,
    /* tp_descr_set      */ 0,
    /* tp_dictoffset     */ 0,
    /* tp_init           */ 0,
    /* tp_alloc          */ 0,
    /* tp_new            */ socketfile_new
};

/** Module definition */
/* FIXME: wait/sleep can possibly leak reference to passed-in value */
/* FIXME: remember WTH I was thinking when I wrote the above */

PyDoc_STRVAR(mod_wait_doc,
"wait(fd, events, timeout) -> None\n\n\
Switch to scheduler until requested IO event or timeout happens.\n\
fd -- file descriptor (integer).\n\
revents -- bitmask.\n\
timeout -- in seconds.");

static PyObject *
mod_wait(PyObject *a, PyObject* args) {
    int fd, revents;
    double timeout;
    
    if (!PyArg_ParseTuple(args, "iid", &fd, &revents, &timeout))
	return NULL;
    
    Py_BEGIN_ALLOW_THREADS
    coev_wait(fd, revents, timeout);
    Py_END_ALLOW_THREADS
    
    return mod_wait_bottom_half();
}

static PyObject *
mod_wait_bottom_half(void) {
    coev_t *cur;

    cur = coev_current();
    coro_dprintf("mod_wait_bottom_half(): entered. [%s] %s\n", 
        coev_treepos(cur), coev_status(cur));
    
    switch (cur->status) {
        case CSW_EVENT:
	case CSW_WAKEUP:
            Py_RETURN_NONE;
        case CSW_SIGCHLD:
        {
            coev_t *dead_meat = cur->origin;
            
            coro_dprintf("mod_wait_bottom_half(): currnt=[%s] dead_meat=[%s] args=%p\n",
                coev_treepos(cur), coev_treepos(dead_meat), dead_meat->A);
            
            PyErr_Format(PyExc_CoroWaitAbort, "SIGCHLD from [%s] ", coev_treepos(dead_meat));
            Py_CLEAR(dead_meat->A);
	    if (!dead_meat->id)
		Py_FatalError("SIGCHLD from root coro: unpossible.");
	    cur->status = CSW_VOLUNTARY; /* SIGCHLD handled. */
            return NULL;
        }
        case CSW_VOLUNTARY:
            /* raise volswitch exception */
            PyErr_SetString(PyExc_CoroWaitAbort,
		    "voluntary switch into waiting coroutine");        
            return NULL;
        case CSW_TIMEOUT:
            /* raise timeout exception */
            PyErr_SetString(PyExc_CoroTimeout,
		    "IO timeout");        
            return NULL;
     
        case CSW_TARGET_DEAD:
            PyErr_SetNone(PyExc_CoroTargetDead);
            return NULL;
        case CSW_TARGET_BUSY:
            PyErr_SetNone(PyExc_CoroTargetBusy);
            return NULL;
	case CSW_TARGET_SELF:
            PyErr_SetNone(PyExc_CoroTargetSelf);
            return NULL;
        case CSW_SCHEDULER_NEEDED:
            PyErr_SetNone(PyExc_CoroNoScheduler);
            return NULL;
            
        default:
            coro_dprintf("wait(): unknown switchback type %d\n", cur->status);
            PyErr_SetString(PyExc_CoroError,
		    "wait(): unknown switchback type");
            return NULL;
    }
}

PyDoc_STRVAR(mod_sleep_doc,
"sleep(amount) -> None\n\n\
Switch to scheduler until at least amount seconds pass.\n\
amount -- number of seconds to sleep.");

static PyObject *
mod_sleep(PyObject *a, PyObject *args) {
    double timeout;
    
    if (!PyArg_ParseTuple(args, "d", &timeout))
	return NULL;

    Py_BEGIN_ALLOW_THREADS
    coev_sleep(timeout);
    Py_END_ALLOW_THREADS
        
    
    return mod_wait_bottom_half();
}

PyDoc_STRVAR(mod_schedule_doc,
"schedule([coroutine], [args]) -> switch rv\n\n\
Schedule given coroutine (or self) for execution on \n\
next runqueue pass.\n\
If current coroutine, no argument, or None is given, then \n\
switch to scheduler after adding itself to the runqueue.\n\n\
coroutine -- a non-dead coroutine\n\
args -- a tuple to pass to it \n\
");

static PyObject *
mod_schedule(PyObject *a, PyObject *args) {
    coev_t *target, *current;
    PyObject *argstuple;
    int rv;
    long target_id = 0;

    current = target = coev_current();
    argstuple = PyTuple_Pack(1, Py_None);

    if (!PyArg_ParseTuple(args, "|lO!", &target_id, &PyTuple_Type, &argstuple))
	return NULL;
    
    target = (coev_t *)target_id;
    current = coev_current();
    
    Py_INCREF(argstuple);
    Py_CLEAR(target->A);
    target->A = argstuple;        
    
    if (target == current) {
        Py_BEGIN_ALLOW_THREADS
        rv = coev_schedule(target);
        Py_END_ALLOW_THREADS
        if (!rv)
            return mod_wait_bottom_half();
    } else {
        rv = coev_schedule(target);
    }
     
    switch(rv) {
        case CSCHED_NOERROR:
            Py_RETURN_NONE;
        case CSCHED_DEADMEAT:
            PyErr_SetString(PyExc_CoroError, "target is dead.");
            break;
        case CSCHED_ALREADY:
            PyErr_SetString(PyExc_CoroError, "target is already scheduled.");
            break;
        case CSCHED_NOSCHEDULER:
            PyErr_SetString(PyExc_CoroError, "target is self, but no scheduler in vicinity.");
            break;
        default:
            PyErr_SetString(PyExc_CoroError, "unknown coev_schedule return value.");
            break;
    }
    return NULL;
}

PyDoc_STRVAR(mod_scheduler_doc,
"scheduler() -> None\n\n\
Run scheduler: dispatch pending IO or timer events");

static PyObject *
mod_scheduler(PyObject *a, PyObject *b) {
    coev_t *sched;
    coro_dprintf("coev.scheduler(): calling coev_loop() (cur=[%s]).\n", 
        coev_current()->treepos);
    Py_BEGIN_ALLOW_THREADS
    sched = coev_loop();
    Py_END_ALLOW_THREADS    
    
    /* this returns if: 
         - an ev_unloop() has been called by coev_unloop() or in interrupt handler. 
         - there's nothing left to schedule
         - there already is a scheduler.
    */
    if (sched != NULL) {
        coro_dprintf("coev.scheduler(): [%s] is already scheduling. (cur=[%s]).\n", 
            sched->treepos, coev_current()->treepos);
        PyErr_Format(PyExc_CoroError, "coev.scheduler(): [%s] is already scheduling. (cur=[%s])",
            sched->treepos, coev_current()->treepos);
        return NULL;
    }
        
    coro_dprintf("coev.scheduler(): coev_loop() returned %p (cur=[%s]).\n", 
        sched, coev_current()->treepos);
    /* return NULL iff this module set up an exception. */
    if (PyErr_Occurred() != NULL)
        return NULL;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(mod_current_doc,
"current() -> coroutine\n\n\
Return ID of currently executing coroutine object");

static PyObject* 
mod_current(PyObject *a, PyObject *b) {
    return PyInt_FromLong( ((long)coev_current()) );
}

static int
_add_K_to_dict(PyObject *dick, const char *key, uint64_t val) {
    PyObject *pyval;
    int rv = -1;
    pyval = Py_BuildValue("K", val);
    if (pyval) {
        if ( PyDict_SetItemString(dick, key, pyval) == 0 )
            rv = 0;
        else {
            Py_DECREF(dick);
            rv = -1;
        }
        Py_DECREF(pyval);
    }
    return rv;
}

PyDoc_STRVAR(mod_stats_doc,
"stats() -> {...}\n\n\
Returns a dict with instrumentation");

static PyObject *
mod_stats(PyObject *a, PyObject *b) {
    PyObject *dick;
    coev_instrumentation_t i;
    
    coev_getstats(&i);
    
    dick = PyDict_New();
    if (!dick)
        return NULL;

    if (_add_K_to_dict(dick, "c_ctxswaps", i.c_ctxswaps)) return NULL;
    if (_add_K_to_dict(dick, "c_switches", i.c_switches)) return NULL;
    if (_add_K_to_dict(dick, "c_waits", i.c_waits)) return NULL;
    if (_add_K_to_dict(dick, "c_sleeps", i.c_sleeps)) return NULL;
    if (_add_K_to_dict(dick, "c_stalls", i.c_stalls)) return NULL;
    if (_add_K_to_dict(dick, "c_runqruns", i.c_runqruns)) return NULL;
    if (_add_K_to_dict(dick, "c_news", i.c_news)) return NULL;
    if (_add_K_to_dict(dick, "stacks.allocated", i.stacks_allocated)) return NULL;
    if (_add_K_to_dict(dick, "stacks.used", i.stacks_used)) return NULL;
    if (_add_K_to_dict(dick, "cnrbufs.allocated", i.cnrbufs_allocated)) return NULL;
    if (_add_K_to_dict(dick, "cnrbufs.used", i.cnrbufs_used)) return NULL;
    if (_add_K_to_dict(dick, "coevs.allocated", i.coevs_allocated)) return NULL;
    if (_add_K_to_dict(dick, "coevs.used", i.coevs_used)) return NULL;
    if (_add_K_to_dict(dick, "coevs.waiting", i.waiters)) return NULL;
    if (_add_K_to_dict(dick, "coevs.slacking", i.slackers)) return NULL;  
    if (_add_K_to_dict(dick, "coevs.on_lock", i.coevs_on_lock)) return NULL;  
    if (_add_K_to_dict(dick, "locks.allocated", i.colocks_allocated)) return NULL;
    if (_add_K_to_dict(dick, "locks.used", i.colocks_used)) return NULL;
    if (_add_K_to_dict(dick, "locks.c_acquires", i.c_lock_acquires)) return NULL;
    if (_add_K_to_dict(dick, "locks.c_acfails", i.c_lock_acfails)) return NULL;
    if (_add_K_to_dict(dick, "locks.c_waits", i.c_lock_waits)) return NULL;
    if (_add_K_to_dict(dick, "locks.c_releases", i.c_lock_releases)) return NULL;

    return dick;
}

PyDoc_STRVAR(mod_setdebug_doc,
"setdebug([module=False, [library=0]) -> \n\n\
module -- enable module-level debug output.\n\
library -- enable library-level debug output, see CDF_* constants \n");

static PyObject *
mod_setdebug(PyObject *a, PyObject *args, PyObject *kwargs) {
    static char *kwds[] = { "module", "library", 0 };
    int module = 0;
    int library = 0;
    
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, 
            "|ii:setdebug", kwds, &module, &library))    
	return NULL;
    debug_flag = module;
    coro_dprintf("mod_setdebug(%x,%x)\n", debug_flag, library);
    coev_setdebug(library);
    
    Py_RETURN_NONE;
}

static PyMethodDef CoevMethods[] = {
    {   "current", mod_current, METH_NOARGS, mod_current_doc },
    {   "switch", mod_switch, METH_VARARGS, mod_switch_doc },
    {   "throw", mod_throw, METH_VARARGS, mod_throw_doc },
    {   "wait", mod_wait, METH_VARARGS, mod_wait_doc },
    {   "sleep", mod_sleep, METH_VARARGS, mod_sleep_doc },
    {   "stall", mod_stall, METH_NOARGS, mod_stall_doc },
    {   "switch2scheduler", mod_switch2scheduler, METH_NOARGS, mod_switch2scheduler_doc },
    {   "schedule", mod_schedule, METH_VARARGS, mod_schedule_doc},
    {   "scheduler", mod_scheduler, METH_NOARGS, mod_scheduler_doc },
    {   "stats", mod_stats, METH_NOARGS, mod_stats_doc },
    {   "setdebug", (PyCFunction)mod_setdebug,
        METH_VARARGS | METH_KEYWORDS, mod_setdebug_doc },
    {   "getpos", mod_getpos, METH_VARARGS, mod_getpos_doc},
        
    { 0 }
};


void
init_coev(void) {
    PyObject* m;
    static void *PyCoev_API[PyCoev_API_pointers];
    PyObject *c_api_object;

    m = Py_InitModule("_coev", CoevMethods);
    if (m == NULL)
        return;
    
    if (PyModule_AddStringConstant(m, "__version__", "0.5") < 0)
        return;
    
    { /* add constants */
        int i;
        for (i = 0; _const_tab[i].name; i++) 
            if (PyModule_AddIntConstant(m,  _const_tab[i].name,  _const_tab[i].value) < 0)
                return;
    }
    
    if (PyType_Ready(&CoroSocketFile_Type) < 0)
        return;

    { /* add exceptions */
        PyObject* exc_obj;
        PyObject* exc_dict;
        PyObject* exc_doc;
        int i, e;
        
        for (i = 0; _exc_tab[i].exc; i++) {
            if (!(exc_dict = PyDict_New()))
                return;
            if (!(exc_doc = PyString_FromString(_exc_tab[i].doc))) {
                Py_DECREF(exc_dict);
                return;
            }
            e = PyDict_SetItemString(exc_dict, "__doc__", exc_doc);
            Py_DECREF(exc_doc);
            if (e == -1) {
                Py_DECREF(exc_dict);
                return;
            }
            
            *(_exc_tab[i].exc) = PyErr_NewException(_exc_tab[i].name, *(_exc_tab[i].parent), exc_dict);
            Py_DECREF(exc_dict);
            
            exc_obj = *(_exc_tab[i].exc);
            
            if (exc_obj == NULL)
                return;
            
            Py_INCREF(exc_obj);
            PyModule_AddObject(m, _exc_tab[i].shortname, exc_obj);

        }
    }
    start_time = time(NULL);
    
    sf_empty_string = PyString_FromStringAndSize("", 0);
    Py_INCREF(sf_empty_string);
    
    Py_INCREF(&CoroSocketFile_Type);
    PyModule_AddObject(m, "socketfile", (PyObject*) &CoroSocketFile_Type);
    
     /* Initialize the C API pointer array */
    PyCoev_API[PyCoev_wait_bottom_half_NUM] = (void *)mod_wait_bottom_half;
    
    /* Create a CObject containing the API pointer array's address */
    c_api_object = PyCObject_FromVoidPtr((void *)PyCoev_API, NULL);

    if (c_api_object != NULL)
        PyModule_AddObject(m, "_C_API", c_api_object);
    
    /* force libcoev initialization */
    { 
        PyThread_type_lock l;
        
        l = PyThread_allocate_lock();
        PyThread_release_lock(l);
    }
    
    Py_AtExit(coev_dmflush);
}
