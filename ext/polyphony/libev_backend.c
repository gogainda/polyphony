#include <netdb.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "polyphony.h"
#include "../libev/ev.h"

VALUE cTCPSocket;

typedef struct LibevBackend_t {
  struct ev_loop *ev_loop;
  struct ev_async break_async;
  int running;
  int ref_count;
  int run_no_wait_count;
} LibevBackend_t;

static size_t LibevBackend_size(const void *ptr) {
  return sizeof(LibevBackend_t);
}

static const rb_data_type_t LibevBackend_type = {
    "Libev",
    {0, 0, LibevBackend_size,},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE LibevBackend_allocate(VALUE klass) {
  LibevBackend_t *backend = ALLOC(LibevBackend_t);

  return TypedData_Wrap_Struct(klass, &LibevBackend_type, backend);
}

#define GetLibevBackend(obj, backend) \
  TypedData_Get_Struct((obj), LibevBackend_t, &LibevBackend_type, (backend))

void break_async_callback(struct ev_loop *ev_loop, struct ev_async *ev_async, int revents) {
  // This callback does nothing, the break async is used solely for breaking out
  // of a *blocking* event loop (waking it up) in a thread-safe, signal-safe manner
}

static VALUE LibevBackend_initialize(VALUE self) {
  LibevBackend_t *backend;
  VALUE thread = rb_thread_current();
  int is_main_thread = (thread == rb_thread_main());

  GetLibevBackend(self, backend);
  backend->ev_loop = is_main_thread ? EV_DEFAULT : ev_loop_new(EVFLAG_NOSIGMASK);

  ev_async_init(&backend->break_async, break_async_callback);
  ev_async_start(backend->ev_loop, &backend->break_async);
  ev_unref(backend->ev_loop); // don't count the break_async watcher

  backend->running = 0;
  backend->ref_count = 0;
  backend->run_no_wait_count = 0;

  return Qnil;
}

VALUE LibevBackend_finalize(VALUE self) {
  LibevBackend_t *backend;
  GetLibevBackend(self, backend);

   ev_async_stop(backend->ev_loop, &backend->break_async);

  if (!ev_is_default_loop(backend->ev_loop)) ev_loop_destroy(backend->ev_loop);

  return self;
}

VALUE LibevBackend_post_fork(VALUE self) {
  LibevBackend_t *backend;
  GetLibevBackend(self, backend);

  // After fork there may be some watchers still active left over from the
  // parent, so we destroy the loop, even if it's the default one, then use the
  // default one, as post_fork is called only from the main thread of the forked
  // process. That way we don't need to call ev_loop_fork, since the loop is
  // always a fresh one.
  ev_loop_destroy(backend->ev_loop);
  backend->ev_loop = EV_DEFAULT;

  return self;
}

VALUE LibevBackend_ref(VALUE self) {
  LibevBackend_t *backend;
  GetLibevBackend(self, backend);

  backend->ref_count++;
  return self;
}

VALUE LibevBackend_unref(VALUE self) {
  LibevBackend_t *backend;
  GetLibevBackend(self, backend);

  backend->ref_count--;
  return self;
}

int LibevBackend_ref_count(VALUE self) {
  LibevBackend_t *backend;
  GetLibevBackend(self, backend);

  return backend->ref_count;
}

void LibevBackend_reset_ref_count(VALUE self) {
  LibevBackend_t *backend;
  GetLibevBackend(self, backend);

  backend->ref_count = 0;
}

VALUE LibevBackend_pending_count(VALUE self) {
  int count;
  LibevBackend_t *backend;
  GetLibevBackend(self, backend);
  count = ev_pending_count(backend->ev_loop);
  return INT2NUM(count);
}

VALUE LibevBackend_poll(VALUE self, VALUE nowait, VALUE current_fiber, VALUE queue) {
  int is_nowait = nowait == Qtrue;
  LibevBackend_t *backend;
  GetLibevBackend(self, backend);

  if (is_nowait) {
    long runnable_count = Queue_len(queue);
    backend->run_no_wait_count++;
    if (backend->run_no_wait_count < runnable_count || backend->run_no_wait_count < 10)
      return self;
  }

  backend->run_no_wait_count = 0;

  COND_TRACE(2, SYM_fiber_ev_loop_enter, current_fiber);
  backend->running = 1;
  ev_run(backend->ev_loop, is_nowait ? EVRUN_NOWAIT : EVRUN_ONCE);
  backend->running = 0;
  COND_TRACE(2, SYM_fiber_ev_loop_leave, current_fiber);

  return self;
}

VALUE LibevBackend_wakeup(VALUE self) {
  LibevBackend_t *backend;
  GetLibevBackend(self, backend);

  if (backend->running) {
    // Since the loop will run until at least one event has occurred, we signal
    // the selector's associated async watcher, which will cause the ev loop to
    // return. In contrast to using `ev_break` to break out of the loop, which
    // should be called from the same thread (from within the ev_loop), using an
    // `ev_async` allows us to interrupt the event loop across threads.
    ev_async_send(backend->ev_loop, &backend->break_async);
    return Qtrue;
  }

  return Qnil;
}

#include "polyphony.h"
#include "../libev/ev.h"

//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
// the following is copied verbatim from the Ruby source code (io.c)
struct io_internal_read_struct {
    int fd;
    int nonblock;
    void *buf;
    size_t capa;
};

#define StringValue(v) rb_string_value(&(v))

int io_setstrbuf(VALUE *str, long len) {
  #ifdef _WIN32
    len = (len + 1) & ~1L;	/* round up for wide char */
  #endif
  if (NIL_P(*str)) {
    *str = rb_str_new(0, len);
    return 1;
  }
  else {
    VALUE s = StringValue(*str);
    long clen = RSTRING_LEN(s);
    if (clen >= len) {
      rb_str_modify(s);
      return 0;
    }
    len -= clen;
  }
  rb_str_modify_expand(*str, len);
  return 0;
}

#define MAX_REALLOC_GAP 4096
static void io_shrink_read_string(VALUE str, long n) {
  if (rb_str_capacity(str) - n > MAX_REALLOC_GAP) {
    rb_str_resize(str, n);
  }
}

void io_set_read_length(VALUE str, long n, int shrinkable) {
  if (RSTRING_LEN(str) != n) {
    rb_str_modify(str);
    rb_str_set_len(str, n);
    if (shrinkable) io_shrink_read_string(str, n);
  }
}

static rb_encoding* io_read_encoding(rb_io_t *fptr) {
    if (fptr->encs.enc) {
	return fptr->encs.enc;
    }
    return rb_default_external_encoding();
}

VALUE io_enc_str(VALUE str, rb_io_t *fptr) {
    OBJ_TAINT(str);
    rb_enc_associate(str, io_read_encoding(fptr));
    return str;
}

//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////

struct libev_io {
  struct ev_io io;
  VALUE fiber;
};

void LibevBackend_io_callback(EV_P_ ev_io *w, int revents)
{
  struct libev_io *watcher = (struct libev_io *)w;
  Fiber_make_runnable(watcher->fiber, Qnil);
}

inline VALUE libev_await(LibevBackend_t *backend) {
  VALUE ret;
  backend->ref_count++;
  ret = Thread_switch_fiber(rb_thread_current());
  backend->ref_count--;
  RB_GC_GUARD(ret);
  return ret;
}

VALUE libev_wait_fd_with_watcher(LibevBackend_t *backend, int fd, struct libev_io *watcher, int events) {
  VALUE switchpoint_result;

  if (watcher->fiber == Qnil) {
    watcher->fiber = rb_fiber_current();
    ev_io_init(&watcher->io, LibevBackend_io_callback, fd, events);
  }
  ev_io_start(backend->ev_loop, &watcher->io);

  switchpoint_result = libev_await(backend);

  ev_io_stop(backend->ev_loop, &watcher->io);
  RB_GC_GUARD(switchpoint_result);
  return switchpoint_result;
}

VALUE libev_wait_fd(LibevBackend_t *backend, int fd, int events, int raise_exception) {
  struct libev_io watcher;
  VALUE switchpoint_result = Qnil;
  watcher.fiber = Qnil;

  switchpoint_result = libev_wait_fd_with_watcher(backend, fd, &watcher, events);

  if (raise_exception) TEST_RESUME_EXCEPTION(switchpoint_result);
  RB_GC_GUARD(switchpoint_result);
  return switchpoint_result;
}

VALUE libev_snooze() {
  Fiber_make_runnable(rb_fiber_current(), Qnil);
  return Thread_switch_fiber(rb_thread_current());
}

ID ID_ivar_is_nonblocking;

// Since we need to ensure that fd's are non-blocking before every I/O
// operation, here we improve upon Ruby's rb_io_set_nonblock by caching the
// "nonblock" state in an instance variable. Calling rb_ivar_get on every read
// is still much cheaper than doing a fcntl syscall on every read! Preliminary
// benchmarks (with a "hello world" HTTP server) show throughput is improved
// by 10-13%.
inline void io_set_nonblock(rb_io_t *fptr, VALUE io) {
  VALUE is_nonblocking = rb_ivar_get(io, ID_ivar_is_nonblocking);
  if (is_nonblocking == Qtrue) return;

  rb_ivar_set(io, ID_ivar_is_nonblocking, Qtrue);

#ifdef _WIN32
  rb_w32_set_nonblock(fptr->fd);
#elif defined(F_GETFL)
  int oflags = fcntl(fptr->fd, F_GETFL);
  if ((oflags == -1) && (oflags & O_NONBLOCK)) return;
  oflags |= O_NONBLOCK;
  fcntl(fptr->fd, F_SETFL, oflags);
#endif
}

VALUE LibevBackend_read(VALUE self, VALUE io, VALUE str, VALUE length, VALUE to_eof) {
  LibevBackend_t *backend;
  struct libev_io watcher;
  rb_io_t *fptr;
  long dynamic_len = length == Qnil;
  long len = dynamic_len ? 4096 : NUM2INT(length);
  int shrinkable = io_setstrbuf(&str, len);
  char *buf = RSTRING_PTR(str);
  long total = 0;
  VALUE switchpoint_result = Qnil;
  int read_to_eof = RTEST(to_eof);
  VALUE underlying_io = rb_iv_get(io, "@io");

  GetLibevBackend(self, backend);
  if (underlying_io != Qnil) io = underlying_io;
  GetOpenFile(io, fptr);
  rb_io_check_byte_readable(fptr);
  io_set_nonblock(fptr, io);
  watcher.fiber = Qnil;

  OBJ_TAINT(str);

  // Apparently after reopening a closed file, the file position is not reset,
  // which causes the read to fail. Fortunately we can use fptr->rbuf.len to
  // find out if that's the case.
  // See: https://github.com/digital-fabric/polyphony/issues/30
  if (fptr->rbuf.len > 0) {
    lseek(fptr->fd, -fptr->rbuf.len, SEEK_CUR);
    fptr->rbuf.len = 0;
  }

  while (1) {
    ssize_t n = read(fptr->fd, buf, len - total);
    if (n < 0) {
      int e = errno;
      if (e != EWOULDBLOCK && e != EAGAIN) rb_syserr_fail(e, strerror(e));

      switchpoint_result = libev_wait_fd_with_watcher(backend, fptr->fd, &watcher, EV_READ);

      if (TEST_EXCEPTION(switchpoint_result)) goto error;
    }
    else {
      switchpoint_result = libev_snooze();

      if (TEST_EXCEPTION(switchpoint_result)) goto error;

      if (n == 0) break; // EOF
      total = total + n;
      if (!read_to_eof) break;

      if (total == len) {
        if (!dynamic_len) break;

        rb_str_resize(str, total);
        rb_str_modify_expand(str, len);
        buf = RSTRING_PTR(str) + total;
        shrinkable = 0;
        len += len;
      }
      else buf += n;
    }
  }

  io_set_read_length(str, total, shrinkable);
  io_enc_str(str, fptr);

  if (total == 0) return Qnil;

  RB_GC_GUARD(watcher.fiber);
  RB_GC_GUARD(switchpoint_result);

  return str;
error:
  return RAISE_EXCEPTION(switchpoint_result);
}

VALUE LibevBackend_read_loop(VALUE self, VALUE io) {

  #define PREPARE_STR() { \
    str = Qnil; \
    shrinkable = io_setstrbuf(&str, len); \
    buf = RSTRING_PTR(str); \
    total = 0; \
    OBJ_TAINT(str); \
  }

  #define YIELD_STR() { \
    io_set_read_length(str, total, shrinkable); \
    io_enc_str(str, fptr); \
    rb_yield(str); \
    PREPARE_STR(); \
  }

  LibevBackend_t *backend;
  struct libev_io watcher;
  rb_io_t *fptr;
  VALUE str;
  long total;
  long len = 8192;
  int shrinkable;
  char *buf;
  VALUE switchpoint_result = Qnil;
  VALUE underlying_io = rb_iv_get(io, "@io");

  PREPARE_STR();

  GetLibevBackend(self, backend);
  if (underlying_io != Qnil) io = underlying_io;
  GetOpenFile(io, fptr);
  rb_io_check_byte_readable(fptr);
  io_set_nonblock(fptr, io);
  watcher.fiber = Qnil;

  // Apparently after reopening a closed file, the file position is not reset,
  // which causes the read to fail. Fortunately we can use fptr->rbuf.len to
  // find out if that's the case.
  // See: https://github.com/digital-fabric/polyphony/issues/30
  if (fptr->rbuf.len > 0) {
    lseek(fptr->fd, -fptr->rbuf.len, SEEK_CUR);
    fptr->rbuf.len = 0;
  }

  while (1) {
    ssize_t n = read(fptr->fd, buf, len);
    if (n < 0) {
      int e = errno;
      if ((e != EWOULDBLOCK && e != EAGAIN)) rb_syserr_fail(e, strerror(e));

      switchpoint_result = libev_wait_fd_with_watcher(backend, fptr->fd, &watcher, EV_READ);
      if (TEST_EXCEPTION(switchpoint_result)) goto error;
    }
    else {
      switchpoint_result = libev_snooze();

      if (TEST_EXCEPTION(switchpoint_result)) goto error;

      if (n == 0) break; // EOF
      total = n;
      YIELD_STR();
    }
  }

  RB_GC_GUARD(str);
  RB_GC_GUARD(watcher.fiber);
  RB_GC_GUARD(switchpoint_result);

  return io;
