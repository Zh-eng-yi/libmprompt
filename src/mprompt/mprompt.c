/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

#include "mprompt.h"
#include "internal/util.h"
#include "internal/longjmp.h"
#include "internal/gstack.h"

#ifdef __cplusplus
#include <exception>
#endif



//-----------------------------------------------------------------------
// Types
//-----------------------------------------------------------------------

typedef enum mp_return_kind_e {
  MP_RETURN,         // normal return
  MP_EXCEPTION,      // return with an exception
  MP_YIELD,          // yielded up
} mp_return_kind_t;


typedef struct mp_resume_point_s {   // allocated on the suspended stack (which performed a yield)
  mp_jmpbuf_t        jmp;     
  void*              result;  // the yield result (= resume argument)
} mp_resume_point_t;

typedef struct mp_return_point_s {   // allocated on the parent stack (which performed an enter/resume)
  mp_jmpbuf_t        jmp;     // must be the first field (in order to find unwind information, see `mp_stack_enter`)
  mp_return_kind_t   kind;    
  mp_yield_fun_t*    fun;     // if yielding, the function to execute
  void*              arg;     // if yielding, the argument to the function; if returning, the result.
  #ifdef __cplusplus
  std::exception_ptr exn;     // returning with an exception to propagate
  #endif
} mp_return_point_t;


// Prompt:
// Represents a piece of stack and can be yielded to.
//
// A prompt can be in 2 states:
// _active_:    top == NULL
//              means the prompt (and its gstack) is part of prompt stack chain.
// _suspended_: top != NULL, resume_point != NULL
//              This when being captured as a resumption. The `top` points to the end of the captured resumption.
//              and the prompt (and its children) are not part of the current stack chain.
//              note that the prompt children are still themselves in the _active_ state (but not part of a current execution stack chain)

struct mp_prompt_s {  
  mp_prompt_t*       parent;        // parent: previous prompt up in the stack chain (towards bottom of the stack)
  mp_prompt_t*       top;           // top of a suspended prompt chain.
  intptr_t           refcount;      // free when drops to zero
  mp_gstack_t*       gstack;        // the growable stacklet associated with this prompt;
                                 
  mp_return_point_t* return_point;  // return point in the parent (if not suspended..)
  mp_resume_point_t* resume_point;  // resume point for a suspended prompt chain. (the resume will be in the `top->gstack`)

  void*              sp;            // security: contains the (guarded) expected stack pointer for a return (if active) or resume (if suspended)
  mp_unwind_frame_t* unwind_frame;  // used to aid with unwinding on some platforms (windows only for now)
};


// Abstract type of resumptions (never used as such)
struct mp_resume_s {
  void* abstract;
};

// If resuming multiple times, the original stack is saved in a corresponding chain of prompt_save structures.
typedef struct mp_prompt_save_s {
  struct mp_prompt_save_s* next;
  mp_prompt_t*             prompt;
  mp_gsave_t*              gsave;  
} mp_prompt_save_t;


// A general resumption that can be resumed multiple times; needs a small allocation and is reference counted.
// Only copies the original stack if it is actually being resumed more than once.
typedef struct mp_mresume_s {
  intptr_t           refcount;
  long               resume_count;       // count number of resumes.
  mp_prompt_t*       prompt;
  mp_prompt_save_t*  save;
  mp_return_point_t* tail_return_point;  // need to save this as the one in the prompt may be overwritten by earlier resumes
} mp_mresume_t;


//-----------------------------------------------------------------------
// Distinguish plain once-resumptions from multi-shot resumptions.
//
// We use bit 2 in the pointers (assuming 8-byte minimal alignment) to 
// distinguish  resume-at-most-once from multi-shot resumptions. This way 
// we do not need  allocation of at-most-once resumptions while still 
// providing a consistent interface.
//-----------------------------------------------------------------------

// Is this a once resumption (returns NULL if not)
static mp_prompt_t* mp_resume_is_once(mp_resume_t* r) {
  intptr_t i = (intptr_t)r;
  return ((i & 4) == 0 ? (mp_prompt_t*)r : NULL);
}

