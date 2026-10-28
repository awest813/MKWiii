#pragma once

// Storage duration for state that belongs to the guest CPU rather than to a host thread:
// recursion guards and cursors of pumps that only guest threads run, and the handoff between
// guest contexts.
//
// Where guest threads are fibers on one host thread (every native host), that is exactly
// `thread_local`: all guest threads share one copy, and real host worker threads keep their own.
// With the thread-backed HostContext (the web build), every guest thread is its own host thread,
// so the same sharing needs one ordinary variable. That is race-free because HostContext lets
// only one guest thread run at a time and orders each handoff with a mutex. Only use this for
// state that no non-guest host thread touches.
#include "host_context.h"

#if defined(MKW_HOST_CONTEXT_THREADS)
#define MKW_GUEST_DOMAIN_LOCAL
#else
#define MKW_GUEST_DOMAIN_LOCAL thread_local
#endif
