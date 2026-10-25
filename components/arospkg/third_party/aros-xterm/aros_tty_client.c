#include "aros_tty_client.h"
#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <string.h>
static int try_action(LONG action)
{ return action==XTY_ACTION_TRY_READ || action==XTY_ACTION_TRY_WRITE; }
LONG aros_tty_control(BPTR handle,XtyControl *control)
{
    struct FileHandle *fh;XtyControl copy;SIPTR result,error;
    if(!handle || !control) { SetIoErr(ERROR_BAD_NUMBER);return DOSFALSE; }
    fh=BADDR(handle);
    if(!fh->fh_Type) { SetIoErr(ERROR_INVALID_LOCK);return DOSFALSE; }
    copy=*control;copy.reserved[0]=0;
    result=DoPkt(fh->fh_Type,XTY_ACTION_CONTROL,fh->fh_Arg1,(SIPTR)&copy,
                 sizeof copy,XTY_PACKET_MAGIC,0);
    error=IoErr();
    /* Validate before narrowing: 0xffffffff is not DOSTRUE on x86_64.
       A success carrying an error must never publish the response block. */
    if(error<0 || error>INT32_MAX ||
       (result!=DOSFALSE && result!=DOSTRUE) || (result==DOSTRUE && error)) {
        SetIoErr(ERROR_BAD_NUMBER);return DOSFALSE;
    }
    if(result!=DOSTRUE) {
        if(!IoErr()) SetIoErr(ERROR_ACTION_NOT_KNOWN);
        return DOSFALSE;
    }
    if(copy.version!=control->version || copy.operation!=control->operation ||
       copy.reserved[0]!=XTY_PACKET_MAGIC || copy.reserved[1]) {
        SetIoErr(ERROR_ACTION_NOT_KNOWN);return DOSFALSE;
    }
    *control=copy;SetIoErr(0);return DOSTRUE;
}
static LONG packet_break(BPTR handle,LONG action,void *buffer,LONG length)
{
    struct FileHandle *fh;struct MsgPort *reply;
    struct DosPacket *request,*cancel;
    LONG failed=action==XTY_ACTION_CONTROL?DOSFALSE:-1;
    LONG result=failed,error=ERROR_NO_FREE_STORE;
    ULONG signals=0;int received=0,cancelled=0,ack=0,interrupted=0;
    if(!handle) { SetIoErr(ERROR_INVALID_LOCK);return failed; }
    fh=BADDR(handle);
    if(!fh->fh_Type) { SetIoErr(ERROR_INVALID_LOCK);return failed; }
    if(SetSignal(0,SIGBREAKF_CTRL_C)&SIGBREAKF_CTRL_C) {
        SetIoErr(ERROR_BREAK);return failed;
    }
    reply=CreateMsgPort();request=AllocDosObject(DOS_STDPKT,NULL);
    cancel=AllocDosObject(DOS_STDPKT,NULL);
    if(!reply || !request || !cancel) goto cleanup;
    request->dp_Type=action;request->dp_Arg1=fh->fh_Arg1;
    request->dp_Arg2=(SIPTR)buffer;request->dp_Arg3=length;
    request->dp_Arg4=(action==XTY_ACTION_CONTROL || try_action(action))?XTY_PACKET_MAGIC:0;
    request->dp_Arg5=0;
    SendPkt(request,fh->fh_Type,reply);
    for(;;) {
        struct Message *m;
        while((m=GetMsg(reply))) {
            if(m==request->dp_Link) received=1;
            if(m==cancel->dp_Link) ack=1;
        }
        if(signals&SIGBREAKF_CTRL_C) interrupted=1;
        /* Completion wins the race. Once cancellation was submitted, its
           acknowledgement also owns storage even if the first reply is ready. */
        if(received && (!cancelled || ack)) break;
        if(interrupted && !received && !cancelled) {
            cancel->dp_Type=XTY_ACTION_CANCEL;cancel->dp_Arg1=fh->fh_Arg1;
            cancel->dp_Arg2=(SIPTR)request;cancel->dp_Arg3=0;
            cancel->dp_Arg4=XTY_PACKET_MAGIC;cancel->dp_Arg5=0;
            SendPkt(cancel,fh->fh_Type,reply);cancelled=1;
        }
        signals=Wait((1UL<<reply->mp_SigBit)|(cancelled?0:SIGBREAKF_CTRL_C));
    }
    {
        SIPTR value=request->dp_Res1,wire_error=request->dp_Res2;
        /* Validate in packet width: narrowing 0x100000000 first would turn
           a malformed count into a successful zero-byte EOF on x86_64. */
        /* A count with ERROR_BREAK is a transfer cancelled part way: the
           endpoint reports the bytes that moved, so they are not repeated.
           Any other count with an error is malformed. */
        if(wire_error<0 || wire_error>INT32_MAX || (action!=XTY_ACTION_CONTROL &&
            (value < -1 || value>length || (value>=0 && wire_error && wire_error!=ERROR_BREAK))) ||
           (action==XTY_ACTION_CONTROL &&
            ((value!=DOSFALSE && value!=DOSTRUE) || (value==DOSTRUE && wire_error)))) {
            result=failed;error=ERROR_BAD_NUMBER;
        } else { result=(LONG)value;error=(LONG)wire_error; }
    }
    /* The break stays pending unless the caller is told about it through
       ERROR_BREAK with no data; after a partial transfer it gets the count,
       so the signal must still reach it. */
    if(interrupted && (error!=ERROR_BREAK || result>0)) Signal(FindTask(NULL),SIGBREAKF_CTRL_C);
    if(result==failed && !error) error=ERROR_ACTION_NOT_KNOWN;
cleanup:
    if(cancel) FreeDosObject(DOS_STDPKT,cancel);
    if(request) FreeDosObject(DOS_STDPKT,request);
    if(reply) DeleteMsgPort(reply);
    SetIoErr(error);return result;
}
LONG aros_tty_control_break(BPTR handle,XtyControl *control)
{
    XtyControl copy;
    if(!handle || !control) { SetIoErr(ERROR_BAD_NUMBER);return DOSFALSE; }
    copy=*control;copy.reserved[0]=0;
    if(packet_break(handle,XTY_ACTION_CONTROL,&copy,sizeof copy)!=DOSTRUE) return DOSFALSE;
    if(copy.version!=control->version || copy.operation!=control->operation ||
       copy.reserved[0]!=XTY_PACKET_MAGIC || copy.reserved[1]) {
        SetIoErr(ERROR_ACTION_NOT_KNOWN);return DOSFALSE;
    }
    *control=copy;SetIoErr(0);return DOSTRUE;
}
static LONG io_break(BPTR handle,LONG action,void *buffer,LONG length)
{
    XtyControl caps;LONG result;
    if(!handle) { SetIoErr(ERROR_INVALID_LOCK);return -1; }
    if(length<0 || (!buffer && length)) { SetIoErr(ERROR_BAD_NUMBER);return -1; }
    if(!IsInteractive(handle)) { SetIoErr(ERROR_ACTION_NOT_KNOWN);return -1; }
    memset(&caps,0,sizeof caps);caps.version=XTY_CONTROL_VERSION;caps.operation=XTY_CAPS;
    if(aros_tty_control(handle,&caps)!=DOSTRUE) return -1;
    if(!(caps.values[0]&XTY_CAP_CANCEL) ||
       (try_action(action) && !(caps.values[0]&XTY_CAP_TRY_IO))) {
        SetIoErr(ERROR_ACTION_NOT_KNOWN);return -1;
    }
    if(!length) { SetIoErr(0);return 0; }
    result=packet_break(handle,action,buffer,length);
    if(result < -1 || result>length) { SetIoErr(ERROR_BAD_NUMBER);return -1; }
    return result;
}
LONG aros_tty_read_break(BPTR handle,void *buffer,LONG length)
{ return io_break(handle,ACTION_READ,buffer,length); }
LONG aros_tty_write_break(BPTR handle,const void *buffer,LONG length)
{ return io_break(handle,ACTION_WRITE,(void *)buffer,length); }
LONG aros_tty_read_try(BPTR handle,void *buffer,LONG length)
{ return io_break(handle,XTY_ACTION_TRY_READ,buffer,length); }
LONG aros_tty_write_try(BPTR handle,const void *buffer,LONG length)
{ return io_break(handle,XTY_ACTION_TRY_WRITE,(void *)buffer,length); }