// Is this a multi-shot resumption (returns NULL if not)
static mp_mresume_t* mp_resume_is_multi(mp_resume_t* r) {
  intptr_t i = (intptr_t)r;
  return ((i & 4) == 0 ? NULL : (mp_mresume_t*)(i ^ 4));
}

// Create a non-allocated at-most-once resumption
static mp_resume_t* mp_resume_as_once(mp_prompt_t* p) {
  return (mp_resume_t*)p;
}

// Create a multi shot resumption
static mp_resume_t* mp_resume_as_multi(mp_mresume_t* r) {
  return (mp_resume_t*)(((intptr_t)r) | 4);
}


//-----------------------------------------------------------------------
// Initialize
//-----------------------------------------------------------------------

void mp_init(const mp_config_t* config) {
  mp_guard_init();
  mp_gstack_init(config);
}


//-----------------------------------------------------------------------
// Prompt chain
//-----------------------------------------------------------------------

// The top of the prompts chain; points to the prompt on whose stack we currently execute.
mp_decl_thread mp_prompt_t* _mp_prompt_top;

// get the top of the prompt chain
mp_prompt_t* mp_prompt_top(void) {  
  return _mp_prompt_top;
}

// get the current gstack; used for on-demand-paging in gstack_mmap/gstack_win
mp_gstack_t* mp_gstack_current(void) {
  mp_prompt_t* top = mp_prompt_top();
  return (top != NULL ? top->gstack : NULL);
  // if (zz_gstack == NULL) zz_gstack = mp_gstack_alloc(0, NULL);
  // return zz_gstack;
}

// walk the prompt chain; returns NULL when done.
// with initial argument `NULL` the first prompt returned is the current top.
mp_prompt_t* mp_prompt_parent(mp_prompt_t* p) {
  return (p == NULL ? mp_prompt_top() : p->parent);
}

#ifndef NDEBUG
// An _active_ prompt is currently part of the stack.
static bool mp_prompt_is_active(mp_prompt_t* p) {
  return (p != NULL && p->top == NULL);
}

// Is a prompt an ancestor in the chain?
static bool mp_prompt_is_ancestor(mp_prompt_t* p) {
  mp_prompt_t* q = NULL;
  while ((q = mp_prompt_parent(q)) != NULL) {
    if (q == p) return true;
  }
  return false;
}
#endif

// Allocate a fresh (suspended) prompt
mp_prompt_t* mp_prompt_create(void) {
  // allocate a fresh growable stack
  mp_prompt_t* p;
  mp_gstack_t* gstack = mp_gstack_alloc(sizeof(mp_prompt_t), (void**)&p);
  if (gstack == NULL) { mp_fatal_message(ENOMEM, "unable to allocate a stack\n"); }
  // allocate the prompt structure at the base of the new stack
  p->parent = NULL;
  p->top = p;
  p->refcount = 1;
  p->gstack = gstack;
  p->resume_point = NULL;
  p->return_point = NULL;
  p->unwind_frame = NULL;
  return p;
}

// Free a prompt and drop its children
static void mp_prompt_free(mp_prompt_t* p, bool delay) {
  mp_assert_internal(!mp_prompt_is_active(p));
  p = p->top;
  while (p != NULL) {
    mp_assert_internal(p->refcount == 0);
    mp_prompt_t* parent = p->parent;    
    mp_gstack_free(p->gstack, delay);
    if (parent != NULL) {
      mp_assert_internal(parent->refcount == 1);
      parent->refcount--;
    }
    p = parent;
  }
}

// Decrement the refcount (and free when it becomes zero).
static void mp_prompt_drop_internal(mp_prompt_t* p, bool delay) {
  int64_t i = p->refcount--;
  if (i <= 1) {
    mp_prompt_free(p, delay);
  }
}

static void mp_prompt_drop(mp_prompt_t* p) {
  mp_prompt_drop_internal(p, false);
}

#ifdef __cplusplus
static void mp_prompt_drop_delayed(mp_prompt_t* p) {
  mp_prompt_drop_internal(p, true);
}
#endif

// Increment the refcount
static mp_prompt_t* mp_prompt_dup(mp_prompt_t* p) {
  p->refcount++;
  return p;
}

