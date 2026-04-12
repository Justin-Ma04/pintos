#include "userprog/syscall.h"
#include <debug.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "devices/shutdown.h"
#include "userprog/pagedir.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static void syscall_handler (struct intr_frame *);
static void terminate_bad_user (void) NO_RETURN;
static void syscall_exit (int status) NO_RETURN;
static void validate_user_ptr (const void *uaddr);
static void validate_user_range (const void *uaddr, size_t size);
static uint32_t copy_in_u32 (const void *uaddr);
static int syscall_write (int fd, const void *buffer, unsigned size);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f)
{
  uint32_t syscall_no = copy_in_u32 (f->esp);

  switch (syscall_no)
    {
    case SYS_HALT:
      shutdown_power_off ();
      NOT_REACHED ();

    case SYS_EXIT:
      syscall_exit ((int) copy_in_u32 ((const uint8_t *) f->esp + 4));
      NOT_REACHED ();

    case SYS_WRITE:
      {
        int fd = (int) copy_in_u32 ((const uint8_t *) f->esp + 4);
        const void *buffer = (const void *) copy_in_u32 ((const uint8_t *) f->esp + 8);
        unsigned size = (unsigned) copy_in_u32 ((const uint8_t *) f->esp + 12);
        validate_user_range (buffer, size);
        f->eax = syscall_write (fd, buffer, size);
        break;
      }

    default:
      terminate_bad_user ();
      NOT_REACHED ();
    }
}

static void
syscall_exit (int status)
{
  thread_current ()->exit_status = status;
  thread_exit ();
  NOT_REACHED ();
}

static void
terminate_bad_user (void)
{
  syscall_exit (-1);
  NOT_REACHED ();
}

static void
validate_user_ptr (const void *uaddr)
{
  struct thread *cur = thread_current ();

  if (uaddr == NULL || !is_user_vaddr (uaddr) || cur->pagedir == NULL
      || pagedir_get_page (cur->pagedir, uaddr) == NULL)
    terminate_bad_user ();
}

static void
validate_user_range (const void *uaddr, size_t size)
{
  uintptr_t start;
  uintptr_t end;
  uintptr_t page;

  if (size == 0)
    return;

  if (uaddr == NULL)
    terminate_bad_user ();

  start = (uintptr_t) uaddr;
  end = start + size - 1;

  if (end < start || !is_user_vaddr ((const void *) start)
      || !is_user_vaddr ((const void *) end))
    terminate_bad_user ();

  page = (uintptr_t) pg_round_down ((const void *) start);
  while (true)
    {
      validate_user_ptr ((const void *) page);
      if (page + PGSIZE - 1 >= end)
        break;
      page += PGSIZE;
    }
}

static uint32_t
copy_in_u32 (const void *uaddr)
{
  validate_user_range (uaddr, sizeof (uint32_t));
  return *(const uint32_t *) uaddr;
}

static int
syscall_write (int fd, const void *buffer, unsigned size)
{
  if (fd != 1)
    return -1;
  if (size == 0)
    return 0;

  putbuf (buffer, size);
  return (int) size;
}
