/**
 * @file winfuse/util.c
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

NTSTATUS FuseGetTokenUid(HANDLE Token, TOKEN_INFORMATION_CLASS InfoClass, PUINT32 PUid);
NTSTATUS FuseSendTransactInternalIrp(PDEVICE_OBJECT DeviceObject, PFILE_OBJECT FileObject,
    FSP_FSCTL_TRANSACT_RSP *Response, FSP_FSCTL_TRANSACT_REQ **PRequest);
static NTSTATUS FuseSendIrpCompletion(
    PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context0);
NTSTATUS FuseNtStatusFromErrno(INT32 Errno);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, FuseGetTokenUid)
#pragma alloc_text(PAGE, FuseSendTransactInternalIrp)
// !#pragma alloc_text(PAGE, FuseSendIrpCompletion)
#pragma alloc_text(PAGE, FuseNtStatusFromErrno)
#endif

NTSTATUS FuseGetTokenUid(HANDLE Token, TOKEN_INFORMATION_CLASS InfoClass, PUINT32 PUid)
{
    PAGED_CODE();

    PSID *PSid;
    struct
    {
        union
        {
            TOKEN_USER U;
            TOKEN_OWNER O;
            TOKEN_PRIMARY_GROUP G;
        } u;
        FSP_FSCTL_DECLSPEC_ALIGN UINT8 B[SECURITY_MAX_SID_SIZE];
    } InfoBuf;
    PVOID Info = &InfoBuf;
    ULONG Size;
    NTSTATUS Result;

    switch (InfoClass)
    {
    case TokenUser:
        PSid = &((PTOKEN_USER)Info)->User.Sid;
        break;
    case TokenOwner:
        PSid = &((PTOKEN_OWNER)Info)->Owner;
        break;
    case TokenPrimaryGroup:
        PSid = &((PTOKEN_PRIMARY_GROUP)Info)->PrimaryGroup;
        break;
    default:
        ASSERT(0);
        return STATUS_INVALID_PARAMETER;
    }

    Result = ZwQueryInformationToken(Token, InfoClass, Info, sizeof InfoBuf, &Size);
    if (!NT_SUCCESS(Result))
        return Result;

    return FspPosixMapSidToUid(*PSid, PUid);
}

typedef struct
{
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
} FUSE_SEND_IRP_CONTEXT;

NTSTATUS FuseSendTransactInternalIrp(PDEVICE_OBJECT DeviceObject, PFILE_OBJECT FileObject,
    FSP_FSCTL_TRANSACT_RSP *Response, FSP_FSCTL_TRANSACT_REQ **PRequest)
{
    PAGED_CODE();

    NTSTATUS Result;
    PIRP Irp;
    PIO_STACK_LOCATION IrpSp;

    if (0 != PRequest)
        *PRequest = 0;

    if (0 == DeviceObject)
        DeviceObject = IoGetRelatedDeviceObject(FileObject);

    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (0 == Irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    IrpSp = IoGetNextIrpStackLocation(Irp);
    Irp->RequestorMode = KernelMode;
    Irp->UserBuffer = PRequest;
    IrpSp->MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL;
    IrpSp->MinorFunction = IRP_MN_USER_FS_REQUEST;
    IrpSp->FileObject = FileObject;
    IrpSp->Parameters.FileSystemControl.FsControlCode = FSP_FSCTL_TRANSACT_INTERNAL;
    IrpSp->Parameters.FileSystemControl.OutputBufferLength = 0 != PRequest ? sizeof(PVOID) : 0;
    IrpSp->Parameters.FileSystemControl.InputBufferLength = 0 != Response ? Response->Size : 0;
    IrpSp->Parameters.FileSystemControl.Type3InputBuffer = Response;

    IoSetCompletionRoutine(Irp, FuseSendIrpCompletion, 0, TRUE, TRUE, TRUE);

    Result = IoCallDriver(DeviceObject, Irp);
    ASSERT(STATUS_PENDING != Result);

    return Result;
}

static NTSTATUS FuseSendIrpCompletion(
    PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context0)
{
    // !PAGED_CODE();

    if (0 != Context0)
    {
        FUSE_SEND_IRP_CONTEXT *Context = Context0;

        Context->IoStatus = Irp->IoStatus;
        KeSetEvent(&Context->Event, 1, FALSE);
    }

    IoFreeIrp(Irp);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS FuseNtStatusFromErrno(INT32 Errno)
{
    PAGED_CODE();

    switch (Errno)
    {
    #undef FUSE_ERRNO
    #define FUSE_ERRNO 87
    #include <winfuse/errno.i>
    default:
        return STATUS_ACCESS_DENIED;
    }
}