error:
  return RAISE_EXCEPTION(switchpoint_result);
}

VALUE LibevBackend_write(VALUE self, VALUE io, VALUE str) {
  LibevBackend_t *backend;
  struct libev_io watcher;
  rb_io_t *fptr;
  VALUE switchpoint_result = Qnil;
  VALUE underlying_io;
  char *buf = StringValuePtr(str);
  long len = RSTRING_LEN(str);
  long left = len;

  underlying_io = rb_iv_get(io, "@io");
  if (underlying_io != Qnil) io = underlying_io;
  GetLibevBackend(self, backend);
  io = rb_io_get_write_io(io);
  GetOpenFile(io, fptr);
  watcher.fiber = Qnil;

  while (left > 0) {
    ssize_t n = write(fptr->fd, buf, left);
    if (n < 0) {
      int e = errno;
      if ((e != EWOULDBLOCK && e != EAGAIN)) rb_syserr_fail(e, strerror(e));

      switchpoint_result = libev_wait_fd_with_watcher(backend, fptr->fd, &watcher, EV_WRITE);

      if (TEST_EXCEPTION(switchpoint_result)) goto error;
    }
    else {
      buf += n;
      left -= n;
    }
  }

  if (watcher.fiber == Qnil) {
    switchpoint_result = libev_snooze();

    if (TEST_EXCEPTION(switchpoint_result)) goto error;
  }

  RB_GC_GUARD(watcher.fiber);
  RB_GC_GUARD(switchpoint_result);

  return INT2NUM(len);
error:
  return RAISE_EXCEPTION(switchpoint_result);
}

