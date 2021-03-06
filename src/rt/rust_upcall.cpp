/*
  Upcalls

  These are runtime functions that the compiler knows about and generates
  calls to. They are called on the Rust stack and, in most cases, immediately
  switch to the C stack.
 */

#include "rust_cc.h"
#include "rust_internal.h"
#include "rust_task_thread.h"
#include "rust_unwind.h"
#include "rust_upcall.h"
#include "rust_util.h"
#include <stdint.h>


#ifdef __GNUC__
#define LOG_UPCALL_ENTRY(task)                            \
    LOG(task, upcall,                                     \
        "> UPCALL %s - task: %s 0x%" PRIxPTR              \
        " retpc: x%" PRIxPTR,                             \
        __FUNCTION__,                                     \
        (task)->name, (task),                             \
        __builtin_return_address(0));
#else
#define LOG_UPCALL_ENTRY(task)                            \
    LOG(task, upcall, "> UPCALL task: %s @x%" PRIxPTR,    \
        (task)->name, (task));
#endif

// This is called to ensure we've set up our rust stacks
// correctly. Strategically placed at entry to upcalls because they begin on
// the rust stack and happen frequently enough to catch most stack changes,
// including at the beginning of all landing pads.
// FIXME: Enable this for windows
#if defined __linux__ || defined __APPLE__ || defined __FreeBSD__
extern "C" void
check_stack_alignment() __attribute__ ((aligned (16)));
#else
static void check_stack_alignment() { }
#endif

#define UPCALL_SWITCH_STACK(A, F) call_upcall_on_c_stack((void*)A, (void*)F)

inline void
call_upcall_on_c_stack(void *args, void *fn_ptr) {
    check_stack_alignment();
    rust_task *task = rust_task_thread::get_task();
    task->call_on_c_stack(args, fn_ptr);
}

extern "C" void record_sp(void *limit);

/**********************************************************************
 * Switches to the C-stack and invokes |fn_ptr|, passing |args| as argument.
 * This is used by the C compiler to call native functions and by other
 * upcalls to switch to the C stack.  The return value is passed through a
 * field in the args parameter. This upcall is specifically for switching
 * to the shim functions generated by rustc.
 */
extern "C" CDECL void
upcall_call_shim_on_c_stack(void *args, void *fn_ptr) {
    rust_task *task = rust_task_thread::get_task();

    // FIXME (1226) - The shim functions generated by rustc contain the
    // morestack prologue, so we need to let them know they have enough
    // stack.
    record_sp(0);

    try {
        task->call_on_c_stack(args, fn_ptr);
    } catch (...) {
        LOG_ERR(task, task, "Native code threw an exception");
        abort();
    }

    task->record_stack_limit();
}

/*
 * The opposite of above. Starts on a C stack and switches to the Rust
 * stack. This is the only upcall that runs from the C stack.
 */
extern "C" CDECL void
upcall_call_shim_on_rust_stack(void *args, void *fn_ptr) {
    rust_task *task = rust_task_thread::get_task();

    // FIXME: Because of the hack in the other function that disables the
    // stack limit when entering the C stack, here we restore the stack limit
    // again.
    task->record_stack_limit();

    try {
        task->call_on_rust_stack(args, fn_ptr);
    } catch (...) {
        // We can't count on being able to unwind through arbitrary
        // code. Our best option is to just fail hard.
        LOG_ERR(task, task,
                "Rust task failed after reentering the Rust stack");
        abort();
    }
}

/**********************************************************************/

struct s_fail_args {
    char const *expr;
    char const *file;
    size_t line;
};

extern "C" CDECL void
upcall_s_fail(s_fail_args *args) {
    rust_task *task = rust_task_thread::get_task();
    LOG_UPCALL_ENTRY(task);
    LOG_ERR(task, upcall, "upcall fail '%s', %s:%" PRIdPTR, 
            args->expr, args->file, args->line);
    task->fail();
}

extern "C" CDECL void
upcall_fail(char const *expr,
            char const *file,
            size_t line) {
    s_fail_args args = {expr,file,line};
    UPCALL_SWITCH_STACK(&args, upcall_s_fail);
}

/**********************************************************************
 * Allocate an object in the task-local heap.
 */

struct s_malloc_args {
    uintptr_t retval;
    type_desc *td;
};

