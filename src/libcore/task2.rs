export task::{};
export task_result;
export notification;
export sched_mode;
export sched_opts;
export task_opts;
export task_builder::{};

export default_task_opts;
export mk_task_builder;
export get_opts;
export set_opts;
export add_wrapper;
export run;

export future_result;
export future_task;
export unsupervise;
export run_listener;

export spawn;
export spawn_listener;
export spawn_sched;
export try;

export yield;
export failing;
export get_task;


/* Data types */

enum task = task_id;

enum task_result {
    success,
    failure,
}

enum notification {
    exit(task, task_result)
}

enum sched_mode {
    single_threaded,
    thread_per_core,
    thread_per_task,
    manual_threads(uint),
}

type sched_opts = {
    mode: sched_mode,
    native_stack_size: option<uint>,
};

type task_opts = {
    supervise: bool,
    notify_chan: option<comm::chan<notification>>,
    sched: option<sched_opts>,
};

enum task_builder = {
    mutable opts: task_opts,
    mutable gen_body: fn@(+fn~()) -> fn~(),
};


/* Task construction */

fn default_task_opts() -> task_opts {
    {
        supervise: true,
        notify_chan: none,
        sched: none
    }
}

fn mk_task_builder() -> task_builder {
    let body_identity = fn@(+body: fn~()) -> fn~() { body };

    task_builder({
        mutable opts: default_task_opts(),
        mutable gen_body: body_identity,
    })
}

fn get_opts(builder: task_builder) -> task_opts {
    builder.opts
}

fn set_opts(builder: task_builder, opts: task_opts) {
    builder.opts = opts;
}

fn add_wrapper(builder: task_builder, gen_body: fn@(+fn~()) -> fn~()) {
    let prev_gen_body = builder.gen_body;
    builder.gen_body = fn@(+body: fn~()) -> fn~() {
        gen_body(prev_gen_body(body))
    };
}

fn run(builder: task_builder, +f: fn~()) {
    let body = builder.gen_body(f);
    spawn_raw(builder.opts) {||
        body();
    }
}


/* Builder convenience functions */

fn future_result(builder: task_builder) -> future::future<task_result> {
    // FIXME (1087, 1857): Once linked failure and notification are
    // handled in the library, I can imagine implementing this by just
    // registering an arbitrary number of task::on_exit handlers and
    // sending out messages.
    if (builder.opts.notify_chan != none) {
        fail "trying to create a result future for a builder with an \
              existing notification channel";
    }

    let po = comm::port();
    let ch = comm::chan(po);

    set_opts(builder, {
        notify_chan: some(ch)
        with get_opts(builder)
    });

    future::from_fn {||
        alt comm::recv(po) {
          exit(_, result) { result }
        }
    }
}

fn future_task(builder: task_builder) -> future::future<task> {
    let po = comm::port();
    let ch = comm::chan(po);
    add_wrapper(builder) {|body|
        fn~() {
            comm::send(ch, get_task());
            body();
        }
    }
    future::from_port(po)
}

fn unsupervise(builder: task_builder) {
    set_opts(builder, {
        supervise: false
        with get_opts(builder)
    });
}

fn run_listener<A:send>(builder: task_builder,
                        +f: fn~(comm::port<A>)) -> comm::chan<A> {
    let setup_po = comm::port();
    let setup_ch = comm::chan(setup_po);

    run(builder) {||
        let po = comm::port();
        let ch = comm::chan(po);
        comm::send(setup_ch, ch);
        f(po);
    };

    comm::recv(setup_po)
}


/* Spawn convenience functions */

fn spawn(+f: fn~()) {
    let builder = mk_task_builder();
    run(builder, f);
}

fn spawn_listener<A:send>(+f: fn~(comm::port<A>)) {
    let builder = mk_task_builder();
    run_listener(builder, f);
}

fn spawn_sched(mode: sched_mode, +f: fn~()) {
    let builder = mk_task_builder();
    set_opts(builder, {
        sched: some({
            mode: mode,
            native_stack_size: none
        })
        with get_opts(builder)
    });
    run(builder, f);
}

fn try<T:send>(+f: fn~() -> T) -> result::t<T,()> {
    let po = comm::port();
    let ch = comm::chan(po);
    let builder = mk_task_builder();
    unsupervise(builder);
    let result = future_result(builder);
    run(builder) {||
        comm::send(ch, f());
    }
    alt future::get(result) {
      success { result::ok(comm::recv(po)) }
      failure { result::err(()) }
    }
}


/* Lifecycle functions */