VALUE LibevBackend_writev(VALUE self, VALUE io, int argc, VALUE *argv) {
  LibevBackend_t *backend;
  struct libev_io watcher;
  rb_io_t *fptr;
  VALUE switchpoint_result = Qnil;
  VALUE underlying_io;
  long total_length = 0;
  long total_written = 0;
  struct iovec *iov = 0;
  struct iovec *iov_ptr = 0;
  int iov_count = argc;

  underlying_io = rb_iv_get(io, "@io");
  if (underlying_io != Qnil) io = underlying_io;
  GetLibevBackend(self, backend);
  io = rb_io_get_write_io(io);
  GetOpenFile(io, fptr);
  watcher.fiber = Qnil;

  iov = malloc(iov_count * sizeof(struct iovec));
  for (int i = 0; i < argc; i++) {
    VALUE str = argv[i];
    iov[i].iov_base = StringValuePtr(str);
    iov[i].iov_len = RSTRING_LEN(str);
    total_length += iov[i].iov_len;
  }
  iov_ptr = iov;

  while (1) {
    ssize_t n = writev(fptr->fd, iov_ptr, iov_count);
    if (n < 0) {
      int e = errno;
      if ((e != EWOULDBLOCK && e != EAGAIN)) rb_syserr_fail(e, strerror(e));

      switchpoint_result = libev_wait_fd_with_watcher(backend, fptr->fd, &watcher, EV_WRITE);

      if (TEST_EXCEPTION(switchpoint_result)) goto error;
    }
    else {
      total_written += n;
      if (total_written == total_length) break;

      while (n > 0) {
        if ((size_t) n < iov_ptr[0].iov_len) {
          iov_ptr[0].iov_base = (char *) iov_ptr[0].iov_base + n;
          iov_ptr[0].iov_len -= n;
          n = 0;
        }
        else {
          n -= iov_ptr[0].iov_len;
          iov_ptr += 1;
          iov_count -= 1;
        }
      }
    }
  }
  if (watcher.fiber == Qnil) {
    switchpoint_result = libev_snooze();

    if (TEST_EXCEPTION(switchpoint_result)) goto error;
  }

  RB_GC_GUARD(watcher.fiber);
  RB_GC_GUARD(switchpoint_result);

  free(iov);
  return INT2NUM(total_written);
error:
  free(iov);
  return RAISE_EXCEPTION(switchpoint_result);
}