extern "C" CDECL void
upcall_s_malloc(s_malloc_args *args) {
    rust_task *task = rust_task_thread::get_task();
    LOG_UPCALL_ENTRY(task);

    LOG(task, mem, "upcall malloc(0x%" PRIxPTR ")", args->td);

    cc::maybe_cc(task);

    // FIXME--does this have to be calloc?
    rust_opaque_box *box = task->boxed.calloc(args->td);
    void *body = box_body(box);

    debug::maybe_track_origin(task, box);

    LOG(task, mem,
        "upcall malloc(0x%" PRIxPTR ") = box 0x%" PRIxPTR
        " with body 0x%" PRIxPTR,
        args->td, (uintptr_t)box, (uintptr_t)body);
    args->retval = (uintptr_t) box;
}

extern "C" CDECL uintptr_t
upcall_malloc(type_desc *td) {
    s_malloc_args args = {0, td};
    UPCALL_SWITCH_STACK(&args, upcall_s_malloc);
    return args.retval;
}

/**********************************************************************
 * Called whenever an object in the task-local heap is freed.
 */

struct s_free_args {
    void *ptr;
};

extern "C" CDECL void
upcall_s_free(s_free_args *args) {
    rust_task *task = rust_task_thread::get_task();
    LOG_UPCALL_ENTRY(task);

    rust_task_thread *thread = task->thread;
    DLOG(thread, mem,
             "upcall free(0x%" PRIxPTR ", is_gc=%" PRIdPTR ")",
             (uintptr_t)args->ptr);

    debug::maybe_untrack_origin(task, args->ptr);

    rust_opaque_box *box = (rust_opaque_box*) args->ptr;
    task->boxed.free(box);
}

extern "C" CDECL void
upcall_free(void* ptr) {
    s_free_args args = {ptr};
    UPCALL_SWITCH_STACK(&args, upcall_s_free);
}

/**********************************************************************
 * Sanity checks on boxes, insert when debugging possible
 * use-after-free bugs.  See maybe_validate_box() in trans.rs.
 */

extern "C" CDECL void
upcall_validate_box(rust_opaque_box* ptr) {
    if (ptr) {
        assert(ptr->ref_count > 0);
        assert(ptr->td != NULL);
        assert(ptr->td->align <= 8);
        assert(ptr->td->size <= 4096); // might not really be true...
    }
}

/**********************************************************************
 * Allocate an object in the exchange heap.
 */

struct s_shared_malloc_args {
    uintptr_t retval;
    size_t nbytes;
    type_desc *td;
};

extern "C" CDECL void
upcall_s_shared_malloc(s_shared_malloc_args *args) {
    rust_task *task = rust_task_thread::get_task();
    LOG_UPCALL_ENTRY(task);

    LOG(task, mem,
        "upcall shared_malloc(%" PRIdPTR ", 0x%" PRIxPTR ")",
        args->nbytes, args->td);
    void *p = task->kernel->malloc(args->nbytes, "shared malloc");
    memset(p, '\0', args->nbytes);
    LOG(task, mem,
        "upcall shared_malloc(%" PRIdPTR ", 0x%" PRIxPTR
        ") = 0x%" PRIxPTR,
        args->nbytes, args->td, (uintptr_t)p);
    args->retval = (uintptr_t) p;
}

extern "C" CDECL uintptr_t
upcall_shared_malloc(size_t nbytes, type_desc *td) {
    s_shared_malloc_args args = {0, nbytes, td};
    UPCALL_SWITCH_STACK(&args, upcall_s_shared_malloc);
    return args.retval;
}

/**********************************************************************
 * Called whenever an object in the exchange heap is freed.
 */

struct s_shared_free_args {
    void *ptr;
};

extern "C" CDECL void
upcall_s_shared_free(s_shared_free_args *args) {
    rust_task *task = rust_task_thread::get_task();
    LOG_UPCALL_ENTRY(task);

    rust_task_thread *thread = task->thread;
    DLOG(thread, mem,
             "upcall shared_free(0x%" PRIxPTR")",
             (uintptr_t)args->ptr);
    task->kernel->free(args->ptr);
}

extern "C" CDECL void
upcall_shared_free(void* ptr) {
    s_shared_free_args args = {ptr};
    UPCALL_SWITCH_STACK(&args, upcall_s_shared_free);
}

struct s_shared_realloc_args {
    void *retval;
    void *ptr;
    size_t size;
};