fn yield() {
    let task_ = rustrt::rust_get_task();
    let killed = false;
    rusti::task_yield(task_, killed);
    if killed && !failing() {
        fail "killed";
    }
}

fn failing() -> bool {
    rustrt::rust_task_is_unwinding(rustrt::rust_get_task())
}

fn get_task() -> task {
    task(rustrt::get_task_id())
}


/* Internal */

type sched_id = int;
type task_id = int;

// FIXME (1866): The task module defines this incorrectly so we
// must as well
type rust_task = *ctypes::void;

type rust_closure = {
    fnptr: ctypes::intptr_t,
    envptr: ctypes::intptr_t
};

fn spawn_raw(opts: task_opts, +f: fn~()) unsafe {

    let f = if opts.supervise {
        f
    } else {
        // FIXME: The runtime supervision API is weird here because it
        // was designed to let the child unsupervise itself, when what
        // we actually want is for parents to unsupervise new
        // children.
        fn~() {
            rustrt::unsupervise();
            f();
        }
    };

    let fptr = ptr::addr_of(f);
    let closure: *rust_closure = unsafe::reinterpret_cast(fptr);

    let task_id = alt opts.sched {
      none {
        rustrt::new_task()
      }
      some(sched_opts) {
        new_task_in_new_sched(sched_opts)
      }
    };

    option::may(opts.notify_chan) {|c|
        // FIXME (1087): Would like to do notification in Rust
        rustrt::rust_task_config_notify(task_id, c);
    }

    rustrt::start_task(task_id, closure);
    unsafe::leak(f);

    fn new_task_in_new_sched(opts: sched_opts) -> task_id {
        if opts.native_stack_size != none {
            fail "native_stack_size scheduler option unimplemented";
        }

        let num_threads = alt opts.mode {
          single_threaded { 1u }
          thread_per_core {
            fail "thread_per_core scheduling mode unimplemented"
          }
          thread_per_task {
            fail "thread_per_task scheduling mode unimplemented"
          }
          manual_threads(threads) {
            if threads == 0u {
                fail "can not create a scheduler with no threads";
            }
            threads
          }
        };

        let sched_id = rustrt::rust_new_sched(num_threads);
        rustrt::rust_new_task_in_sched(sched_id)
    }

}

#[abi = "rust-intrinsic"]
native mod rusti {
    fn task_yield(task: *rust_task, &killed: bool);
}

native mod rustrt {
    fn rust_get_sched_id() -> sched_id;
    fn rust_new_sched(num_threads: ctypes::uintptr_t) -> sched_id;

    fn get_task_id() -> task_id;
    fn rust_get_task() -> *rust_task;

    fn new_task() -> task_id;
    fn rust_new_task_in_sched(id: sched_id) -> task_id;

    fn rust_task_config_notify(
        id: task_id, &&chan: comm::chan<notification>);

    fn start_task(id: task_id, closure: *rust_closure);

    fn rust_task_is_unwinding(rt: *rust_task) -> bool;
    fn unsupervise();
}


#[test]
fn test_spawn_raw_simple() {
    let po = comm::port();
    let ch = comm::chan(po);
    spawn_raw(default_task_opts()) {||
        comm::send(ch, ());
    }
    comm::recv(po);
}

#[test]
fn test_spawn_raw_unsupervise() {
    let opts = {
        supervise: false
        with default_task_opts()
    };
    spawn_raw(opts) {||
        fail;
    }
}

#[test]
fn test_spawn_raw_notify() {
    let task_po = comm::port();
    let task_ch = comm::chan(task_po);
    let notify_po = comm::port();
    let notify_ch = comm::chan(notify_po);

    let opts = {
        notify_chan: some(notify_ch)
        with default_task_opts()
    };
    spawn_raw(opts) {||
        comm::send(task_ch, get_task());
    }
    let task_ = comm::recv(task_po);
    assert comm::recv(notify_po) == exit(task_, success);

    let opts = {
        supervise: false,
        notify_chan: some(notify_ch)
        with default_task_opts()
    };
    spawn_raw(opts) {||
        comm::send(task_ch, get_task());
        fail;
    }
    let task_ = comm::recv(task_po);
    assert comm::recv(notify_po) == exit(task_, failure);
}

#[test]
fn test_run_basic() {
    let po = comm::port();
    let ch = comm::chan(po);
    let builder = mk_task_builder();
    run(builder) {||
        comm::send(ch, ());
    }
    comm::recv(po);
}