// Link a suspended prompt to the current prompt chain and set the new prompt top
static inline mp_resume_point_t* mp_prompt_link(mp_prompt_t* p, mp_return_point_t* ret, void** sp) {
  mp_assert_internal(ret != NULL);
  mp_assert_internal(!mp_prompt_is_active(p));
  *sp = p->sp;
  p->parent = mp_prompt_top();
  _mp_prompt_top = p->top;
  p->top = NULL;
  if (mp_likely(ret != NULL)) { 
    p->return_point = ret; 
    p->sp = mp_guard(ret->jmp.reg_sp);
    mp_unwind_frame_update(p->unwind_frame, &ret->jmp);
  }                           
  mp_assert_internal(mp_prompt_is_active(p));  
  mp_debug_asan_start_switch(_mp_prompt_top->gstack);
  return p->resume_point;
}

// Unlink a prompt from the current chain and make suspend it (and set the new prompt top to its parent)
static inline mp_return_point_t* mp_prompt_unlink(mp_prompt_t* p, mp_resume_point_t* res, void** sp) {
  mp_assert_internal(mp_prompt_is_active(p));
  mp_assert_internal(mp_prompt_is_ancestor(p)); // ancestor of current top?
  *sp = p->sp;
  p->top = mp_prompt_top();
  _mp_prompt_top = p->parent;
  p->parent = NULL;  
  p->resume_point = res;
  if (mp_likely(res != NULL)) {   // on return/exception
    p->sp = mp_guard(res->jmp.reg_sp);
  }
  // note: leave return_point as-is for potential reuse in tail resumes
  mp_assert_internal(!mp_prompt_is_active(p));
  mp_debug_asan_start_switch(_mp_prompt_top == NULL ? NULL : _mp_prompt_top->gstack);
  return p->return_point;
}


//-----------------------------------------------------------------------
// Checked longjmp
// We use a form of control-flow integrity by only allowing
// a longjmp to two known code locations (one for resume, and one for return)
//-----------------------------------------------------------------------

// The code addresses are initialized on the first call to setjmp (and are located right after the setjmp call)
// todo: can we make this static so these go to the readonly section? 
static void* mp_return_label;
static void* mp_resume_label;


// Checked longjmp to a known location (with a known stack pointer)
static mp_decl_noreturn void mp_checked_longjmp(void* label, void* sp, mp_jmpbuf_t* jmp) {
  // security: check if we return to the designated label
  if (mp_unlikely(mp_unguard(label) != jmp->reg_ip)) {
    mp_fatal_message(EFAULT, "potential stack corruption detected: expected ip %p, but found %p\n", mp_unguard(label), jmp->reg_ip);
  }
  if (mp_unlikely(mp_unguard(sp) != jmp->reg_sp)) {
    mp_fatal_message(EFAULT, "potential stack corruption detected: expected sp %p, but found %p\n", mp_unguard(sp), jmp->reg_sp);
  }
  mp_longjmp(jmp); 
}


//-----------------------------------------------------------------------
// Create an initial prompt
//-----------------------------------------------------------------------

// Initial stack entry

typedef struct mp_entry_env_s {
  mp_prompt_t* prompt;
  mp_start_fun_t* fun;
  void* arg;
} mp_entry_env_t;

static  void mp_prompt_stack_entry(void* penv, mp_unwind_frame_t* unwind_frame) {
  MP_UNUSED(unwind_frame);
  mp_entry_env_t* env = (mp_entry_env_t*)penv;
  mp_prompt_t* p = env->prompt;
  p->unwind_frame = unwind_frame;
  mp_debug_asan_end_switch(p->parent==NULL);
  //mp_prompt_stack_entry(p, env->fun, env->arg);
  void* sp;
  mp_return_point_t* ret;
  #ifdef __cplusplus
  try {
  #endif
    void* result = (env->fun)(p, env->arg);
    // RET: return from a prompt
    ret = mp_prompt_unlink(p, NULL, &sp);
    ret->arg = result;
    ret->fun = NULL;
    ret->kind = MP_RETURN;    
  #ifdef __cplusplus
  }
  catch (...) {
    mp_trace_message("catch exception to propagate across the prompt %p..\n", p);
    ret = mp_prompt_unlink(p, NULL, &sp);
    ret->exn = std::current_exception();
    ret->arg = NULL;
    ret->fun = NULL;
    ret->kind = MP_EXCEPTION;
  }
  #endif  
  mp_checked_longjmp(mp_return_label, sp, &ret->jmp);
}