VALUE LibevBackend_write_m(int argc, VALUE *argv, VALUE self) {
  if (argc < 2)
    // TODO: raise ArgumentError
    rb_raise(rb_eRuntimeError, "(wrong number of arguments (expected 2 or more))");

  return (argc == 2) ?
    LibevBackend_write(self, argv[0], argv[1]) :
    LibevBackend_writev(self, argv[0], argc - 1, argv + 1);
}

///////////////////////////////////////////////////////////////////////////

VALUE LibevBackend_accept(VALUE self, VALUE sock) {
  LibevBackend_t *backend;
  struct libev_io watcher;
  rb_io_t *fptr;
  int fd;
  struct sockaddr addr;
  socklen_t len = (socklen_t)sizeof addr;
  VALUE switchpoint_result = Qnil;
  VALUE underlying_sock = rb_iv_get(sock, "@io");
  if (underlying_sock != Qnil) sock = underlying_sock;

  GetLibevBackend(self, backend);
  GetOpenFile(sock, fptr);
  io_set_nonblock(fptr, sock);
  watcher.fiber = Qnil;
  while (1) {
    fd = accept(fptr->fd, &addr, &len);
    if (fd < 0) {
      int e = errno;
      if ((e != EWOULDBLOCK && e != EAGAIN)) rb_syserr_fail(e, strerror(e));

      switchpoint_result = libev_wait_fd_with_watcher(backend, fptr->fd, &watcher, EV_READ);

      if (TEST_EXCEPTION(switchpoint_result)) goto error;
    }
    else {
      VALUE socket;
      rb_io_t *fp;
      switchpoint_result = libev_snooze();

      if (TEST_EXCEPTION(switchpoint_result)) {
        close(fd); // close fd since we're raising an exception
        goto error;
      }

      socket = rb_obj_alloc(cTCPSocket);
      MakeOpenFile(socket, fp);
      rb_update_max_fd(fd);
      fp->fd = fd;
      fp->mode = FMODE_READWRITE | FMODE_DUPLEX;
      rb_io_ascii8bit_binmode(socket);
      io_set_nonblock(fp, socket);
      rb_io_synchronized(fp);

      // if (rsock_do_not_reverse_lookup) {
	    //   fp->mode |= FMODE_NOREVLOOKUP;
      // }
      return socket;
    }
  }
  RB_GC_GUARD(switchpoint_result);
  return Qnil;
error:
  return RAISE_EXCEPTION(switchpoint_result);
}

