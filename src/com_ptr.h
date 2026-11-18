#ifndef COM_PTR_H
#define COM_PTR_H

/* Minimal COM smart pointer for C11. All COM interfaces share the IUnknown
   vtable layout (QueryInterface, AddRef, Release first), so a single
   IUnknown*-based wrapper can own any interface pointer. Ownership is
   explicit: attach takes over a caller-addref'd pointer; reset releases.
   Copies are shallow (no implicit AddRef) - use attach/detach to move
   ownership deliberately. */

#include <windows.h>

typedef struct {
    IUnknown *p;
} com_ptr;

static inline IUnknown *com_ptr_get(com_ptr *ptr) {
    return ptr->p;
}

/* Take ownership of `p` (which must already be addref'd), releasing any
   previously held pointer. Returns the previous raw pointer or NULL. */
static inline void com_ptr_release(IUnknown *p) {
    if (p) p->lpVtbl->Release(p);
}

static inline IUnknown *com_ptr_attach(com_ptr *ptr, IUnknown *p) {
    IUnknown *old = ptr->p;
    ptr->p = p;
    com_ptr_release(old);
    return old;
}

/* Release the held pointer (if any) and clear. Returns the released pointer
   or NULL. The caller takes no additional reference. */
static inline IUnknown *com_ptr_reset(com_ptr *ptr) {
    IUnknown *p = ptr->p;
    ptr->p = NULL;
    com_ptr_release(p);
    return p;
}

/* Detach: return the held pointer without releasing it, and clear the slot.
   The caller now owns the reference. */
static inline IUnknown *com_ptr_detach(com_ptr *ptr) {
    IUnknown *p = ptr->p;
    ptr->p = NULL;
    return p;
}

#define COM_PTR_INIT { NULL }

#endif