extern "C" CDECL void
upcall_s_shared_realloc(s_shared_realloc_args *args) {
    rust_task *task = rust_task_thread::get_task();
    LOG_UPCALL_ENTRY(task);
    args->retval = task->kernel->realloc(args->ptr, args->size);
}

extern "C" CDECL void *
upcall_shared_realloc(void *ptr, size_t size) {
    s_shared_realloc_args args = {NULL, ptr, size};
    UPCALL_SWITCH_STACK(&args, upcall_s_shared_realloc);
    return args.retval;
}

/**********************************************************************
 * Called to deep copy a type descriptor onto the exchange heap.
 * Used when sending closures.  It's possible that we should have
 * a central hashtable to avoid copying and re-copying the same 
 * type descriptors.
 */

struct s_create_shared_type_desc_args {
    const type_desc *td;
    type_desc *res;
};

void upcall_s_create_shared_type_desc(s_create_shared_type_desc_args *args)
{
    rust_task *task = rust_task_thread::get_task();
    LOG_UPCALL_ENTRY(task);

    // Copy the main part of the type descriptor:
    const type_desc *td = args->td;
    int n_params = td->n_params;
    size_t sz = sizeof(type_desc) + sizeof(type_desc*) * (n_params+1);
    args->res = (type_desc*) task->kernel->malloc(sz, "create_shared_type_desc");
    memcpy(args->res, td, sizeof(type_desc));

    // Recursively copy any referenced descriptors:
    if (n_params == 0) {
        args->res->first_param = NULL;
    } else {
        args->res->first_param = &args->res->descs[1];
        args->res->descs[0] = args->res;
        for (int i = 0; i < n_params; i++) {
            s_create_shared_type_desc_args rec_args = {
                td->first_param[i], 0
            };
            upcall_s_create_shared_type_desc(&rec_args);
            args->res->first_param[i] = rec_args.res;
        }
    }
}

extern "C" CDECL type_desc *
upcall_create_shared_type_desc(type_desc *td) {
    s_create_shared_type_desc_args args = { td, 0 };
    UPCALL_SWITCH_STACK(&args, upcall_s_create_shared_type_desc);
    return args.res;
}

/**********************************************************************
 * Called to deep free a type descriptor from the exchange heap.
 */

void upcall_s_free_shared_type_desc(type_desc *td)
{ // n.b.: invoked from rust_cc.cpp as well as generated code
    rust_task *task = rust_task_thread::get_task();
    LOG_UPCALL_ENTRY(task);

    if (td) {
        // Recursively free any referenced descriptors:
        for (unsigned i = 0; i < td->n_params; i++) {
            upcall_s_free_shared_type_desc((type_desc*) td->first_param[i]);
        }

        task->kernel->free(td);
    }
}

extern "C" CDECL void
upcall_free_shared_type_desc(type_desc *td) {
    if (td) {
        UPCALL_SWITCH_STACK(td, upcall_s_free_shared_type_desc);
    }
}

/**********************************************************************
 * Called to intern a task-local type descriptor into the hashtable
 * associated with each scheduler.
 */

struct s_get_type_desc_args {
    type_desc *retval;
    size_t size;
    size_t align;
    size_t n_descs;
    type_desc const **descs;
    uintptr_t n_obj_params;
};

extern "C" CDECL void
upcall_s_get_type_desc(s_get_type_desc_args *args) {
    rust_task *task = rust_task_thread::get_task();
    LOG_UPCALL_ENTRY(task);

    LOG(task, cache, "upcall get_type_desc with size=%" PRIdPTR
        ", align=%" PRIdPTR ", %" PRIdPTR " descs", args->size, args->align,
        args->n_descs);
    rust_crate_cache *cache = task->get_crate_cache();
    type_desc *td = cache->get_type_desc(args->size, args->align, args->n_descs,
                                         args->descs, args->n_obj_params);
    LOG(task, cache, "returning tydesc 0x%" PRIxPTR, td);
    args->retval = td;
}

extern "C" CDECL type_desc *
upcall_get_type_desc(void *curr_crate, // ignored, legacy compat.
                     size_t size,
                     size_t align,
                     size_t n_descs,
                     type_desc const **descs,
                     uintptr_t n_obj_params) {
    s_get_type_desc_args args = {0,size,align,n_descs,descs,n_obj_params};
    UPCALL_SWITCH_STACK(&args, upcall_s_get_type_desc);
    return args.retval;
}

/**********************************************************************
 * Called to get a heap-allocated dict. These are interned and kept
 * around indefinitely
 */