VALUE LibevBackend_accept_loop(VALUE self, VALUE sock) {
  LibevBackend_t *backend;
  struct libev_io watcher;
  rb_io_t *fptr;
  int fd;
  struct sockaddr addr;
  socklen_t len = (socklen_t)sizeof addr;
  VALUE switchpoint_result = Qnil;
  VALUE socket = Qnil;
  VALUE underlying_sock = rb_iv_get(sock, "@io");
  if (underlying_sock != Qnil) sock = underlying_sock;

  GetLibevBackend(self, backend);
  GetOpenFile(sock, fptr);
  io_set_nonblock(fptr, sock);
  watcher.fiber = Qnil;

  while (1) {
    fd = accept(fptr->fd, &addr, &len);
    if (fd < 0) {
      int e = errno;
      if ((e != EWOULDBLOCK && e != EAGAIN)) rb_syserr_fail(e, strerror(e));

      switchpoint_result = libev_wait_fd_with_watcher(backend, fptr->fd, &watcher, EV_READ);

      if (TEST_EXCEPTION(switchpoint_result)) goto error;
    }
    else {
      rb_io_t *fp;
      switchpoint_result = libev_snooze();

      if (TEST_EXCEPTION(switchpoint_result)) {
        close(fd); // close fd since we're raising an exception
        goto error;
      }

      socket = rb_obj_alloc(cTCPSocket);
      MakeOpenFile(socket, fp);
      rb_update_max_fd(fd);
      fp->fd = fd;
      fp->mode = FMODE_READWRITE | FMODE_DUPLEX;
      rb_io_ascii8bit_binmode(socket);
      io_set_nonblock(fp, socket);
      rb_io_synchronized(fp);

      rb_yield(socket);
      socket = Qnil;
    }
  }

  RB_GC_GUARD(socket);
  RB_GC_GUARD(watcher.fiber);
  RB_GC_GUARD(switchpoint_result);
  return Qnil;
error:
  return RAISE_EXCEPTION(switchpoint_result);
}

