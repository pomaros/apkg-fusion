#ifndef AROS_TTY_CLIENT_H
#define AROS_TTY_CLIENT_H
#include <dos/dos.h>
#include "tty_control.h"
/* Live DOS FileHandle, caller serialized. DOSTRUE on validated response,
   DOSFALSE + IoErr on failure. Caller block stays unchanged on failure.
   Not a POSIX descriptor API and not intended for arbitrary non-TTY handlers. */
LONG aros_tty_control(BPTR,XtyControl *);
/* Requires negotiated XTY_CAP_CANCEL. Ctrl-C cancels pending controls only;
   both replies are collected before returning. Not an ISIG implementation. */
LONG aros_tty_control_break(BPTR,XtyControl *);
/* Private, capability-negotiated DOS-style I/O: byte count or -1 + IoErr.
   Partial completion wins over Ctrl-C. Zero length leaves Ctrl-C pending.
   Not fd-based read/write; buffers/handles stay live until the call returns. */
LONG aros_tty_read_break(BPTR,void *,LONG);
LONG aros_tty_write_break(BPTR,const void *,LONG);
/* Negotiated TRY_IO + CANCEL. Same buffer/handle lifetime and Ctrl-C rules.
   -1 + ERROR_OBJECT_IN_USE means AGAIN; a zero read means EOF (or length zero).
   No wait for bytes/capacity, but requires a responsive owner to reply. */
LONG aros_tty_read_try(BPTR,void *,LONG);
LONG aros_tty_write_try(BPTR,const void *,LONG);
#endif
