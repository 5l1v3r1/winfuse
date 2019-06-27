/**
 * @file winfuse/proto.c
 *
 * @copyright 2019 Bill Zissimopoulos
 */
/*
 * This file is part of WinFuse.
 *
 * You can redistribute it and/or modify it under the terms of the GNU
 * Affero General Public License version 3 as published by the Free
 * Software Foundation.
 *
 * Licensees holding a valid commercial license may use this software
 * in accordance with the commercial license agreement provided in
 * conjunction with the software.  The terms and conditions of any such
 * commercial license agreement shall govern, supersede, and render
 * ineffective any application of the AGPLv3 license to this software,
 * notwithstanding of any reference thereto in the software or
 * associated repository.
 */

#include <winfuse/driver.h>

NTSTATUS FuseProtoPostInit(PDEVICE_OBJECT DeviceObject);
VOID FuseProtoSendInit(FUSE_CONTEXT *Context);
VOID FuseProtoSendLookup(FUSE_CONTEXT *Context);
NTSTATUS FuseProtoPostForget(PDEVICE_OBJECT DeviceObject, PLIST_ENTRY ForgetList);
static VOID FuseProtoPostForget_ContextFini(FUSE_CONTEXT *Context);
VOID FuseProtoFillForget(FUSE_CONTEXT *Context);
VOID FuseProtoFillBatchForget(FUSE_CONTEXT *Context);
VOID FuseProtoSendGetattr(FUSE_CONTEXT *Context);
VOID FuseProtoSendCreate(FUSE_CONTEXT *Context);
VOID FuseProtoSendOpen(FUSE_CONTEXT *Context);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, FuseProtoPostInit)
#pragma alloc_text(PAGE, FuseProtoSendInit)
#pragma alloc_text(PAGE, FuseProtoSendLookup)
#pragma alloc_text(PAGE, FuseProtoPostForget)
#pragma alloc_text(PAGE, FuseProtoPostForget_ContextFini)
#pragma alloc_text(PAGE, FuseProtoFillForget)
#pragma alloc_text(PAGE, FuseProtoFillBatchForget)
#pragma alloc_text(PAGE, FuseProtoSendGetattr)
#pragma alloc_text(PAGE, FuseProtoSendCreate)
#pragma alloc_text(PAGE, FuseProtoSendOpen)
#endif

static inline VOID FuseProtoInitRequest(FUSE_CONTEXT *Context,
    UINT32 len, UINT32 opcode, UINT64 nodeid)
{
    Context->FuseRequest->len = len;
    Context->FuseRequest->opcode = opcode;
    Context->FuseRequest->unique = (UINT64)(UINT_PTR)Context;
    Context->FuseRequest->nodeid = nodeid;
    Context->FuseRequest->uid = Context->OrigUid;
    Context->FuseRequest->gid = Context->OrigGid;
    Context->FuseRequest->pid = Context->OrigPid;
}

NTSTATUS FuseProtoPostInit(PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();

    FUSE_CONTEXT *Context;

    FuseContextCreate(&Context, DeviceObject, 0);
    ASSERT(0 != Context);
    if (FuseContextIsStatus(Context))
        return FuseContextToStatus(Context);

    Context->InternalResponse->Hint = FUSE_PROTO_OPCODE_INIT;

    FuseIoqPostPending(FuseDeviceExtension(DeviceObject)->Ioq, Context);

    return STATUS_SUCCESS;
}

VOID FuseProtoSendInit(FUSE_CONTEXT *Context)
{
    PAGED_CODE();

    coro_block (Context->CoroState)
    {
        FuseProtoInitRequest(Context,
            FUSE_PROTO_REQ_SIZE(init), FUSE_PROTO_OPCODE_INIT, 0);
        Context->FuseRequest->req.init.major = FUSE_PROTO_VERSION;
        Context->FuseRequest->req.init.minor = FUSE_PROTO_MINOR_VERSION;
        Context->FuseRequest->req.init.max_readahead = 0;   /* !!!: REVISIT */
        Context->FuseRequest->req.init.flags = 0;           /* !!!: REVISIT */
        coro_yield;

        if (0 != Context->FuseResponse->error)
            Context->InternalResponse->IoStatus.Status =
                FuseNtStatusFromErrno(Context->FuseResponse->error);
        coro_break;
    }
}