// Execute the function that is yielded or return normally.
static mp_decl_noinline void* mp_prompt_exec_yield_fun(mp_return_point_t* ret, mp_prompt_t* p) {
  mp_assert_internal(!mp_prompt_is_active(p));
  if (ret->kind == MP_YIELD) {
    return (ret->fun)(mp_resume_as_once(p), ret->arg);
  }
  else if (ret->kind == MP_RETURN) {
    void* result = ret->arg;
    mp_prompt_drop(p);
    return result;
  }
  else {
    #ifdef __cplusplus
    mp_assert_internal(ret->kind == MP_EXCEPTION);
    mp_trace_message("rethrow propagated exception again (from prompt %p)..\n", p);
    mp_prompt_drop_delayed(p);
    std::rethrow_exception(ret->exn);
    #else
    mp_unreachable("invalid return kind");
    #endif
  }
}


// Resume a prompt: used for the initial entry as well as for resuming in a suspended prompt.
static mp_decl_noinline void* mp_prompt_resume(mp_prompt_t * p, void* arg) {
  mp_return_point_t ret;    
  // save our return location for yields and regular return  
  if (mp_setjmp(&ret.jmp)) {
    //mp_return_label:
    // P: return from yield (YR), or a regular return (RET)
    // printf("%s to prompt %p\n", (ret.kind == MP_RETURN ? "returned" : "yielded"), p);    
    mp_debug_asan_end_switch(false);
    return mp_prompt_exec_yield_fun(&ret, p);  // must be under the setjmp to preserve the stack
  }
  else {
    // security: longjmp can only jump to a known code point
    if (mp_unlikely(mp_return_label == NULL)) { 
      mp_return_label = mp_guard(ret.jmp.reg_ip); 
    }

    mp_assert(p->parent == NULL);
    void* sp;
    mp_resume_point_t* res = mp_prompt_link(p,&ret,&sp);  // make active
    if (res != NULL) {
      // PR: resume to yield point
      res->result = arg;
      mp_checked_longjmp(mp_resume_label, sp, &res->jmp);
    }
    else {
      // PI: initial entry, switch to the new stack with an initial function      
      mp_gstack_enter(p->gstack, (mp_jmpbuf_t**)&p->return_point, &mp_prompt_stack_entry, arg);
    }
    mp_unreachable("mp_prompt_resume");    // should never return
  }
}

void* mp_prompt_enter(mp_prompt_t* p, mp_start_fun_t* fun, void* arg) {
  mp_assert_internal(!mp_prompt_is_active(p) && p->resume_point == NULL);
  mp_entry_env_t env;
  env.prompt = p;
  env.fun = fun;
  env.arg = arg;
  return mp_prompt_resume(p, &env);
}

// Install a fresh prompt `p` with a growable stack and start running `fun(p,arg)` on it.
void* mp_prompt(mp_start_fun_t* fun, void* arg) {
  mp_prompt_t* p = mp_prompt_create();
  return mp_prompt_enter(p, fun, arg);  // enter the initial stack with fun(arg)
}



//-----------------------------------------------------------------------
// Resume from a yield (once)
//-----------------------------------------------------------------------

// Forwards for multi-shot resumptions
static void* mp_mresume(mp_mresume_t* r, void* arg);
static void* mp_mresume_tail(mp_mresume_t* r, void* arg);
static void  mp_mresume_drop(mp_mresume_t* r);
static mp_mresume_t* mp_mresume_dup(mp_mresume_t* r);


// Resume 
void* mp_resume(mp_resume_t* resume, void* arg) {
  mp_prompt_t* p = mp_resume_is_once(resume);
  if (mp_unlikely(p == NULL)) return mp_mresume(mp_resume_is_multi(resume), arg);
  mp_assert_internal(p->refcount == 1);
  mp_assert_internal(p->resume_point != NULL);
  return mp_prompt_resume(p, arg);  // resume back to yield point
}