struct s_intern_dict_args {
    size_t n_fields;
    void** dict;
    void** res;
};

extern "C" CDECL void
upcall_s_intern_dict(s_intern_dict_args *args) {
    rust_task *task = rust_task_thread::get_task();
    LOG_UPCALL_ENTRY(task);
    rust_crate_cache *cache = task->get_crate_cache();
    args->res = cache->get_dict(args->n_fields, args->dict);
}

extern "C" CDECL void**
upcall_intern_dict(size_t n_fields, void** dict) {
    s_intern_dict_args args = {n_fields, dict, 0 };
    UPCALL_SWITCH_STACK(&args, upcall_s_intern_dict);
    return args.res;
}

/**********************************************************************/

struct s_vec_grow_args {
    rust_vec** vp;
    size_t new_sz;
};

extern "C" CDECL void
upcall_s_vec_grow(s_vec_grow_args *args) {
    rust_task *task = rust_task_thread::get_task();
    LOG_UPCALL_ENTRY(task);
    reserve_vec(task, args->vp, args->new_sz);
    (*args->vp)->fill = args->new_sz;
}

extern "C" CDECL void
upcall_vec_grow(rust_vec** vp, size_t new_sz) {
    s_vec_grow_args args = {vp, new_sz};
    UPCALL_SWITCH_STACK(&args, upcall_s_vec_grow);
}


/**********************************************************************
 * Returns a token that can be used to deallocate all of the allocated space
 * space in the dynamic stack.
 */

struct s_dynastack_mark_args {
    void *retval;
};

extern "C" CDECL void
upcall_s_dynastack_mark(s_dynastack_mark_args *args) {
    args->retval = rust_task_thread::get_task()->dynastack.mark();
}

extern "C" CDECL void *
upcall_dynastack_mark() {
    s_dynastack_mark_args args = {0};
    UPCALL_SWITCH_STACK(&args, upcall_s_dynastack_mark);
    return args.retval;
}

/**********************************************************************
 * Allocates space in the dynamic stack and returns it.
 *
 * FIXME: Deprecated since dynamic stacks need to be self-describing for GC.
 */

struct s_dynastack_alloc_args {
    void *retval;
    size_t sz;
};

extern "C" CDECL void
upcall_s_dynastack_alloc(s_dynastack_alloc_args *args) {
    size_t sz = args->sz;
    args->retval = sz ?
        rust_task_thread::get_task()->dynastack.alloc(sz, NULL) : NULL;
}

extern "C" CDECL void *
upcall_dynastack_alloc(size_t sz) {
    s_dynastack_alloc_args args = {0, sz};
    UPCALL_SWITCH_STACK(&args, upcall_s_dynastack_alloc);
    return args.retval;
}

/**********************************************************************
 * Allocates space associated with a type descriptor in the dynamic stack and
 * returns it.
 */

struct s_dynastack_alloc_2_args {
    void *retval;
    size_t sz;
    type_desc *ty;
};

extern "C" CDECL void
upcall_s_dynastack_alloc_2(s_dynastack_alloc_2_args *args) {
    size_t sz = args->sz;
    type_desc *ty = args->ty;
    args->retval = sz ?
        rust_task_thread::get_task()->dynastack.alloc(sz, ty) : NULL;
}

extern "C" CDECL void *
upcall_dynastack_alloc_2(size_t sz, type_desc *ty) {
    s_dynastack_alloc_2_args args = {0, sz, ty};
    UPCALL_SWITCH_STACK(&args, upcall_s_dynastack_alloc_2);
    return args.retval;
}

struct s_dynastack_free_args {
    void *ptr;
};

extern "C" CDECL void
upcall_s_dynastack_free(s_dynastack_free_args *args) {
    return rust_task_thread::get_task()->dynastack.free(args->ptr);
}

/** Frees space in the dynamic stack. */
extern "C" CDECL void
upcall_dynastack_free(void *ptr) {
    s_dynastack_free_args args = {ptr};
    UPCALL_SWITCH_STACK(&args, upcall_s_dynastack_free);
}

extern "C" _Unwind_Reason_Code
__gxx_personality_v0(int version,
                     _Unwind_Action actions,
                     uint64_t exception_class,
                     _Unwind_Exception *ue_header,
                     _Unwind_Context *context);

struct s_rust_personality_args {
    _Unwind_Reason_Code retval;
    int version;
    _Unwind_Action actions;
    uint64_t exception_class;
    _Unwind_Exception *ue_header;
    _Unwind_Context *context;
};

