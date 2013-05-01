#ifndef Py_MODCOEV_H
#define Py_MODCOEV_H
#ifdef __cplusplus
extern "C" {
#endif

/* Header file for modcoev */

/* C API functions */
#define PyCoev_wait_bottom_half_NUM 0
#define PyCoev_wait_bottom_half_RETURN PyObject *
#define PyCoev_wait_bottom_half_PROTO (void)

/* Total number of C API pointers */
#define PyCoev_API_pointers 1

#ifndef COEV_MODULE
/* This section is used in modules that use spammodule's API */

static void **PyCoev_API;

#define PyCoev_wait_bottom_half \
 (*(PyCoev_wait_bottom_half_RETURN (*)PyCoev_wait_bottom_half_PROTO) \
    PyCoev_API[PyCoev_wait_bottom_half_NUM])

/* Return -1 and set exception on error, 0 on success. */
static int
import_coev(void)
{
    PyObject *module = PyImport_ImportModule("coev");

    if (module != NULL) {
        PyObject *c_api_object = PyObject_GetAttrString(module, "_C_API");
        if (c_api_object == NULL)
            return -1;
        if (PyCObject_Check(c_api_object))
            PySpam_API = (void **)PyCObject_AsVoidPtr(c_api_object);
        Py_DECREF(c_api_object);
    }
    return 0;
}

#endif /* !defined(COEV_MODULE) */

#ifdef __cplusplus
}
#endif

#endif /* !defined(Py_MODCOEV_H) */