VALUE LibevBackend_connect(VALUE self, VALUE sock, VALUE host, VALUE port) {
  LibevBackend_t *backend;
  struct libev_io watcher;
  rb_io_t *fptr;
  struct sockaddr_in addr;
  char *host_buf = StringValueCStr(host);
  VALUE switchpoint_result = Qnil;
  VALUE underlying_sock = rb_iv_get(sock, "@io");
  if (underlying_sock != Qnil) sock = underlying_sock;

  GetLibevBackend(self, backend);
  GetOpenFile(sock, fptr);
  io_set_nonblock(fptr, sock);
  watcher.fiber = Qnil;

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(host_buf);
  addr.sin_port = htons(NUM2INT(port));

  int result = connect(fptr->fd, (struct sockaddr *)&addr, sizeof(addr));
  if (result < 0) {
    int e = errno;
    if (e != EINPROGRESS) rb_syserr_fail(e, strerror(e));

    switchpoint_result = libev_wait_fd_with_watcher(backend, fptr->fd, &watcher, EV_WRITE);

    if (TEST_EXCEPTION(switchpoint_result)) goto error;
  }
  else {
    switchpoint_result = libev_snooze();

    if (TEST_EXCEPTION(switchpoint_result)) goto error;
  }
  RB_GC_GUARD(switchpoint_result);
  return sock;
error:
  return RAISE_EXCEPTION(switchpoint_result);
}

VALUE LibevBackend_wait_io(VALUE self, VALUE io, VALUE write) {
  LibevBackend_t *backend;
  rb_io_t *fptr;
  int events = RTEST(write) ? EV_WRITE : EV_READ;
  VALUE underlying_io = rb_iv_get(io, "@io");
  if (underlying_io != Qnil) io = underlying_io;
  GetLibevBackend(self, backend);
  GetOpenFile(io, fptr);

  return libev_wait_fd(backend, fptr->fd, events, 1);
}

struct libev_timer {
  struct ev_timer timer;
  VALUE fiber;
};

void LibevBackend_timer_callback(EV_P_ ev_timer *w, int revents)
{
  struct libev_timer *watcher = (struct libev_timer *)w;
  Fiber_make_runnable(watcher->fiber, Qnil);
}

VALUE LibevBackend_sleep(VALUE self, VALUE duration) {
  LibevBackend_t *backend;
  struct libev_timer watcher;
  VALUE switchpoint_result = Qnil;

  GetLibevBackend(self, backend);
  watcher.fiber = rb_fiber_current();
  ev_timer_init(&watcher.timer, LibevBackend_timer_callback, NUM2DBL(duration), 0.);
  ev_timer_start(backend->ev_loop, &watcher.timer);

  switchpoint_result = libev_await(backend);

  ev_timer_stop(backend->ev_loop, &watcher.timer);
  TEST_RESUME_EXCEPTION(switchpoint_result);
  RB_GC_GUARD(watcher.fiber);
  RB_GC_GUARD(switchpoint_result);
  return switchpoint_result;
}

struct libev_child {
  struct ev_child child;
  VALUE fiber;
};