extern "C" void
upcall_s_rust_personality(s_rust_personality_args *args) {
    args->retval = __gxx_personality_v0(args->version,
                                        args->actions,
                                        args->exception_class,
                                        args->ue_header,
                                        args->context);
}

/**
   The exception handling personality function. It figures
   out what to do with each landing pad. Just a stack-switching
   wrapper around the C++ personality function.
*/
extern "C" _Unwind_Reason_Code
upcall_rust_personality(int version,
                        _Unwind_Action actions,
                        uint64_t exception_class,
                        _Unwind_Exception *ue_header,
                        _Unwind_Context *context) {
    s_rust_personality_args args = {(_Unwind_Reason_Code)0,
                                    version, actions, exception_class,
                                    ue_header, context};
    rust_task *task = rust_task_thread::get_task();

    // The personality function is run on the stack of the
    // last function that threw or landed, which is going
    // to sometimes be the C stack. If we're on the Rust stack
    // then switch to the C stack.

    if (task->on_rust_stack()) {
        UPCALL_SWITCH_STACK(&args, upcall_s_rust_personality);
    } else {
        upcall_s_rust_personality(&args);
    }
    return args.retval;
}

extern "C" void
shape_cmp_type(int8_t *result, const type_desc *tydesc,
               const type_desc **subtydescs, uint8_t *data_0,
               uint8_t *data_1, uint8_t cmp_type);

struct s_cmp_type_args {
    int8_t *result;
    const type_desc *tydesc;
    const type_desc **subtydescs;
    uint8_t *data_0;
    uint8_t *data_1;
    uint8_t cmp_type;
};

extern "C" void
upcall_s_cmp_type(s_cmp_type_args *args) {
    shape_cmp_type(args->result, args->tydesc, args->subtydescs,
                   args->data_0, args->data_1, args->cmp_type);
}

extern "C" void
upcall_cmp_type(int8_t *result, const type_desc *tydesc,
                const type_desc **subtydescs, uint8_t *data_0,
                uint8_t *data_1, uint8_t cmp_type) {
    s_cmp_type_args args = {result, tydesc, subtydescs, data_0, data_1, cmp_type};
    UPCALL_SWITCH_STACK(&args, upcall_s_cmp_type);
}

extern "C" void
shape_log_type(const type_desc *tydesc, uint8_t *data, uint32_t level);

struct s_log_type_args {
    const type_desc *tydesc;
    uint8_t *data;
    uint32_t level;
};

extern "C" void
upcall_s_log_type(s_log_type_args *args) {
    shape_log_type(args->tydesc, args->data, args->level);
}

extern "C" void
upcall_log_type(const type_desc *tydesc, uint8_t *data, uint32_t level) {
    s_log_type_args args = {tydesc, data, level};
    UPCALL_SWITCH_STACK(&args, upcall_s_log_type);
}

struct s_new_stack_args {
    void *result;
    size_t stk_sz;
    void *args_addr;
    size_t args_sz;
};

extern "C" CDECL void
upcall_s_new_stack(struct s_new_stack_args *args) {
    rust_task *task = rust_task_thread::get_task();
    args->result = task->next_stack(args->stk_sz,
                                    args->args_addr,
                                    args->args_sz);
}

extern "C" CDECL void *
upcall_new_stack(size_t stk_sz, void *args_addr, size_t args_sz) {
    s_new_stack_args args = {NULL, stk_sz, args_addr, args_sz};
    UPCALL_SWITCH_STACK(&args, upcall_s_new_stack);
    return args.result;
}

extern "C" CDECL void
upcall_s_del_stack() {
    rust_task *task = rust_task_thread::get_task();
    task->prev_stack();
}

extern "C" CDECL void
upcall_del_stack() {
    UPCALL_SWITCH_STACK(NULL, upcall_s_del_stack);
}

// Landing pads need to call this to insert the
// correct limit into TLS.
// NB: This must run on the Rust stack because it
// needs to acquire the value of the stack pointer
extern "C" CDECL void
upcall_reset_stack_limit() {
    rust_task *task = rust_task_thread::get_task();
    task->reset_stack_limit();
}

//
// Local Variables:
// mode: C++
// fill-column: 78;
// indent-tabs-mode: nil
// c-basic-offset: 4
// buffer-file-coding-system: utf-8-unix
// End:
//
