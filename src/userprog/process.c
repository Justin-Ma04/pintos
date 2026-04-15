#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "threads/synch.h"
#include "threads/malloc.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
 
static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static bool push_stack_bytes (void **esp, const void *src, size_t size);
static bool setup_arguments (void **esp, char **argv, int argc);
 
#define MAX_CMD_ARGS 128
 
/* Shared descriptor between a parent and one of its children.
   Allocated on the kernel heap so it outlives whichever of the two
   threads exits first.  The last party to exit (parent abandons child
   or child exits after parent already called wait) frees the block.
 
   Lifetime rules:
     - Created by process_execute() before the child thread starts.
     - ref_count starts at 2 (one for parent, one for child).
     - Each side decrements ref_count (under the lock) when it is done
       with the block; the one that reaches 0 calls free().
     - The child writes exit_status and does sema_up in process_exit().
     - The parent does sema_down in process_wait(), reads exit_status,
       then decrements ref_count.
*/
struct child_info
  {
    tid_t tid;                  /* Child's thread id.                        */
    int exit_status;            /* Set by child just before it exits.        */
    struct semaphore sema;      /* Child ups this when it has exited.        */
    int ref_count;              /* 2 initially; freed when it reaches 0.     */
    struct lock ref_lock;       /* Protects ref_count.                       */
    struct list_elem elem;      /* Element in parent's children list.        */
  };
 
/* Global filesystem lock.
   The Pintos file system provides no internal synchronization, so
   every call to filesys_*() or file_*() must be serialized through
   this lock.  It is initialized once in process_execute() before any
   user process runs, but since process_execute() may be called before
   the first user process we initialize it lazily via a one-time flag
   protected by the fact that the kernel is single-threaded at boot.
   For simplicity we initialize it at module level using an initializer
   and expose it via process.h so syscall.c can also acquire it. */
struct lock filesys_lock;
 
/** Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  char *name_copy;
  char *program_name;
  char *save_ptr;
  tid_t tid;
 
  /* Initialize the global filesystem lock exactly once.
     This runs in kernel context before any user thread can race us. */
  static bool filesys_lock_initialized = false;
  if (!filesys_lock_initialized)
    {
      lock_init (&filesys_lock);
      filesys_lock_initialized = true;
    }
 
  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);
 
  name_copy = palloc_get_page (0);
  if (name_copy == NULL)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  strlcpy (name_copy, file_name, PGSIZE);
  program_name = strtok_r (name_copy, " ", &save_ptr);
  if (program_name == NULL)
    {
      palloc_free_page (name_copy);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
 
  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (program_name, PRI_DEFAULT, start_process, fn_copy);
  palloc_free_page (name_copy);
  if (tid == TID_ERROR)
    palloc_free_page (fn_copy);
  return tid;
}
 
/** A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  char *cmdline = file_name_;
  char *argv[MAX_CMD_ARGS];
  char *save_ptr;
  char *token;
  int argc;
  bool arg_ok;
  struct intr_frame if_;
  bool success;
  argc = 0;
  arg_ok = true;
  for (token = strtok_r (cmdline, " ", &save_ptr);
       token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    {
      if (argc >= MAX_CMD_ARGS)
        {
          arg_ok = false;
          break;
        }
      argv[argc++] = token;
    }
 
  if (!arg_ok || argc == 0)
    {
      palloc_free_page (cmdline);
      thread_exit ();
    }