void LibevBackend_child_callback(EV_P_ ev_child *w, int revents)
{
  struct libev_child *watcher = (struct libev_child *)w;
  int exit_status = w->rstatus >> 8; // weird, why should we do this?
  VALUE status;

  status = rb_ary_new_from_args(2, INT2NUM(w->rpid), INT2NUM(exit_status));
  Fiber_make_runnable(watcher->fiber, status);
}

VALUE LibevBackend_waitpid(VALUE self, VALUE pid) {
  LibevBackend_t *backend;
  struct libev_child watcher;
  VALUE switchpoint_result = Qnil;
  GetLibevBackend(self, backend);

  watcher.fiber = rb_fiber_current();
  ev_child_init(&watcher.child, LibevBackend_child_callback, NUM2INT(pid), 0);
  ev_child_start(backend->ev_loop, &watcher.child);

  switchpoint_result = libev_await(backend);

  ev_child_stop(backend->ev_loop, &watcher.child);
  TEST_RESUME_EXCEPTION(switchpoint_result);
  RB_GC_GUARD(watcher.fiber);
  RB_GC_GUARD(switchpoint_result);
  return switchpoint_result;
}

struct ev_loop *LibevBackend_ev_loop(VALUE self) {
  LibevBackend_t *backend;
  GetLibevBackend(self, backend);
  return backend->ev_loop;
}

void LibevBackend_async_callback(EV_P_ ev_async *w, int revents) { }

VALUE LibevBackend_wait_event(VALUE self, VALUE raise) {
  LibevBackend_t *backend;
  VALUE switchpoint_result = Qnil;
  GetLibevBackend(self, backend);

  struct ev_async async;

  ev_async_init(&async, LibevBackend_async_callback);
  ev_async_start(backend->ev_loop, &async);

  switchpoint_result = libev_await(backend);

  ev_async_stop(backend->ev_loop, &async);
  if (RTEST(raise)) TEST_RESUME_EXCEPTION(switchpoint_result);
  RB_GC_GUARD(switchpoint_result);
  return switchpoint_result;
}

void Init_LibevBackend() {
  rb_require("socket");
  cTCPSocket = rb_const_get(rb_cObject, rb_intern("TCPSocket"));

  VALUE cBackend = rb_define_class_under(mPolyphony, "Backend", rb_cData);
  rb_define_alloc_func(cBackend, LibevBackend_allocate);

  rb_define_method(cBackend, "initialize", LibevBackend_initialize, 0);
  rb_define_method(cBackend, "finalize", LibevBackend_finalize, 0);
  rb_define_method(cBackend, "post_fork", LibevBackend_post_fork, 0);
  rb_define_method(cBackend, "pending_count", LibevBackend_pending_count, 0);

  rb_define_method(cBackend, "ref", LibevBackend_ref, 0);
  rb_define_method(cBackend, "unref", LibevBackend_unref, 0);

  rb_define_method(cBackend, "poll", LibevBackend_poll, 3);
  rb_define_method(cBackend, "break", LibevBackend_wakeup, 0);

  rb_define_method(cBackend, "read", LibevBackend_read, 4);
  rb_define_method(cBackend, "read_loop", LibevBackend_read_loop, 1);
  rb_define_method(cBackend, "write", LibevBackend_write_m, -1);
  rb_define_method(cBackend, "accept", LibevBackend_accept, 1);
  rb_define_method(cBackend, "accept_loop", LibevBackend_accept_loop, 1);
  rb_define_method(cBackend, "connect", LibevBackend_connect, 3);
  rb_define_method(cBackend, "wait_io", LibevBackend_wait_io, 2);
  rb_define_method(cBackend, "sleep", LibevBackend_sleep, 1);
  rb_define_method(cBackend, "waitpid", LibevBackend_waitpid, 1);
  rb_define_method(cBackend, "wait_event", LibevBackend_wait_event, 1);

  ID_ivar_is_nonblocking = rb_intern("@is_nonblocking");

  __BACKEND__.pending_count   = LibevBackend_pending_count;
  __BACKEND__.poll            = LibevBackend_poll;
  __BACKEND__.ref             = LibevBackend_ref;
  __BACKEND__.ref_count       = LibevBackend_ref_count;
  __BACKEND__.reset_ref_count = LibevBackend_reset_ref_count;
  __BACKEND__.unref           = LibevBackend_unref;
  __BACKEND__.wait_event      = LibevBackend_wait_event;
  __BACKEND__.wakeup          = LibevBackend_wakeup;
}