// Resume in tail position to a prompt `p`
// Uses longjmp back to the `return_jump` as if it is yielding; this
// makes the tail-recursion use no stack as they keep getting back (P)
// and then into the exec_yield_fun function.
static void* mp_prompt_resume_tail(mp_prompt_t* p, void* arg, mp_return_point_t* ret) {
  mp_assert_internal(p->refcount == 1);
  mp_assert_internal(!mp_prompt_is_active(p));
  mp_assert_internal(p->resume_point != NULL);
  void* sp;
  mp_resume_point_t* res = mp_prompt_link(p,ret,&sp);   // make active using the given return point!
  res->result = arg;
  mp_checked_longjmp(mp_resume_label, sp, &res->jmp);
}


// Resume in tail position (last and only resume in scope)
void* mp_resume_tail(mp_resume_t* resume, void* arg) {
  mp_prompt_t* p = mp_resume_is_once(resume);
  if (mp_unlikely(p == NULL)) return mp_mresume_tail(mp_resume_is_multi(resume), arg);
  return mp_prompt_resume_tail(p, arg, p->return_point);  // reuse return-point of the original entry
}

void mp_resume_drop(mp_resume_t* resume) {
  mp_prompt_t* p = mp_resume_is_once(resume);
  if (mp_unlikely(p == NULL)) return mp_mresume_drop(mp_resume_is_multi(resume));
  mp_prompt_drop(p);
}

mp_resume_t* mp_resume_dup(mp_resume_t* resume) {
  mp_mresume_t* r = mp_resume_is_multi(resume);
  if (mp_unlikely(r == NULL)) {
    mp_error_message(EINVAL, "cannot dup once-resumptions; use 'mp_myield' instead.\n");    
    return NULL;
  }
  else {
    mp_mresume_dup(r);
    return resume;
  }
}


long mp_resume_resume_count(mp_resume_t* resume) {
  mp_mresume_t* r = mp_resume_is_multi(resume);
  return (r == NULL ? 0 : r->resume_count);
}

int mp_resume_should_unwind(mp_resume_t* resume) {
  mp_mresume_t* r = mp_resume_is_multi(resume);
  return (r != NULL && r->refcount == 1 && r->resume_count == 0);
}


//-----------------------------------------------------------------------
// Yield up to a prompt
//-----------------------------------------------------------------------


// Yield back to a prompt with a `mp_resume_once_t` resumption and run `fun(arg)` at the yield point
void* mp_yield(mp_prompt_t* p, mp_yield_fun_t* fun, void* arg) {
  mp_assert(mp_prompt_is_ancestor(p));           // can only yield up to an ancestor
  mp_assert_internal(mp_prompt_is_active(p));    // can only yield to an active prompt
  // set our resume point (Y)
  mp_resume_point_t res;
  if (mp_setjmp(&res.jmp)) {
    //mp_resume_label:
    // Y: resuming with a result (from PR)
    mp_assert_internal(mp_prompt_is_active(p));  // when resuming, we should be active again
    mp_assert_internal(mp_prompt_is_ancestor(p));
    mp_debug_asan_end_switch(p->parent==NULL);
    return res.result;
  }
  else {
    // security: can only longjmp to a static location
    if (mp_unlikely(mp_resume_label == NULL)) {
      mp_resume_label = mp_guard(res.jmp.reg_ip);
    }
    // YR: yielding to prompt, or resumed prompt (P)
    void* sp;
    mp_return_point_t* ret = mp_prompt_unlink(p, &res, &sp);
    ret->fun = fun;
    ret->arg = arg;
    ret->kind = MP_YIELD;
    mp_checked_longjmp(mp_return_label, sp, &ret->jmp);
  }
}



//-----------------------------------------------------------------------
// General resume's that are first-class (and need allocation)
//-----------------------------------------------------------------------

// Create a multi-shot resumption from a single-shot one
mp_resume_t* mp_resume_multi(mp_resume_t* once) {
  mp_prompt_t* p = mp_resume_is_once(once);
  if (p == NULL) return once; // already multi-shot
  mp_mresume_t* r = mp_malloc_safe_tp(mp_mresume_t);
  r->prompt = p;
  r->refcount = 1;
  r->resume_count = 0;
  r->save = NULL;
  r->tail_return_point = p->return_point;
  return mp_resume_as_multi(r);
}

