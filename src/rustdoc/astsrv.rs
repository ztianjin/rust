#[doc(
    brief = "Provides all access to AST-related, non-sendable info",
    desc =
    "Rustdoc is intended to be parallel, and the rustc AST is filled \
     with shared boxes. The AST service attempts to provide a single \
     place to query AST-related information, shielding the rest of \
     Rustdoc from its non-sendableness."
)];

import rustc::driver::session;
import rustc::driver::driver;
import rustc::driver::diagnostic;
import rustc::driver::diagnostic::handler;
import rustc::syntax::ast;
import rustc::syntax::codemap;
import rustc::middle::ast_map;
import rustc::back::link;
import rustc::util::filesearch;
import rustc::front;
import rustc::middle::resolve;

export ctxt;
export ctxt_handler;
export srv;
export mk_srv_from_str;
export mk_srv_from_file;
export exec;

type ctxt = {
    ast: @ast::crate,
    ast_map: ast_map::map,
    exp_map: resolve::exp_map
};

type ctxt_handler<T> = fn~(ctxt: ctxt) -> T;

type srv = {
    ctxt: ctxt
};

fn mk_srv_from_str(source: str) -> srv {
    let (sess, ignore_errors) = build_session();
    {
        ctxt: build_ctxt(sess, parse::from_str_sess(sess, source),
                         ignore_errors)
    }
}

fn mk_srv_from_file(file: str) -> srv {
    let (sess, ignore_errors) = build_session();
    {
        ctxt: build_ctxt(sess, parse::from_file_sess(sess, file),
                         ignore_errors)
    }
}

fn build_ctxt(sess: session::session, ast: @ast::crate,
              ignore_errors: @mutable bool) -> ctxt {

    import rustc::front::config;

    let ast = config::strip_unconfigured_items(ast);
    let ast = front::test::modify_for_testing(sess, ast);
    let ast_map = ast_map::map_crate(*ast);
    *ignore_errors = true;
    let exp_map = resolve::resolve_crate_reexports(sess, ast_map, ast);
    *ignore_errors = false;

    {
        ast: ast,
        ast_map: ast_map,
        exp_map: exp_map
    }
}

// FIXME: this whole structure should not be duplicated here. makes it
// painful to add or remove options.
fn build_session() -> (session::session, @mutable bool) {
    let sopts: @session::options = @{
        crate_type: session::lib_crate,
        static: false,
        optimize: 0u,
        debuginfo: false,
        extra_debuginfo: false,
        verify: false,
        lint_opts: [],
        save_temps: false,
        stats: false,
        time_passes: false,
        time_llvm_passes: false,
        output_type: link::output_type_exe,
        addl_lib_search_paths: [],
        maybe_sysroot: none,
        target_triple: driver::host_triple(),
        cfg: [],
        test: false,
        parse_only: false,
        no_trans: false,
        no_asm_comments: false,
        monomorphize: false,
        warn_unused_imports: false
    };

    let codemap = codemap::new_codemap();
    let error_handlers = build_error_handlers(codemap);
    let {emitter, span_handler, ignore_errors} = error_handlers;

    let session = driver::build_session_(sopts, ".", codemap, emitter,
                                         span_handler);
    (session, ignore_errors)
}

type error_handlers = {
    emitter: diagnostic::emitter,
    span_handler: diagnostic::span_handler,
    ignore_errors: @mutable bool
};

// Build a custom error handler that will allow us to ignore non-fatal
// errors
fn build_error_handlers(
    codemap: codemap::codemap
) -> error_handlers {

    type diagnostic_handler = {
        inner: diagnostic::handler,
        ignore_errors: @mutable bool
    };

    impl of diagnostic::handler for diagnostic_handler {
        fn fatal(msg: str) -> ! { self.inner.fatal(msg) }
        fn err(msg: str) { self.inner.err(msg) }
        fn bump_err_count() {
            if !(*self.ignore_errors) {
                self.inner.bump_err_count();
            }
        }
        fn has_errors() -> bool { self.inner.has_errors() }
        fn abort_if_errors() { self.inner.abort_if_errors() }
        fn warn(msg: str) { self.inner.warn(msg) }
        fn note(msg: str) { self.inner.note(msg) }
        fn bug(msg: str) -> ! { self.inner.bug(msg) }
        fn unimpl(msg: str) -> ! { self.inner.unimpl(msg) }
        fn emit(cmsp: option<(codemap::codemap, codemap::span)>,
                msg: str, lvl: diagnostic::level) {
            self.inner.emit(cmsp, msg, lvl)
        }
    }

    let ignore_errors = @mutable false;
    let emitter = fn@(cmsp: option<(codemap::codemap, codemap::span)>,
                       msg: str, lvl: diagnostic::level) {
        if !(*ignore_errors) {
            diagnostic::emit(cmsp, msg, lvl);
        }
    };
    let inner_handler = diagnostic::mk_handler(some(emitter));
    let handler = {
        inner: inner_handler,
        ignore_errors: ignore_errors
    };
    let span_handler = diagnostic::mk_span_handler(
        handler as diagnostic::handler, codemap);

    {
        emitter: emitter,
        span_handler: span_handler,
        ignore_errors: ignore_errors
    }
}

#[test]
fn should_prune_unconfigured_items() {
    let source = "#[cfg(shut_up_and_leave_me_alone)]fn a() { }";
    let srv = mk_srv_from_str(source);
    exec(srv) {|ctxt|
        assert vec::is_empty(ctxt.ast.node.module.items);
    }
}

#[test]
fn srv_should_build_ast_map() {
    let source = "fn a() { }";
    let srv = mk_srv_from_str(source);
    exec(srv) {|ctxt|
        assert ctxt.ast_map.size() != 0u
    };
}

#[test]
fn srv_should_build_reexport_map() {
    let source = "import a::b; export b; mod a { mod b { } }";
    let srv = mk_srv_from_str(source);
    exec(srv) {|ctxt|
        assert ctxt.exp_map.size() != 0u
    };
}

#[test]
fn srv_should_resolve_external_crates() {
    let source = "use std;\
                  fn f() -> std::sha1::sha1 {\
                  std::sha1::mk_sha1() }";
    // Just testing that resolve doesn't crash
    mk_srv_from_str(source);
}

#[test]
fn srv_should_resolve_core_crate() {
    let source = "fn a() -> option { fail }";
    // Just testing that resolve doesn't crash
    mk_srv_from_str(source);
}

#[test]
fn srv_should_resolve_non_existant_imports() {
    // We want to ignore things we can't resolve. Shouldn't
    // need to be able to find external crates to create docs.
    let source = "import wooboo; fn a() { }";
    mk_srv_from_str(source);
}

#[test]
fn srv_should_resolve_non_existant_uses() {
    let source = "use forble; fn a() { }";
    mk_srv_from_str(source);
}

#[test]
fn should_ignore_external_import_paths_that_dont_exist() {
    let source = "use forble; import forble::bippy;";
    mk_srv_from_str(source);
}

fn exec<T>(
    srv: srv,
    f: fn~(ctxt: ctxt) -> T
) -> T {
    f(srv.ctxt)
}

#[test]
fn srv_should_return_request_result() {
    let source = "fn a() { }";
    let srv = mk_srv_from_str(source);
    let result = exec(srv) {|_ctxt| 1000};
    assert result == 1000;
}