VOID FuseProtoSendLookup(FUSE_CONTEXT *Context)
{
    PAGED_CODE();

    coro_block (Context->CoroState)
    {
        FuseProtoInitRequest(Context,
            (UINT32)(FUSE_PROTO_REQ_SIZE(lookup) + Context->Name.Length + 1),
            FUSE_PROTO_OPCODE_LOOKUP, Context->Ino);
        ASSERT(FUSE_PROTO_REQ_SIZEMIN >= Context->FuseRequest->len);
        RtlCopyMemory(Context->FuseRequest->req.lookup.name, Context->Name.Buffer,
            Context->Name.Length);
        Context->FuseRequest->req.lookup.name[Context->Name.Length] = '\0';
        coro_yield;

        if (0 != Context->FuseResponse->error)
            Context->InternalResponse->IoStatus.Status =
                FuseNtStatusFromErrno(Context->FuseResponse->error);
        coro_break;
    }
}

NTSTATUS FuseProtoPostForget(PDEVICE_OBJECT DeviceObject, PLIST_ENTRY ForgetList)
{
    PAGED_CODE();

    FUSE_CONTEXT *Context;

    FuseContextCreate(&Context, DeviceObject, 0);
    ASSERT(0 != Context);
    if (FuseContextIsStatus(Context))
        return FuseContextToStatus(Context);

    Context->Fini = FuseProtoPostForget_ContextFini;
    Context->InternalResponse->Hint = FUSE_PROTO_OPCODE_FORGET;

    ASSERT(ForgetList != ForgetList->Flink);
    Context->ForgetList = *ForgetList;
    /* fixup first/last list entry */
    Context->ForgetList.Flink->Blink = &Context->ForgetList;
    Context->ForgetList.Blink->Flink = &Context->ForgetList;

    FuseIoqPostPending(FuseDeviceExtension(DeviceObject)->Ioq, Context);

    return STATUS_SUCCESS;
}

static VOID FuseProtoPostForget_ContextFini(FUSE_CONTEXT *Context)
{
    PAGED_CODE();

    FuseCacheDeleteItems(&Context->ForgetList);
}

VOID FuseProtoFillForget(FUSE_CONTEXT *Context)
{
    PAGED_CODE();

    UINT64 Ino;
    BOOLEAN Ok;

    Ok = FuseCacheForgetNextItem(&Context->ForgetList, &Ino);
    ASSERT(Ok);

    FuseProtoInitRequest(Context,
        FUSE_PROTO_REQ_SIZE(forget), FUSE_PROTO_OPCODE_FORGET, Ino);
    Context->FuseRequest->req.forget.nlookup = 1;
}

VOID FuseProtoFillBatchForget(FUSE_CONTEXT *Context)
{
    PAGED_CODE();

    UINT64 Ino;
    FUSE_PROTO_FORGET_ONE *StartP, *EndP, *P;

    StartP = (PVOID)((PUINT8)Context->FuseRequest + FUSE_PROTO_REQ_SIZE(batch_forget));
    EndP = (PVOID)((PUINT8)StartP + (FUSE_PROTO_REQ_SIZEMIN - FUSE_PROTO_REQ_SIZE(batch_forget)));
    for (P = StartP; EndP > P && FuseCacheForgetNextItem(&Context->ForgetList, &Ino); P++)
    {
        P->nodeid = Ino;
        P->nlookup = 1;
    }

    FuseProtoInitRequest(Context,
        (UINT32)((PUINT8)P - (PUINT8)Context->FuseRequest), FUSE_PROTO_OPCODE_BATCH_FORGET, 0);
    ASSERT(FUSE_PROTO_REQ_SIZEMIN >= Context->FuseRequest->len);
    Context->FuseRequest->req.batch_forget.count = (ULONG)(P - StartP);
}

VOID FuseProtoSendGetattr(FUSE_CONTEXT *Context)
{
    PAGED_CODE();

    coro_block (Context->CoroState)
    {
        FuseProtoInitRequest(Context,
            FUSE_PROTO_REQ_SIZE(getattr), FUSE_PROTO_OPCODE_GETATTR, Context->Ino);
        coro_yield;

        if (0 != Context->FuseResponse->error)
            Context->InternalResponse->IoStatus.Status =
                FuseNtStatusFromErrno(Context->FuseResponse->error);
        coro_break;
    }
}

VOID FuseProtoSendCreate(FUSE_CONTEXT *Context)
{
    PAGED_CODE();

    coro_block (Context->CoroState)
    {
        coro_break;
    }
}

VOID FuseProtoSendOpen(FUSE_CONTEXT *Context)
{
    PAGED_CODE();

    coro_block (Context->CoroState)
    {
        coro_break;
    }
}