// Increment the reference count of a resumption.
static mp_mresume_t* mp_mresume_dup(mp_mresume_t* r) {
  r->refcount++;  
  return r;
}

// Decrement the reference count of a resumption.
static void mp_mresume_drop(mp_mresume_t* r) {
  int64_t i = r->refcount--;
  if (i <= 1) {
    // free saved stacklets
    mp_prompt_save_t* s = r->save;
    while (s != NULL) {
      mp_prompt_save_t* next = s->next;
      mp_prompt_t* p = s->prompt;
      mp_gsave_free(s->gsave);
      mp_free(s);
      mp_prompt_drop(p);
      s = next;
    }
    mp_prompt_drop(r->prompt);
    //mp_trace_message("free resume: %p\n", r);
    mp_free(r);
  }
}


// Save a full prompt chain started at `p`
static mp_prompt_save_t* mp_prompt_save(mp_prompt_t* p) {
  mp_assert_internal(!mp_prompt_is_active(p));  
  mp_prompt_save_t* savep = NULL;
  uint8_t* sp = (uint8_t*)p->resume_point->jmp.reg_sp;
  p = p->top;
  do {
    mp_prompt_save_t* save = mp_malloc_tp(mp_prompt_save_t);
    save->prompt = mp_prompt_dup(p);
    save->next = savep;
    save->gsave = mp_gstack_save(p->gstack,sp);
    savep = save;
    sp = (uint8_t*)(p->parent == NULL ? NULL : p->return_point->jmp.reg_sp);  // set to parent's sp
    p = p->parent;    
  } while (p != NULL);
  mp_assert_internal(savep != NULL);
  return savep;
}

// Restore all prompt stacks from a save.
static void mp_prompt_restore(mp_prompt_t* p, mp_prompt_save_t* save) {
  mp_assert_internal(!mp_prompt_is_active(p));
  mp_assert_internal(p == save->prompt);
  MP_UNUSED(p);
  do {
    //mp_assert_internal(p == save->prompt);
    mp_gsave_restore(save->gsave);  // TODO: restore refcount?
    save = save->next;
  } while (save != NULL);
}


// Ensure proper refcount and pristine stack
static mp_prompt_t* mp_resume_get_prompt(mp_mresume_t* r) {
  mp_prompt_t* p = r->prompt;
  if (r->save != NULL) {
    mp_prompt_restore(p, r->save);
  }
  else if (r->refcount > 1 || p->refcount > 1) {
    r->save = mp_prompt_save(p);
  }
  mp_prompt_dup(p);
  mp_mresume_drop(r);
  return p;
}


// Resume with a regular resumption (and consumes `r` so dup if it needs to used later on)
static void* mp_mresume(mp_mresume_t* r, void* arg) {
  r->resume_count++;
  mp_prompt_t* p = mp_resume_get_prompt(r);
  return mp_prompt_resume(p, arg);  // set a fresh prompt 
}

// Resume in tail position 
// Note: this only works if all earlier resumes were in-scope -- which should hold
// or otherwise the tail resumption wasn't in tail position anyways.
static void* mp_mresume_tail(mp_mresume_t* r, void* arg) {
  mp_return_point_t* ret = r->tail_return_point;
  if (ret == NULL) {
    return mp_mresume(r, arg);  // resume normally as the return_point may not be preserved correctly
  }
  else {
    r->tail_return_point = NULL;                    // todo: do we need `sp` as well?
    r->resume_count++;
    mp_prompt_t* p = mp_resume_get_prompt(r);       
    return mp_prompt_resume_tail(p, arg, ret);      // resume tail by reusing the original entry return point
  }
}


//-----------------------------------------------------------------------
// Backtrace
//-----------------------------------------------------------------------

#if defined(_WIN32)

#include <windows.h>
// On windows, CaptureStackBackTrace only captures to the first prompt 
// (probably due to stack extent checks stored in the TIB?). 
// To return a proper backtrace, we can yield up to each parent prompt and 
// recursively capture partial backtraces at each point.
typedef struct mp_yield_backtrace_env_s {
  void** bt;
  int    len;
} mp_yield_backtrace_env_t;