#[test]
fn test_add_wrapper() {
    let po = comm::port();
    let ch = comm::chan(po);
    let builder = mk_task_builder();
    add_wrapper(builder) {|body|
        fn~() {
            body();
            comm::send(ch, ());
        }
    }
    run(builder) {||}
    comm::recv(po);
}

#[test]
fn test_future_result() {
    let builder = mk_task_builder();
    let result = future_result(builder);
    run(builder) {||}
    assert future::get(result) == success;

    let builder = mk_task_builder();
    let result = future_result(builder);
    unsupervise(builder);
    run(builder) {|| fail }
    assert future::get(result) == failure;
}

#[test]
fn test_future_task() {
    let po = comm::port();
    let ch = comm::chan(po);
    let builder = mk_task_builder();
    let task1 = future_task(builder);
    run(builder) {|| comm::send(ch, get_task()) }
    assert future::get(task1) == comm::recv(po);
}

#[test]
fn test_try_success() {
    alt try {||
        "Success!"
    } {
        result::ok("Success!") { }
        _ { fail; }
    }
}

#[test]
#[ignore(cfg(target_os = "win32"))]
fn test_try_fail() {
    alt try {||
        fail
    } {
        result::err(()) { }
        _ { fail; }
    }
}

#[test]
#[should_fail]
#[ignore(cfg(target_os = "win32"))]
fn test_spawn_sched_no_threads() {
    spawn_sched(manual_threads(0u)) {|| };
}

#[test]
fn test_spawn_sched() {
    let po = comm::port();
    let ch = comm::chan(po);

    fn f(i: int, ch: comm::chan<()>) {
        let parent_sched_id = rustrt::rust_get_sched_id();

        spawn_sched(single_threaded) {||
            let child_sched_id = rustrt::rust_get_sched_id();
            assert parent_sched_id != child_sched_id;

            if (i == 0) {
                comm::send(ch, ());
            } else {
                f(i - 1, ch);
            }
        };

    }
    f(10, ch);
    comm::recv(po);
}

#[test]
fn test_spawn_sched_childs_on_same_sched() {
    let po = comm::port();
    let ch = comm::chan(po);

    spawn_sched(single_threaded) {||
        let parent_sched_id = rustrt::rust_get_sched_id();
        spawn {||
            let child_sched_id = rustrt::rust_get_sched_id();
            // This should be on the same scheduler
            assert parent_sched_id == child_sched_id;
            comm::send(ch, ());
        };
    };

    comm::recv(po);
}

#[nolink]
#[cfg(test)]
native mod testrt {
    fn rust_dbg_lock_create() -> *ctypes::void;
    fn rust_dbg_lock_destroy(lock: *ctypes::void);
    fn rust_dbg_lock_lock(lock: *ctypes::void);
    fn rust_dbg_lock_unlock(lock: *ctypes::void);
    fn rust_dbg_lock_wait(lock: *ctypes::void);
    fn rust_dbg_lock_signal(lock: *ctypes::void);
}

#[test]
fn test_spawn_sched_blocking() {

    // Testing that a task in one scheduler can block natively
    // without affecting other schedulers
    iter::repeat(20u) {||

        let start_po = comm::port();
        let start_ch = comm::chan(start_po);
        let fin_po = comm::port();
        let fin_ch = comm::chan(fin_po);

        let lock = testrt::rust_dbg_lock_create();

        spawn_sched(single_threaded) {||
            testrt::rust_dbg_lock_lock(lock);

            comm::send(start_ch, ());

            // Block the scheduler thread
            testrt::rust_dbg_lock_wait(lock);
            testrt::rust_dbg_lock_unlock(lock);

            comm::send(fin_ch, ());
        };

        // Wait until the other task has its lock
        comm::recv(start_po);

        fn pingpong(po: comm::port<int>, ch: comm::chan<int>) {
            let val = 20;
            while val > 0 {
                val = comm::recv(po);
                comm::send(ch, val - 1);
            }
        }

        let setup_po = comm::port();
        let setup_ch = comm::chan(setup_po);
        let parent_po = comm::port();
        let parent_ch = comm::chan(parent_po);
        spawn {||
            let child_po = comm::port();
            comm::send(setup_ch, comm::chan(child_po));
            pingpong(child_po, parent_ch);
        };

        let child_ch = comm::recv(setup_po);
        comm::send(child_ch, 20);
        pingpong(parent_po, child_ch);
        testrt::rust_dbg_lock_lock(lock);
        testrt::rust_dbg_lock_signal(lock);
        testrt::rust_dbg_lock_unlock(lock);
        comm::recv(fin_po);
        testrt::rust_dbg_lock_destroy(lock);
    }
}