static int mp_win_backtrace(void** bt, int len, int skip);

static void* mp_yield_backtrace(mp_resume_t* resume, void* envarg) {
  mp_yield_backtrace_env_t* env = (mp_yield_backtrace_env_t*)envarg;
  intptr_t n = mp_win_backtrace(env->bt, env->len, 1 /* don't include yield_backtrace */);
  return mp_resume_tail(resume, (void*)n);
}

static int mp_win_backtrace(void** bt, int len, int skip) {
  if (len <= 0) return 0; // done
  int n = (int)CaptureStackBackTrace(skip + 1 /* don't include our own frame */, len, bt, NULL);
  if (n <= 0 || n >= len) return n;
  // check if we have more parent frames in a parent prompt
  mp_prompt_t* p = mp_prompt_top();
  if (p == NULL) return n;  // no more frames available
  // yield recursively up to get more frames
  mp_yield_backtrace_env_t env = { bt + n, len - n };
  intptr_t m = (intptr_t)mp_yield(p, &mp_yield_backtrace, &env);
  mp_assert_internal(m + n <= len);
  return (int)(n + m);
}

int mp_backtrace(void** bt, int len) {
  return mp_win_backtrace(bt, len, 1 /* don't include mp_backtrace */ );
}

#elif defined(__MACH__)

// On macOS, standard backtrace fails cross the prompt boundaries (despite proper dwarf info).
// We use a similar strategy as on windows recursively yielding up and 
// capturing backtraces per prompt using the standard unwind provided on macOS.
// Note, we could just unwind directly but that is not always working in release mode.

#define UNW_LOCAL_ONLY
#include <libunwind.h>

typedef struct mp_yield_backtrace_env_s {
  void** bt;
  int    len;
} mp_yield_backtrace_env_t;

static int mp_mach_backtrace(void** bt, int len);

static void* mp_yield_backtrace(mp_resume_t* resume, void* envarg) {
  mp_yield_backtrace_env_t* env = (mp_yield_backtrace_env_t*)envarg;
  intptr_t n = mp_mach_backtrace(env->bt, env->len);
  return mp_resume_tail(resume, (void*)n);
}

static int mp_mach_unw_backtrace(void** bt, int len, int skip) {
  unw_cursor_t cursor; 
  unw_context_t uc;  
  unw_getcontext(&uc);
  unw_init_local(&cursor, &uc);
  int count = 0;
  while (count < len && unw_step(&cursor) > 0) {
    unw_word_t ip;
    unw_get_reg(&cursor, UNW_REG_IP, &ip);
    if (skip > 0) {
      skip--;
    }
    else {     
      bt[count++] = (void*)ip;
    }
    unw_proc_info_t pinfo;
    unw_get_proc_info(&cursor, &pinfo);
    if ((void*)pinfo.start_ip == &mp_stack_enter) break;
  }
  return count;
}

static int mp_mach_backtrace(void** bt, int len) {
  if (len <= 0) return 0; // done
  int n = mp_mach_unw_backtrace(bt,len, 2);
  if (n <= 0 || n >= len) return n;
  // check if we have more parent frames in a parent prompt
  mp_prompt_t* p = mp_prompt_top();
  if (p == NULL) return n;  // no more frames available
  // yield recursively up to get more frames
  mp_yield_backtrace_env_t env = { bt + n, len - n };
  intptr_t m = (intptr_t)mp_yield(p, &mp_yield_backtrace, &env);
  mp_assert_internal(m + n <= len);
  return (int)(n + m);
}

int mp_backtrace(void** bt, int len) {
  return mp_mach_backtrace(bt, len);
}


#else

// Linux, etc. 
// Unwinding works as is (due to reliable dwarf unwind info and no stack limits in the thread local data)
#include <execinfo.h>
int mp_backtrace(void** bt, int len) {
  return backtrace(bt, len);
}

#endif

/*
void mp_gstack_win_test(mp_gstack_t* g);
void* win_test(mp_prompt_t* p, void* arg) {
  mp_gstack_win_test(p->gstack);
  return NULL;
}
*/
