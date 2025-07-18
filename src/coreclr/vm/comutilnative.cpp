// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//

//

/*============================================================
**
** File:  COMUtilNative
**
**
**
** Purpose: A dumping ground for classes which aren't large
** enough to get their own file in the EE.
**
**
**
===========================================================*/
#include "common.h"
#include "object.h"
#include "excep.h"
#include "vars.hpp"
#include "comutilnative.h"

#include "utilcode.h"
#include "frames.h"
#include "field.h"
#include "winwrap.h"
#include "gcheaputilities.h"
#include "fcall.h"
#include "invokeutil.h"
#include "eeconfig.h"
#include "typestring.h"
#include "finalizerthread.h"
#include "threadsuspend.h"

#ifdef FEATURE_COMINTEROP
    #include "comcallablewrapper.h"
    #include "comcache.h"
#endif // FEATURE_COMINTEROP

#include "arraynative.inl"

//
//
// EXCEPTION NATIVE
//
//
FCIMPL1(FC_BOOL_RET, ExceptionNative::IsImmutableAgileException, Object* pExceptionUNSAFE)
{
    FCALL_CONTRACT;

    ASSERT(pExceptionUNSAFE != NULL);

    OBJECTREF pException = (OBJECTREF) pExceptionUNSAFE;

    // The preallocated exception objects may be used from multiple AppDomains
    // and therefore must remain immutable from the application's perspective.
    FC_RETURN_BOOL(CLRException::IsPreallocatedExceptionObject(pException));
}
FCIMPLEND

// This FCall sets a flag against the thread exception state to indicate to
// IL_Throw and the StackTraceInfo implementation to account for the fact
// that we have restored a foreign exception dispatch details.
//
// Refer to the respective methods for details on how they use this flag.
FCIMPL0(VOID, ExceptionNative::PrepareForForeignExceptionRaise)
{
    FCALL_CONTRACT;

    PTR_ThreadExceptionState pCurTES = GetThread()->GetExceptionState();

    // Set a flag against the TES to indicate this is a foreign exception raise.
    pCurTES->SetRaisingForeignException();
}
FCIMPLEND

// Given an exception object, this method will mark its stack trace as frozen and return it to the caller.
// Frozen stack traces are immutable, when a thread attempts to add a frame to it, the stack trace is cloned first.
extern "C" void QCALLTYPE ExceptionNative_GetFrozenStackTrace(QCall::ObjectHandleOnStack exception, QCall::ObjectHandleOnStack ret)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    GCX_COOP();

    _ASSERTE(exception.Get() != NULL);

    struct
    {
        StackTraceArray stackTrace;
        EXCEPTIONREF refException = NULL;
        PTRARRAYREF keepAliveArray = NULL; // Object array of Managed Resolvers / AssemblyLoadContexts
    } gc;
    GCPROTECT_BEGIN(gc);

    // Get the exception object reference
    gc.refException = (EXCEPTIONREF)exception.Get();

    gc.refException->GetStackTrace(gc.stackTrace, &gc.keepAliveArray);

    gc.stackTrace.MarkAsFrozen();

    if (gc.keepAliveArray != NULL)
    {
        ret.Set(gc.keepAliveArray);
    }
    else
    {
        ret.Set(gc.stackTrace.Get());
    }
    GCPROTECT_END();

    END_QCALL;
}

#ifdef FEATURE_COMINTEROP

static BSTR BStrFromString(STRINGREF s)
{
    CONTRACTL
    {
        THROWS;
    }
    CONTRACTL_END;

    WCHAR *wz;
    int cch;
    BSTR bstr;

    if (s == NULL)
        return NULL;

    s->RefInterpretGetStringValuesDangerousForGC(&wz, &cch);

    bstr = SysAllocString(wz);
    if (bstr == NULL)
        COMPlusThrowOM();

    return bstr;
}

static BSTR GetExceptionDescription(OBJECTREF objException)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
        PRECONDITION( IsException(objException->GetMethodTable()) );
    }
    CONTRACTL_END;

    BSTR bstrDescription;

    STRINGREF MessageString = NULL;
    GCPROTECT_BEGIN(MessageString)
    GCPROTECT_BEGIN(objException)
    {
        // read Exception.Message property
        MethodDescCallSite getMessage(METHOD__EXCEPTION__GET_MESSAGE, &objException);

        ARG_SLOT GetMessageArgs[] = { ObjToArgSlot(objException)};
        MessageString = getMessage.Call_RetSTRINGREF(GetMessageArgs);

        // if the message string is empty then use the exception classname.
        if (MessageString == NULL || MessageString->GetStringLength() == 0) {
            // call GetClassName
            MethodDescCallSite getClassName(METHOD__EXCEPTION__GET_CLASS_NAME, &objException);
            ARG_SLOT GetClassNameArgs[] = { ObjToArgSlot(objException)};
            MessageString = getClassName.Call_RetSTRINGREF(GetClassNameArgs);
            _ASSERTE(MessageString != NULL && MessageString->GetStringLength() != 0);
        }

        // Allocate the description BSTR.
        int DescriptionLen = MessageString->GetStringLength();
        bstrDescription = SysAllocStringLen(MessageString->GetBuffer(), DescriptionLen);
    }
    GCPROTECT_END();
    GCPROTECT_END();

    return bstrDescription;
}

static BSTR GetExceptionSource(OBJECTREF objException)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
        PRECONDITION( IsException(objException->GetMethodTable()) );
    }
    CONTRACTL_END;

    STRINGREF refRetVal;
    GCPROTECT_BEGIN(objException)

    // read Exception.Source property
    MethodDescCallSite getSource(METHOD__EXCEPTION__GET_SOURCE, &objException);

    ARG_SLOT GetSourceArgs[] = { ObjToArgSlot(objException)};

    refRetVal = getSource.Call_RetSTRINGREF(GetSourceArgs);

    GCPROTECT_END();
    return BStrFromString(refRetVal);
}

static void GetExceptionHelp(OBJECTREF objException, BSTR *pbstrHelpFile, DWORD *pdwHelpContext)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
        INJECT_FAULT(COMPlusThrowOM());
        PRECONDITION(IsException(objException->GetMethodTable()));
        PRECONDITION(CheckPointer(pbstrHelpFile));
        PRECONDITION(CheckPointer(pdwHelpContext));
    }
    CONTRACTL_END;

    *pdwHelpContext = 0;

    GCPROTECT_BEGIN(objException);

    // call managed code to parse help context
    MethodDescCallSite getHelpContext(METHOD__EXCEPTION__GET_HELP_CONTEXT, &objException);

    ARG_SLOT GetHelpContextArgs[] =
    {
        ObjToArgSlot(objException),
        PtrToArgSlot(pdwHelpContext)
    };
    *pbstrHelpFile = BStrFromString(getHelpContext.Call_RetSTRINGREF(GetHelpContextArgs));

    GCPROTECT_END();
}

// NOTE: caller cleans up any partially initialized BSTRs in pED
void ExceptionNative::GetExceptionData(OBJECTREF objException, ExceptionData *pED)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
        PRECONDITION(IsException(objException->GetMethodTable()));
        PRECONDITION(CheckPointer(pED));
    }
    CONTRACTL_END;

    ZeroMemory(pED, sizeof(ExceptionData));

    GCPROTECT_BEGIN(objException);
    pED->hr = GetExceptionHResult(objException);
    pED->bstrDescription = GetExceptionDescription(objException);
    pED->bstrSource = GetExceptionSource(objException);
    GetExceptionHelp(objException, &pED->bstrHelpFile, &pED->dwHelpContext);
    GCPROTECT_END();
    return;
}

HRESULT SimpleComCallWrapper::IErrorInfo_hr()
{
    WRAPPER_NO_CONTRACT;
    return GetExceptionHResult(this->GetObjectRef());
}

BSTR SimpleComCallWrapper::IErrorInfo_bstrDescription()
{
    WRAPPER_NO_CONTRACT;
    return GetExceptionDescription(this->GetObjectRef());
}

BSTR SimpleComCallWrapper::IErrorInfo_bstrSource()
{
    WRAPPER_NO_CONTRACT;
    return GetExceptionSource(this->GetObjectRef());
}

BSTR SimpleComCallWrapper::IErrorInfo_bstrHelpFile()
{
    WRAPPER_NO_CONTRACT;
    BSTR  bstrHelpFile;
    DWORD dwHelpContext;
    GetExceptionHelp(this->GetObjectRef(), &bstrHelpFile, &dwHelpContext);
    return bstrHelpFile;
}

DWORD SimpleComCallWrapper::IErrorInfo_dwHelpContext()
{
    WRAPPER_NO_CONTRACT;
    BSTR  bstrHelpFile;
    DWORD dwHelpContext;
    GetExceptionHelp(this->GetObjectRef(), &bstrHelpFile, &dwHelpContext);
    SysFreeString(bstrHelpFile);
    return dwHelpContext;
}

GUID SimpleComCallWrapper::IErrorInfo_guid()
{
    LIMITED_METHOD_CONTRACT;
    return GUID_NULL;
}

#endif // FEATURE_COMINTEROP

FCIMPL0(EXCEPTION_POINTERS*, ExceptionNative::GetExceptionPointers)
{
    FCALL_CONTRACT;

    EXCEPTION_POINTERS* retVal = NULL;

    Thread *pThread = GetThread();
    if (pThread->IsExceptionInProgress())
    {
        retVal = pThread->GetExceptionState()->GetExceptionPointers();
    }

    return retVal;
}
FCIMPLEND

FCIMPL0(INT32, ExceptionNative::GetExceptionCode)
{
    FCALL_CONTRACT;

    INT32 retVal = 0;

    Thread *pThread = GetThread();
    if (pThread->IsExceptionInProgress())
    {
        retVal = pThread->GetExceptionState()->GetExceptionCode();
    }

    return retVal;
}
FCIMPLEND

extern uint32_t g_exceptionCount;
FCIMPL0(UINT32, ExceptionNative::GetExceptionCount)
{
    FCALL_CONTRACT;
    return g_exceptionCount;
}
FCIMPLEND


//
// This must be implemented as an FCALL because managed code cannot
// swallow a thread abort exception without resetting the abort,
// which we don't want to do.  Additionally, we can run into deadlocks
// if we use the ResourceManager to do resource lookups - it requires
// taking managed locks when initializing Globalization & Security,
// but a thread abort on a separate thread initializing those same
// systems would also do a resource lookup via the ResourceManager.
// We've deadlocked in CompareInfo.GetCompareInfo &
// Environment.GetResourceString.  It's not practical to take all of
// our locks within CER's to avoid this problem - just use the CLR's
// unmanaged resources.
//
extern "C" void QCALLTYPE ExceptionNative_GetMessageFromNativeResources(ExceptionMessageKind kind, QCall::StringHandleOnStack retMesg)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    SString buffer;
    HRESULT hr = S_OK;
    const WCHAR * wszFallbackString = NULL;

    switch(kind) {
    case ExceptionMessageKind::ThreadAbort:
        hr = buffer.LoadResourceAndReturnHR(CCompRC::Error, IDS_EE_THREAD_ABORT);
        if (FAILED(hr)) {
            wszFallbackString = W("Thread was being aborted.");
        }
        break;

    case ExceptionMessageKind::ThreadInterrupted:
        hr = buffer.LoadResourceAndReturnHR(CCompRC::Error, IDS_EE_THREAD_INTERRUPTED);
        if (FAILED(hr)) {
            wszFallbackString = W("Thread was interrupted from a waiting state.");
        }
        break;

    case ExceptionMessageKind::OutOfMemory:
        hr = buffer.LoadResourceAndReturnHR(CCompRC::Error, IDS_EE_OUT_OF_MEMORY);
        if (FAILED(hr)) {
            wszFallbackString = W("Insufficient memory to continue the execution of the program.");
        }
        break;

    default:
        _ASSERTE(!"Unknown ExceptionMessageKind value!");
    }
    if (FAILED(hr)) {
        STRESS_LOG1(LF_BCL, LL_ALWAYS, "LoadResource error: %x", hr);
        _ASSERTE(wszFallbackString != NULL);
        retMesg.Set(wszFallbackString);
    }
    else {
        retMesg.Set(buffer);
    }

    END_QCALL;
}

extern "C" void QCALLTYPE ExceptionNative_GetMethodFromStackTrace(QCall::ObjectHandleOnStack stacktrace, QCall::ObjectHandleOnStack retMethodInfo)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    GCX_COOP();

    MethodDesc* pMD = NULL;
    // See ExceptionObject::GetStackTrace() and ExceptionObject::SetStackTrace()
    // for details on the stacktrace array.
    {
        ARRAYBASEREF arrayBaseRef = (ARRAYBASEREF)stacktrace.Get();
        _ASSERTE(arrayBaseRef != NULL);

        // The stacktrace can be either sbyte[] or Object[]. In the latter case,
        // the first entry is the actual stack trace sbyte[], the rest are pointers
        // to the method info objects. We only care about the first entry here.
        CorElementType elemType = arrayBaseRef->GetArrayElementType();
        if (elemType != ELEMENT_TYPE_I1)
        {
            _ASSERTE(elemType == ELEMENT_TYPE_CLASS); // object[]
            PTRARRAYREF ptrArrayRef = (PTRARRAYREF)arrayBaseRef;
            arrayBaseRef = (ARRAYBASEREF)OBJECTREFToObject(ptrArrayRef->GetAt(0));
        }

        I1ARRAYREF arrayRef = (I1ARRAYREF)arrayBaseRef;
        StackTraceArray stackArray(arrayRef);
        _ASSERTE(stackArray.Size() > 0);
        pMD = stackArray[0].pFunc;
    }

    // The managed stack trace classes always return typical method definition,
    // so we don't need to bother providing exact instantiation.
    MethodDesc* pMDTypical = pMD->LoadTypicalMethodDefinition();
    retMethodInfo.Set(pMDTypical->AllocateStubMethodInfo());
    _ASSERTE(pMDTypical->IsRuntimeMethodHandle());

    END_QCALL;
}

extern "C" void QCALLTYPE ExceptionNative_ThrowAmbiguousResolutionException(
    MethodTable* pTargetClass,
    MethodTable* pInterfaceMT,
    MethodDesc* pInterfaceMD)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    ThrowAmbiguousResolutionException(pTargetClass, pInterfaceMT, pInterfaceMD);

    END_QCALL;
}

extern "C" void QCALLTYPE ExceptionNative_ThrowEntryPointNotFoundException(
    MethodTable* pTargetClass,
    MethodTable* pInterfaceMT,
    MethodDesc* pInterfaceMD)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    ThrowEntryPointNotFoundException(pTargetClass, pInterfaceMT, pInterfaceMD);

    END_QCALL;
}

extern "C" void QCALLTYPE ExceptionNative_ThrowMethodAccessException(MethodDesc* caller, MethodDesc* callee)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    _ASSERTE(caller != NULL);
    AccessCheckContext accessContext(caller);
    ThrowMethodAccessException(&accessContext, callee);

    END_QCALL;
}

extern "C" void QCALLTYPE ExceptionNative_ThrowFieldAccessException(MethodDesc* caller, FieldDesc* callee)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    _ASSERTE(caller != NULL);
    AccessCheckContext accessContext(caller);
    ThrowFieldAccessException(&accessContext, callee);

    END_QCALL;
}

extern "C" void QCALLTYPE ExceptionNative_ThrowClassAccessException(MethodDesc* caller, EnregisteredTypeHandle callee)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    _ASSERTE(caller != NULL);
    AccessCheckContext accessContext(caller);
    ThrowTypeAccessException(&accessContext, TypeHandle::FromPtr(callee).GetMethodTable());

    END_QCALL;
}

extern "C" void QCALLTYPE Buffer_Clear(void *dst, size_t length)
{
    QCALL_CONTRACT;

#if defined(HOST_X86) || defined(HOST_AMD64)
    if (length > 0x100)
    {
        // memset ends up calling rep stosb if the hardware claims to support it efficiently. rep stosb is up to 2x slower
        // on misaligned blocks. Workaround this issue by aligning the blocks passed to memset upfront.

        *(uint64_t*)dst = 0;
        *((uint64_t*)dst + 1) = 0;
        *((uint64_t*)dst + 2) = 0;
        *((uint64_t*)dst + 3) = 0;

        void* end = (uint8_t*)dst + length;
        *((uint64_t*)end - 1) = 0;
        *((uint64_t*)end - 2) = 0;
        *((uint64_t*)end - 3) = 0;
        *((uint64_t*)end - 4) = 0;

        dst = ALIGN_UP((uint8_t*)dst + 1, 32);
        length = ALIGN_DOWN((uint8_t*)end - 1, 32) - (uint8_t*)dst;
    }
#endif

    memset(dst, 0, length);
}

extern "C" void QCALLTYPE Buffer_MemMove(void *dst, void *src, size_t length)
{
    QCALL_CONTRACT;

    memmove(dst, src, length);
}

FCIMPL3(VOID, Buffer::BulkMoveWithWriteBarrier, void *dst, void *src, size_t byteCount)
{
    FCALL_CONTRACT;

    if (dst != src && byteCount != 0)
        InlinedMemmoveGCRefsHelper(dst, src, byteCount);
}
FCIMPLEND

//
// GCInterface
//
INT32    GCInterface::m_gc_counts[3] = {0,0,0};
UINT64   GCInterface::m_addPressure[MEM_PRESSURE_COUNT] = {0, 0, 0, 0};   // history of memory pressure additions
UINT64   GCInterface::m_remPressure[MEM_PRESSURE_COUNT] = {0, 0, 0, 0};   // history of memory pressure removals

// incremented after a gen2 GC has been detected,
// (m_iteration % MEM_PRESSURE_COUNT) is used as an index into m_addPressure and m_remPressure
UINT     GCInterface::m_iteration = 0;

FCIMPL0(INT64, GCInterface::GetTotalPauseDuration)
{
    FCALL_CONTRACT;

    return GCHeapUtilities::GetGCHeap()->GetTotalPauseDuration();
}
FCIMPLEND

FCIMPL2(void, GCInterface::GetMemoryInfo, Object* objUNSAFE, int kind)
{
    FCALL_CONTRACT;

    GCMEMORYINFODATAREF objGCMemoryInfo = (GCMEMORYINFODATAREF)(ObjectToOBJECTREF (objUNSAFE));

    UINT64* genInfoRaw = (UINT64*)&(objGCMemoryInfo->generationInfo0);
    UINT64* pauseInfoRaw = (UINT64*)&(objGCMemoryInfo->pauseDuration0);

    return GCHeapUtilities::GetGCHeap()->GetMemoryInfo(
        &(objGCMemoryInfo->highMemLoadThresholdBytes),
        &(objGCMemoryInfo->totalAvailableMemoryBytes),
        &(objGCMemoryInfo->lastRecordedMemLoadBytes),
        &(objGCMemoryInfo->lastRecordedHeapSizeBytes),
        &(objGCMemoryInfo->lastRecordedFragmentationBytes),
        &(objGCMemoryInfo->totalCommittedBytes),
        &(objGCMemoryInfo->promotedBytes),
        &(objGCMemoryInfo->pinnedObjectCount),
        &(objGCMemoryInfo->finalizationPendingCount),
        &(objGCMemoryInfo->index),
        &(objGCMemoryInfo->generation),
        &(objGCMemoryInfo->pauseTimePercent),
        (bool*)&(objGCMemoryInfo->isCompaction),
        (bool*)&(objGCMemoryInfo->isConcurrent),
        genInfoRaw,
        pauseInfoRaw,
        kind);
}
FCIMPLEND

FCIMPL0(UINT32, GCInterface::GetMemoryLoad)
{
    FCALL_CONTRACT;

    int result = (INT32)GCHeapUtilities::GetGCHeap()->GetMemoryLoad();
    return result;
}
FCIMPLEND

FCIMPL0(int, GCInterface::GetGcLatencyMode)
{
    FCALL_CONTRACT;

    int result = (INT32)GCHeapUtilities::GetGCHeap()->GetGcLatencyMode();
    return result;
}
FCIMPLEND

FCIMPL1(int, GCInterface::SetGcLatencyMode, int newLatencyMode)
{
    FCALL_CONTRACT;

    return GCHeapUtilities::GetGCHeap()->SetGcLatencyMode(newLatencyMode);
}
FCIMPLEND

FCIMPL0(int, GCInterface::GetLOHCompactionMode)
{
    FCALL_CONTRACT;

    int result = (INT32)GCHeapUtilities::GetGCHeap()->GetLOHCompactionMode();
    return result;
}
FCIMPLEND

FCIMPL1(void, GCInterface::SetLOHCompactionMode, int newLOHCompactionyMode)
{
    FCALL_CONTRACT;

    GCHeapUtilities::GetGCHeap()->SetLOHCompactionMode(newLOHCompactionyMode);
}
FCIMPLEND


FCIMPL2(FC_BOOL_RET, GCInterface::RegisterForFullGCNotification, UINT32 gen2Percentage, UINT32 lohPercentage)
{
    FCALL_CONTRACT;

    FC_RETURN_BOOL(GCHeapUtilities::GetGCHeap()->RegisterForFullGCNotification(gen2Percentage, lohPercentage));
}
FCIMPLEND

FCIMPL0(FC_BOOL_RET, GCInterface::CancelFullGCNotification)
{
    FCALL_CONTRACT;

    FC_RETURN_BOOL(GCHeapUtilities::GetGCHeap()->CancelFullGCNotification());
}
FCIMPLEND

extern "C" int QCALLTYPE GCInterface_WaitForFullGCApproach(int millisecondsTimeout)
{
    QCALL_CONTRACT;

    int result = 0;

    BEGIN_QCALL;

    GCX_COOP();

    DWORD dwMilliseconds = ((millisecondsTimeout == -1) ? INFINITE : millisecondsTimeout);
    result = GCHeapUtilities::GetGCHeap()->WaitForFullGCApproach(dwMilliseconds);

    END_QCALL;

    return result;
}

extern "C" int QCALLTYPE GCInterface_WaitForFullGCComplete(int millisecondsTimeout)
{
    QCALL_CONTRACT;

    int result = 0;

    BEGIN_QCALL;

    GCX_COOP();

    DWORD dwMilliseconds = ((millisecondsTimeout == -1) ? INFINITE : millisecondsTimeout);
    result = GCHeapUtilities::GetGCHeap()->WaitForFullGCComplete(dwMilliseconds);

    END_QCALL;

    return result;
}

/*================================GetGenerationInternal=================================
**Action: Returns the generation in which args->obj is found.
**Returns: The generation in which args->obj is found.
**Arguments: args->obj -- The object to locate.
==============================================================================*/
FCIMPL1(int, GCInterface::GetGenerationInternal, Object* objUNSAFE)
{
    FCALL_CONTRACT;
    _ASSERTE(objUNSAFE != NULL);
    return (INT32)GCHeapUtilities::GetGCHeap()->WhichGeneration(objUNSAFE);
}
FCIMPLEND

/*================================GetSegmentSize========-=======================
**Action: Returns the maximum GC heap segment size
**Returns: The maximum segment size of either the normal heap or the large object heap, whichever is bigger
==============================================================================*/
FCIMPL0(UINT64, GCInterface::GetSegmentSize)
{
    FCALL_CONTRACT;

    IGCHeap * pGC = GCHeapUtilities::GetGCHeap();
    size_t segment_size = pGC->GetValidSegmentSize(false);
    size_t large_segment_size = pGC->GetValidSegmentSize(true);
    _ASSERTE(segment_size < SIZE_T_MAX && large_segment_size < SIZE_T_MAX);
    if (segment_size < large_segment_size)
        segment_size = large_segment_size;
    return (UINT64) segment_size;
}
FCIMPLEND

/*================================CollectionCount=================================
**Action: Returns the number of collections for this generation since the beginning of the life of the process
**Returns: The collection count.
**Arguments: args->generation -- The generation
**Exceptions: Argument exception if args->generation is < 0 or > GetMaxGeneration();
==============================================================================*/
FCIMPL2(int, GCInterface::CollectionCount, INT32 generation, INT32 getSpecialGCCount)
{
    FCALL_CONTRACT;

    //We've already checked this in GC.cs, so we'll just assert it here.
    _ASSERTE(generation >= 0);

    //We don't need to check the top end because the GC will take care of that.
    return (INT32)GCHeapUtilities::GetGCHeap()->CollectionCount(generation, getSpecialGCCount);
}
FCIMPLEND

extern "C" int QCALLTYPE GCInterface_StartNoGCRegion(INT64 totalSize, BOOL lohSizeKnown, INT64 lohSize, BOOL disallowFullBlockingGC)
{
    QCALL_CONTRACT;

    int retVal = 0;

    BEGIN_QCALL;

    GCX_COOP();

    retVal = GCHeapUtilities::GetGCHeap()->StartNoGCRegion((ULONGLONG)totalSize,
                                                  !!lohSizeKnown,
                                                  (ULONGLONG)lohSize,
                                                  !!disallowFullBlockingGC);

    END_QCALL;

    return retVal;
}

extern "C" int QCALLTYPE GCInterface_EndNoGCRegion()
{
    QCALL_CONTRACT;

    int retVal = FALSE;

    BEGIN_QCALL;

    retVal = GCHeapUtilities::GetGCHeap()->EndNoGCRegion();

    END_QCALL;

    return retVal;
}

FCIMPL0(int, GCInterface::GetLastGCPercentTimeInGC)
{
    FCALL_CONTRACT;

    return GCHeapUtilities::GetGCHeap()->GetLastGCPercentTimeInGC();
}
FCIMPLEND

FCIMPL1(UINT64, GCInterface::GetGenerationSize, int gen)
{
    FCALL_CONTRACT;

    return (UINT64)(GCHeapUtilities::GetGCHeap()->GetLastGCGenerationSize(gen));
}
FCIMPLEND

/*================================GetTotalMemory================================
**Action: Returns the total number of bytes in use
**Returns: The total number of bytes in use
**Arguments: None
**Exceptions: None
==============================================================================*/
extern "C" INT64 QCALLTYPE GCInterface_GetTotalMemory()
{
    QCALL_CONTRACT;

    INT64 iRetVal = 0;

    BEGIN_QCALL;

    GCX_COOP();
    iRetVal = (INT64) GCHeapUtilities::GetGCHeap()->GetTotalBytesInUse();

    END_QCALL;

    return iRetVal;
}

/*==============================Collect=========================================
**Action: Collects all generations <= args->generation
**Returns: void
**Arguments: args->generation:  The maximum generation to collect
**Exceptions: Argument exception if args->generation is < 0 or > GetMaxGeneration();
==============================================================================*/
extern "C" void QCALLTYPE GCInterface_Collect(INT32 generation, INT32 mode, CLR_BOOL lowMemoryPressure)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    //We've already checked this in GC.cs, so we'll just assert it here.
    _ASSERTE(generation >= -1);

    //We don't need to check the top end because the GC will take care of that.

    GCX_COOP();
    GCHeapUtilities::GetGCHeap()->GarbageCollect(generation, lowMemoryPressure, mode);

    END_QCALL;
}

extern "C" void* QCALLTYPE GCInterface_GetNextFinalizableObject(QCall::ObjectHandleOnStack pObj)
{
    QCALL_CONTRACT;

    PCODE funcPtr = 0;

    BEGIN_QCALL;

    GCX_COOP();

    OBJECTREF target = FinalizerThread::GetNextFinalizableObject();

    if (target != NULL)
    {
        pObj.Set(target);

        MethodTable* pMT = target->GetMethodTable();

        funcPtr = pMT->GetRestoredSlot(g_pObjectFinalizerMD->GetSlot());
    }

    END_QCALL;

    return (void*)funcPtr;
}

/*==========================WaitForPendingFinalizers============================
**Action: Run all Finalizers that haven't been run.
**Arguments: None
**Exceptions: None
==============================================================================*/
extern "C" void QCALLTYPE GCInterface_WaitForPendingFinalizers()
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    FinalizerThread::FinalizerThreadWait();

    END_QCALL;
}


/*===============================GetMaxGeneration===============================
**Action: Returns the largest GC generation
**Returns: The largest GC Generation
**Arguments: None
**Exceptions: None
==============================================================================*/
FCIMPL0(int, GCInterface::GetMaxGeneration)
{
    FCALL_CONTRACT;

    return(INT32)GCHeapUtilities::GetGCHeap()->GetMaxGeneration();
}
FCIMPLEND

/*===============================GetAllocatedBytesForCurrentThread===============================
**Action: Computes the allocated bytes so far on the current thread
**Returns: The allocated bytes so far on the current thread
**Arguments: None
**Exceptions: None
==============================================================================*/
FCIMPL0(INT64, GCInterface::GetAllocatedBytesForCurrentThread)
{
    FCALL_CONTRACT;

    INT64 currentAllocated = 0;
    Thread *pThread = GetThread();
    gc_alloc_context* ac = &t_runtime_thread_locals.alloc_context.m_GCAllocContext;
    currentAllocated = ac->alloc_bytes + ac->alloc_bytes_uoh - (ac->alloc_limit - ac->alloc_ptr);

    return currentAllocated;
}
FCIMPLEND

/*===============================AllocateNewArray===============================
**Action: Allocates a new array object. Allows passing extra flags
**Arguments: typeHandlePtr -> TypeHandle pointer of array,
**           length -> Number of elements,
**           flags -> Flags that impact allocated memory,
**           ret -> The allocated array.
**Exceptions: IDS_EE_ARRAY_DIMENSIONS_EXCEEDED when size is too large. OOM if can't allocate.
==============================================================================*/
extern "C" void QCALLTYPE GCInterface_AllocateNewArray(void* typeHandlePtr, INT32 length, INT32 flags, QCall::ObjectHandleOnStack ret)
{
    QCALL_CONTRACT;
    _ASSERTE(typeHandlePtr != NULL);

    BEGIN_QCALL;

    GCX_COOP();

    TypeHandle typeHandle = TypeHandle::FromPtr(typeHandlePtr);
    _ASSERTE(typeHandle.IsArray());

    //Only the following flags are used by GC.cs, so we'll just assert it here.
    _ASSERTE((flags & ~(GC_ALLOC_ZEROING_OPTIONAL | GC_ALLOC_PINNED_OBJECT_HEAP)) == 0);
    ret.Set(AllocateSzArray(typeHandle, length, (GC_ALLOC_FLAGS)flags));

    END_QCALL;
}

FCIMPL0(INT64, GCInterface::GetTotalAllocatedBytesApproximate)
{
    FCALL_CONTRACT;

#ifdef TARGET_64BIT
    uint64_t unused_bytes = Thread::dead_threads_non_alloc_bytes;
#else
    // As it could be noticed we read 64bit values that may be concurrently updated.
    // Such reads are not guaranteed to be atomic on 32bit so extra care should be taken.
    uint64_t unused_bytes = InterlockedCompareExchange64((LONG64*)& Thread::dead_threads_non_alloc_bytes, 0, 0);
#endif

    uint64_t allocated_bytes = GCHeapUtilities::GetGCHeap()->GetTotalAllocatedBytes() - unused_bytes;

    // highest reported allocated_bytes. We do not want to report a value less than that even if unused_bytes has increased.
    static uint64_t high_watermark;

    uint64_t current_high = high_watermark;
    while (allocated_bytes > current_high)
    {
        uint64_t orig = InterlockedCompareExchange64((LONG64*)& high_watermark, allocated_bytes, current_high);
        if (orig == current_high)
            return allocated_bytes;

        current_high = orig;
    }

    return current_high;
}
FCIMPLEND;

extern "C" INT64 QCALLTYPE GCInterface_GetTotalAllocatedBytesPrecise()
{
    INT64 allocated = 0;

    BEGIN_QCALL;

    GCX_COOP();

    // We need to suspend/restart the EE to get each thread's
    // non-allocated memory from their allocation contexts

    ThreadSuspend::SuspendEE(ThreadSuspend::SUSPEND_OTHER);

    allocated = GCHeapUtilities::GetGCHeap()->GetTotalAllocatedBytes() - Thread::dead_threads_non_alloc_bytes;

    for (Thread *pThread = ThreadStore::GetThreadList(NULL); pThread; pThread = ThreadStore::GetThreadList(pThread))
    {
        gc_alloc_context* ac = pThread->GetAllocContext();
        if (ac != nullptr)
        {
            allocated -= ac->alloc_limit - ac->alloc_ptr;
        }
    }

    ThreadSuspend::RestartEE(FALSE, TRUE);

    END_QCALL;

    return allocated;
}

#ifdef FEATURE_BASICFREEZE

/*===============================RegisterFrozenSegment===============================
**Action: Registers the frozen segment
**Returns: segment_handle
**Arguments: args-> pointer to section, size of section
**Exceptions: None
==============================================================================*/
extern "C" void* QCALLTYPE GCInterface_RegisterFrozenSegment(void* pSection, SIZE_T sizeSection)
{
    QCALL_CONTRACT;

    void* retVal = nullptr;

    BEGIN_QCALL;

    _ASSERTE(pSection != nullptr);
    _ASSERTE(sizeSection > 0);

    GCX_COOP();

    segment_info seginfo;
    seginfo.pvMem           = pSection;
    seginfo.ibFirstObject   = sizeof(ObjHeader);
    seginfo.ibAllocated     = sizeSection;
    seginfo.ibCommit        = seginfo.ibAllocated;
    seginfo.ibReserved      = seginfo.ibAllocated;

    retVal = (void*)GCHeapUtilities::GetGCHeap()->RegisterFrozenSegment(&seginfo);

    END_QCALL;

    return retVal;
}

/*===============================UnregisterFrozenSegment===============================
**Action: Unregisters the frozen segment
**Returns: void
**Arguments: args-> segment handle
**Exceptions: None
==============================================================================*/
extern "C" void QCALLTYPE GCInterface_UnregisterFrozenSegment(void* segment)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    _ASSERTE(segment != nullptr);

    GCX_COOP();

    GCHeapUtilities::GetGCHeap()->UnregisterFrozenSegment((segment_handle)segment);

    END_QCALL;
}

#endif // FEATURE_BASICFREEZE

/*==============================SuppressFinalize================================
**Action: Indicate that an object's finalizer should not be run by the system
**Arguments: Object of interest
**Exceptions: None
==============================================================================*/
FCIMPL1(void, GCInterface::SuppressFinalize, Object *obj)
{
    FCALL_CONTRACT;

    _ASSERTE(obj->GetMethodTable ()->HasFinalizer());
    GCHeapUtilities::GetGCHeap()->SetFinalizationRun(obj);
}
FCIMPLEND


/*============================ReRegisterForFinalize==============================
**Action: Indicate that an object's finalizer should be run by the system.
**Arguments: Object of interest
**Exceptions: None
==============================================================================*/
extern "C" void QCALLTYPE GCInterface_ReRegisterForFinalize(QCall::ObjectHandleOnStack pObj)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    GCX_COOP();

    // Checked by the caller
    _ASSERTE(pObj.Get() != NULL);
    _ASSERTE(pObj.Get()->GetMethodTable()->HasFinalizer());

    if (!GCHeapUtilities::GetGCHeap()->RegisterForFinalization(-1, OBJECTREFToObject(pObj.Get())))
    {
        ThrowOutOfMemory();
    }
    END_QCALL;
}

FORCEINLINE UINT64 GCInterface::InterlockedAdd (UINT64 *pAugend, UINT64 addend) {
    WRAPPER_NO_CONTRACT;

    UINT64 oldMemValue;
    UINT64 newMemValue;

    do {
        oldMemValue = *pAugend;
        newMemValue = oldMemValue + addend;

        // check for overflow
        if (newMemValue < oldMemValue)
        {
            newMemValue = UINT64_MAX;
        }
    } while (InterlockedCompareExchange64((LONGLONG*) pAugend, (LONGLONG) newMemValue, (LONGLONG) oldMemValue) != (LONGLONG) oldMemValue);

    return newMemValue;
}

FORCEINLINE UINT64 GCInterface::InterlockedSub(UINT64 *pMinuend, UINT64 subtrahend) {
    WRAPPER_NO_CONTRACT;

    UINT64 oldMemValue;
    UINT64 newMemValue;

    do {
        oldMemValue = *pMinuend;
        newMemValue = oldMemValue - subtrahend;

        // check for underflow
        if (newMemValue > oldMemValue)
            newMemValue = 0;

    } while (InterlockedCompareExchange64((LONGLONG*) pMinuend, (LONGLONG) newMemValue, (LONGLONG) oldMemValue) != (LONGLONG) oldMemValue);

    return newMemValue;
}

extern "C" void QCALLTYPE GCInterface_AddMemoryPressure(UINT64 bytesAllocated)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;
    GCInterface::AddMemoryPressure(bytesAllocated);
    END_QCALL;
}

extern "C" void QCALLTYPE GCInterface_EnumerateConfigurationValues(void* configurationContext, EnumerateConfigurationValuesCallback callback)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;
    GCInterface::EnumerateConfigurationValues(configurationContext, callback);
    END_QCALL;
}

void GCInterface::EnumerateConfigurationValues(void* configurationContext, EnumerateConfigurationValuesCallback callback)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_PREEMPTIVE;
        PRECONDITION(configurationContext != nullptr);
        PRECONDITION(callback != nullptr);
    }
    CONTRACTL_END;

    IGCHeap* pHeap = GCHeapUtilities::GetGCHeap();
    pHeap->EnumerateConfigurationValues(configurationContext, callback);
}

GCHeapHardLimitInfo g_gcHeapHardLimitInfo;
bool g_gcHeapHardLimitInfoSpecified = false;

extern "C" int QCALLTYPE GCInterface_RefreshMemoryLimit(GCHeapHardLimitInfo heapHardLimitInfo)
{
    QCALL_CONTRACT;

    int result = 0;

    BEGIN_QCALL;
    g_gcHeapHardLimitInfo = heapHardLimitInfo;
    g_gcHeapHardLimitInfoSpecified = true;
    result = GCInterface::RefreshMemoryLimit();
    END_QCALL;

    return result;
}

int GCInterface::RefreshMemoryLimit()
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_PREEMPTIVE;
    }
    CONTRACTL_END;

    return GCHeapUtilities::GetGCHeap()->RefreshMemoryLimit();
}

extern "C" enable_no_gc_region_callback_status QCALLTYPE GCInterface_EnableNoGCRegionCallback(NoGCRegionCallbackFinalizerWorkItem* callback, INT64 totalSize)
{
    enable_no_gc_region_callback_status status = enable_no_gc_region_callback_status::succeed;
    QCALL_CONTRACT;

    BEGIN_QCALL;
    status = GCInterface::EnableNoGCRegionCallback(callback, totalSize);
    END_QCALL;
    return status;
}

enable_no_gc_region_callback_status GCInterface::EnableNoGCRegionCallback(NoGCRegionCallbackFinalizerWorkItem* callback, INT64 totalSize)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_PREEMPTIVE;
    }
    CONTRACTL_END;

    return GCHeapUtilities::GetGCHeap()->EnableNoGCRegionCallback(callback, totalSize);
}

extern "C" uint64_t QCALLTYPE GCInterface_GetGenerationBudget(int generation)
{
    uint64_t result = 0;
    QCALL_CONTRACT;

    BEGIN_QCALL;
    result = GCInterface::GetGenerationBudget(generation);
    END_QCALL;

    return result;
}

uint64_t GCInterface::GetGenerationBudget(int generation)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_PREEMPTIVE;
    }
    CONTRACTL_END;

    return GCHeapUtilities::GetGCHeap()->GetGenerationBudget(generation);
}

#ifdef HOST_64BIT
const unsigned MIN_MEMORYPRESSURE_BUDGET = 4 * 1024 * 1024;        // 4 MB
#else // HOST_64BIT
const unsigned MIN_MEMORYPRESSURE_BUDGET = 3 * 1024 * 1024;        // 3 MB
#endif // HOST_64BIT

const unsigned MAX_MEMORYPRESSURE_RATIO = 10;                      // 40 MB or 30 MB


// Resets pressure accounting after a gen2 GC has occurred.
void GCInterface::CheckCollectionCount()
{
    LIMITED_METHOD_CONTRACT;

    IGCHeap * pHeap = GCHeapUtilities::GetGCHeap();

    if (m_gc_counts[2] != pHeap->CollectionCount(2))
    {
        for (int i = 0; i < 3; i++)
        {
            m_gc_counts[i] = pHeap->CollectionCount(i);
        }

        m_iteration++;

        UINT p = m_iteration % MEM_PRESSURE_COUNT;

        m_addPressure[p] = 0;   // new pressure will be accumulated here
        m_remPressure[p] = 0;
    }
}

// AddMemoryPressure implementation
//
//   1. Start budget - MIN_MEMORYPRESSURE_BUDGET
//   2. Focuses more on newly added memory pressure
//   3. Budget adjusted by effectiveness of last 3 triggered GC (add / remove ratio, max 10x)
//   4. Budget maxed with 30% of current managed GC size
//   5. If Gen2 GC is happening naturally, ignore past pressure
//
// Here's a brief description of the ideal algorithm for Add/Remove memory pressure:
// Do a GC when (HeapStart < X * MemPressureGrowth) where
// - HeapStart is GC Heap size after doing the last GC
// - MemPressureGrowth is the net of Add and Remove since the last GC
// - X is proportional to our guess of the ummanaged memory death rate per GC interval,
//   and would be calculated based on historic data using standard exponential approximation:
//   Xnew = UMDeath/UMTotal * 0.5 + Xprev
//
void GCInterface::AddMemoryPressure(UINT64 bytesAllocated)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;

    CheckCollectionCount();

    UINT p = m_iteration % MEM_PRESSURE_COUNT;

    UINT64 newMemValue = InterlockedAdd(&m_addPressure[p], bytesAllocated);

    static_assert(MEM_PRESSURE_COUNT == 4, "AddMemoryPressure contains unrolled loops which depend on MEM_PRESSURE_COUNT");

    UINT64 add = m_addPressure[0] + m_addPressure[1] + m_addPressure[2] + m_addPressure[3] - m_addPressure[p];
    UINT64 rem = m_remPressure[0] + m_remPressure[1] + m_remPressure[2] + m_remPressure[3] - m_remPressure[p];

    STRESS_LOG4(LF_GCINFO, LL_INFO10000, "AMP Add: %llu => added=%llu total_added=%llu total_removed=%llu",
        bytesAllocated, newMemValue, add, rem);

    SendEtwAddMemoryPressureEvent(bytesAllocated);

    if (newMemValue >= MIN_MEMORYPRESSURE_BUDGET)
    {
        UINT64 budget = MIN_MEMORYPRESSURE_BUDGET;

        if (m_iteration >= MEM_PRESSURE_COUNT) // wait until we have enough data points
        {
            // Adjust according to effectiveness of GC
            // Scale budget according to past m_addPressure / m_remPressure ratio
            if (add >= rem * MAX_MEMORYPRESSURE_RATIO)
            {
                budget = MIN_MEMORYPRESSURE_BUDGET * MAX_MEMORYPRESSURE_RATIO;
            }
            else if (add > rem)
            {
                CONSISTENCY_CHECK(rem != 0);

                // Avoid overflow by calculating addPressure / remPressure as fixed point (1 = 1024)
                budget = (add * 1024 / rem) * budget / 1024;
            }
        }

        // If still over budget, check current managed heap size
        if (newMemValue >= budget)
        {
            IGCHeap *pGCHeap = GCHeapUtilities::GetGCHeap();
            UINT64 heapOver3 = pGCHeap->GetCurrentObjSize() / 3;

            if (budget < heapOver3) // Max
            {
                budget = heapOver3;
            }

            if (newMemValue >= budget)
            {
                // last check - if we would exceed 20% of GC "duty cycle", do not trigger GC at this time
                if ((size_t)(pGCHeap->GetNow() - pGCHeap->GetLastGCStartTime(2)) > (pGCHeap->GetLastGCDuration(2) * 5))
                {
                    STRESS_LOG6(LF_GCINFO, LL_INFO10000, "AMP Budget: pressure=%llu ? budget=%llu (total_added=%llu, total_removed=%llu, mng_heap=%llu) pos=%d",
                        newMemValue, budget, add, rem, heapOver3 * 3, m_iteration);

                    GarbageCollectModeAny(2);

                    CheckCollectionCount();
                }
            }
        }
    }
}

extern "C" void QCALLTYPE GCInterface_RemoveMemoryPressure(UINT64 bytesAllocated)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;
    GCInterface::RemoveMemoryPressure(bytesAllocated);
    END_QCALL;
}

void GCInterface::RemoveMemoryPressure(UINT64 bytesAllocated)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END;

    CheckCollectionCount();

    UINT p = m_iteration % MEM_PRESSURE_COUNT;

    SendEtwRemoveMemoryPressureEvent(bytesAllocated);

    InterlockedAdd(&m_remPressure[p], bytesAllocated);

    STRESS_LOG2(LF_GCINFO, LL_INFO10000, "AMP Remove: %llu => removed=%llu",
        bytesAllocated, m_remPressure[p]);
}

inline void GCInterface::SendEtwAddMemoryPressureEvent(UINT64 bytesAllocated)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;

    FireEtwIncreaseMemoryPressure(bytesAllocated, GetClrInstanceId());
}

// Out-of-line helper to avoid EH prolog/epilog in functions that otherwise don't throw.
NOINLINE void GCInterface::SendEtwRemoveMemoryPressureEvent(UINT64 bytesAllocated)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END;

    EX_TRY
    {
        FireEtwDecreaseMemoryPressure(bytesAllocated, GetClrInstanceId());
    }
    EX_CATCH
    {
        // Ignore failures
    }
    EX_END_CATCH
}

// Out-of-line helper to avoid EH prolog/epilog in functions that otherwise don't throw.
NOINLINE void GCInterface::GarbageCollectModeAny(int generation)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;

    GCX_COOP();
    GCHeapUtilities::GetGCHeap()->GarbageCollect(generation, false, collection_non_blocking);
}

//
// COMInterlocked
//

#include <optsmallperfcritical.h>

FCIMPL2(INT32,COMInterlocked::Exchange32, INT32 *location, INT32 value)
{
    FCALL_CONTRACT;

    return InterlockedExchange((LONG *) location, value);
}
FCIMPLEND

FCIMPL2_IV(INT64,COMInterlocked::Exchange64, INT64 *location, INT64 value)
{
    FCALL_CONTRACT;

    return InterlockedExchange64((INT64 *) location, value);
}
FCIMPLEND

FCIMPL3(INT32, COMInterlocked::CompareExchange32, INT32* location, INT32 value, INT32 comparand)
{
    FCALL_CONTRACT;

    return InterlockedCompareExchange((LONG*)location, value, comparand);
}
FCIMPLEND

FCIMPL3_IVV(INT64, COMInterlocked::CompareExchange64, INT64* location, INT64 value, INT64 comparand)
{
    FCALL_CONTRACT;

    return InterlockedCompareExchange64((INT64*)location, value, comparand);
}
FCIMPLEND

FCIMPL2(LPVOID,COMInterlocked::ExchangeObject, LPVOID*location, LPVOID value)
{
    FCALL_CONTRACT;

    LPVOID ret = InterlockedExchangeT(location, value);
#ifdef _DEBUG
    Thread::ObjectRefAssign((OBJECTREF *)location);
#endif
    ErectWriteBarrier((OBJECTREF*) location, ObjectToOBJECTREF((Object*) value));
    return ret;
}
FCIMPLEND

FCIMPL3(LPVOID,COMInterlocked::CompareExchangeObject, LPVOID *location, LPVOID value, LPVOID comparand)
{
    FCALL_CONTRACT;

    // <TODO>@todo: only set ref if is updated</TODO>
    LPVOID ret = InterlockedCompareExchangeT(location, value, comparand);
    if (ret == comparand) {
#ifdef _DEBUG
        Thread::ObjectRefAssign((OBJECTREF *)location);
#endif
        ErectWriteBarrier((OBJECTREF*) location, ObjectToOBJECTREF((Object*) value));
    }
    return ret;
}
FCIMPLEND

FCIMPL2(INT32,COMInterlocked::ExchangeAdd32, INT32 *location, INT32 value)
{
    FCALL_CONTRACT;

    return InterlockedExchangeAdd((LONG *) location, value);
}
FCIMPLEND

FCIMPL2_IV(INT64,COMInterlocked::ExchangeAdd64, INT64 *location, INT64 value)
{
    FCALL_CONTRACT;

    return InterlockedExchangeAdd64((INT64 *) location, value);
}
FCIMPLEND

#include <optdefault.h>

extern "C" void QCALLTYPE Interlocked_MemoryBarrierProcessWide()
{
    QCALL_CONTRACT;

    FlushProcessWriteBuffers();
}

static BOOL HasOverriddenMethod(MethodTable* mt, MethodTable* classMT, WORD methodSlot)
{
    CONTRACTL{
        THROWS;
        GC_NOTRIGGER;
        MODE_ANY;
    } CONTRACTL_END;

    _ASSERTE(mt != NULL);
    _ASSERTE(classMT != NULL);
    _ASSERTE(methodSlot != 0);

    PCODE actual = mt->GetRestoredSlot(methodSlot);
    PCODE base = classMT->GetRestoredSlot(methodSlot);

    if (actual == base)
    {
        return FALSE;
    }

    // If CoreLib is JITed, the slots can be patched and thus we need to compare the actual MethodDescs
    // to detect match reliably
    if (MethodTable::GetMethodDescForSlotAddress(actual) == MethodTable::GetMethodDescForSlotAddress(base))
    {
        return FALSE;
    }

    return TRUE;
}

BOOL CanCompareBitsOrUseFastGetHashCode(MethodTable* mt)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
    } CONTRACTL_END;

    _ASSERTE(mt != NULL);

    if (mt->HasCheckedCanCompareBitsOrUseFastGetHashCode())
    {
        return mt->CanCompareBitsOrUseFastGetHashCode();
    }

    if (mt->ContainsGCPointers()
        || mt->IsNotTightlyPacked()
        || mt->GetClass()->IsInlineArray())
    {
        mt->SetHasCheckedCanCompareBitsOrUseFastGetHashCode();
        return FALSE;
    }

    MethodTable* valueTypeMT = CoreLibBinder::GetClass(CLASS__VALUE_TYPE);
    WORD slotEquals = CoreLibBinder::GetMethod(METHOD__VALUE_TYPE__EQUALS)->GetSlot();
    WORD slotGetHashCode = CoreLibBinder::GetMethod(METHOD__VALUE_TYPE__GET_HASH_CODE)->GetSlot();

    // Check the input type.
    if (HasOverriddenMethod(mt, valueTypeMT, slotEquals)
        || HasOverriddenMethod(mt, valueTypeMT, slotGetHashCode))
    {
        mt->SetHasCheckedCanCompareBitsOrUseFastGetHashCode();

        // If overridden Equals or GetHashCode found, stop searching further.
        return FALSE;
    }

    BOOL canCompareBitsOrUseFastGetHashCode = TRUE;

    // The type itself did not override Equals or GetHashCode, go for its fields.
    ApproxFieldDescIterator iter = ApproxFieldDescIterator(mt, ApproxFieldDescIterator::INSTANCE_FIELDS);
    for (FieldDesc* pField = iter.Next(); pField != NULL; pField = iter.Next())
    {
        if (pField->GetFieldType() == ELEMENT_TYPE_VALUETYPE)
        {
            // Check current field type.
            MethodTable* fieldMethodTable = pField->GetApproxFieldTypeHandleThrowing().GetMethodTable();
            if (!CanCompareBitsOrUseFastGetHashCode(fieldMethodTable))
            {
                canCompareBitsOrUseFastGetHashCode = FALSE;
                break;
            }
        }
        else if (pField->GetFieldType() == ELEMENT_TYPE_R8
                || pField->GetFieldType() == ELEMENT_TYPE_R4)
        {
            // We have double/single field, cannot compare in fast path.
            canCompareBitsOrUseFastGetHashCode = FALSE;
            break;
        }
    }

    // We've gone through all instance fields. It's time to cache the result.
    // Note SetCanCompareBitsOrUseFastGetHashCode(BOOL) ensures the checked flag
    // and canCompare flag being set atomically to avoid race.
    mt->SetCanCompareBitsOrUseFastGetHashCode(canCompareBitsOrUseFastGetHashCode);

    return canCompareBitsOrUseFastGetHashCode;
}

extern "C" BOOL QCALLTYPE MethodTable_CanCompareBitsOrUseFastGetHashCode(MethodTable* mt)
{
    QCALL_CONTRACT;

    BOOL ret = FALSE;

    BEGIN_QCALL;

    if (mt->GetClass()->IsInlineArray())
        COMPlusThrow(kNotSupportedException, W("NotSupported_InlineArrayEqualsGetHashCode"));

    ret = CanCompareBitsOrUseFastGetHashCode(mt);

    END_QCALL;

    return ret;
}

enum ValueTypeHashCodeStrategy
{
    None,
    ReferenceField,
    DoubleField,
    SingleField,
    FastGetHashCode,
    ValueTypeOverride,
};

static ValueTypeHashCodeStrategy GetHashCodeStrategy(MethodTable* mt, QCall::ObjectHandleOnStack objHandle, UINT32* fieldOffset, UINT32* fieldSize, MethodTable** fieldMTOut)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_PREEMPTIVE;
    } CONTRACTL_END;

    // Should be handled by caller
    _ASSERTE(!mt->CanCompareBitsOrUseFastGetHashCode());

    ValueTypeHashCodeStrategy ret = ValueTypeHashCodeStrategy::None;

    // Grab the first non-null field and return its hash code or 'it' as hash code
    ApproxFieldDescIterator fdIterator(mt, ApproxFieldDescIterator::INSTANCE_FIELDS);

    FieldDesc *field;
    while ((field = fdIterator.Next()) != NULL)
    {
        _ASSERTE(!field->IsRVA());
        if (field->IsObjRef())
        {
            GCX_COOP();
            // if we get an object reference we get the hash code out of that
            if (*(Object**)((BYTE *)objHandle.Get()->UnBox() + *fieldOffset + field->GetOffsetUnsafe()) != NULL)
            {
                *fieldOffset += field->GetOffsetUnsafe();
                ret = ValueTypeHashCodeStrategy::ReferenceField;
            }
            else
            {
                // null object reference, try next
                continue;
            }
        }
        else
        {
            CorElementType fieldType = field->GetFieldType();
            if (fieldType == ELEMENT_TYPE_R8)
            {
                *fieldOffset += field->GetOffsetUnsafe();
                ret = ValueTypeHashCodeStrategy::DoubleField;
            }
            else if (fieldType == ELEMENT_TYPE_R4)
            {
                *fieldOffset += field->GetOffsetUnsafe();
                ret = ValueTypeHashCodeStrategy::SingleField;
            }
            else if (fieldType != ELEMENT_TYPE_VALUETYPE)
            {
                *fieldOffset += field->GetOffsetUnsafe();
                *fieldSize = field->LoadSize();
                ret = ValueTypeHashCodeStrategy::FastGetHashCode;
            }
            else
            {
                // got another value type. Get the type
                TypeHandle fieldTH = field->GetFieldTypeHandleThrowing();
                _ASSERTE(!fieldTH.IsNull());
                MethodTable* fieldMT = fieldTH.GetMethodTable();
                if (CanCompareBitsOrUseFastGetHashCode(fieldMT))
                {
                    *fieldOffset += field->GetOffsetUnsafe();
                    *fieldSize = field->LoadSize();
                    ret = ValueTypeHashCodeStrategy::FastGetHashCode;
                }
                else if (HasOverriddenMethod(fieldMT,
                                             CoreLibBinder::GetClass(CLASS__VALUE_TYPE),
                                             CoreLibBinder::GetMethod(METHOD__VALUE_TYPE__GET_HASH_CODE)->GetSlot()))
                {
                    *fieldOffset += field->GetOffsetUnsafe();
                    *fieldMTOut = fieldMT;
                    ret = ValueTypeHashCodeStrategy::ValueTypeOverride;
                }
                else
                {
                    *fieldOffset += field->GetOffsetUnsafe();
                    ret = GetHashCodeStrategy(fieldMT, objHandle, fieldOffset, fieldSize, fieldMTOut);
                }
            }
        }
        break;
    }

    return ret;
}

extern "C" INT32 QCALLTYPE ValueType_GetHashCodeStrategy(MethodTable* mt, QCall::ObjectHandleOnStack objHandle, UINT32* fieldOffset, UINT32* fieldSize, MethodTable** fieldMT)
{
    QCALL_CONTRACT;

    ValueTypeHashCodeStrategy ret = ValueTypeHashCodeStrategy::None;
    *fieldOffset = 0;
    *fieldSize = 0;
    *fieldMT = NULL;

    BEGIN_QCALL;

    ret = GetHashCodeStrategy(mt, objHandle, fieldOffset, fieldSize, fieldMT);

    END_QCALL;

    return ret;
}

FCIMPL1(UINT32, MethodTableNative::GetNumInstanceFieldBytes, MethodTable* mt)
{
    FCALL_CONTRACT;
    return mt->GetNumInstanceFieldBytes();
}
FCIMPLEND

FCIMPL1(CorElementType, MethodTableNative::GetPrimitiveCorElementType, MethodTable* mt)
{
    FCALL_CONTRACT;

    _ASSERTE(mt->IsTruePrimitive() || mt->IsEnum());

    // MethodTable::GetInternalCorElementType has unnecessary overhead for primitives and enums
    // Call EEClass::GetInternalCorElementType directly to avoid it
    return mt->GetClass()->GetInternalCorElementType();
}
FCIMPLEND

FCIMPL2(MethodTable*, MethodTableNative::GetMethodTableMatchingParentClass, MethodTable *mt, MethodTable* parent)
{
    FCALL_CONTRACT;

    return mt->GetMethodTableMatchingParentClass(parent);
}
FCIMPLEND

FCIMPL1(MethodTable*, MethodTableNative::InstantiationArg0, MethodTable* mt);
{
    FCALL_CONTRACT;

    return mt->GetInstantiation()[0].AsMethodTable();
}
FCIMPLEND

FCIMPL1(OBJECTHANDLE, MethodTableNative::GetLoaderAllocatorHandle, MethodTable *mt)
{
    FCALL_CONTRACT;

    return mt->GetLoaderAllocatorObjectHandle();
}
FCIMPLEND

extern "C" BOOL QCALLTYPE MethodTable_AreTypesEquivalent(MethodTable* mta, MethodTable* mtb)
{
    QCALL_CONTRACT;

    BOOL bResult = FALSE;

    BEGIN_QCALL;

    bResult = mta->IsEquivalentTo(mtb);

    END_QCALL;

    return bResult;
}

extern "C" BOOL QCALLTYPE TypeHandle_CanCastTo_NoCacheLookup(void* fromTypeHnd, void* toTypeHnd)
{
    QCALL_CONTRACT;

    BOOL ret = false;

    BEGIN_QCALL;

    // Cache lookup and trivial cases are already handled at managed side. Call the uncached versions directly.
    _ASSERTE(fromTypeHnd != toTypeHnd);

    GCX_COOP();

    TypeHandle fromTH = TypeHandle::FromPtr(fromTypeHnd);
    TypeHandle toTH = TypeHandle::FromPtr(toTypeHnd);

    if (fromTH.IsTypeDesc())
    {
        ret = fromTH.AsTypeDesc()->CanCastTo(toTH, NULL);
    }
    else
    {
        ret = fromTH.AsMethodTable()->CanCastTo(toTH.AsMethodTable(), NULL);
    }

    END_QCALL;

    return ret;
}

extern "C" INT32 QCALLTYPE TypeHandle_GetCorElementType(void* typeHnd)
{
    QCALL_CONTRACT_NO_GC_TRANSITION;

    return (INT32)TypeHandle::FromPtr(typeHnd).GetSignatureCorElementType();
}

static bool HasOverriddenStreamMethod(MethodTable* streamMT, MethodTable* pMT, WORD slot)
{
    CONTRACTL
    {
        STANDARD_VM_CHECK;
        PRECONDITION(streamMT != NULL);
        PRECONDITION(pMT != NULL);
    } CONTRACTL_END;

    PCODE actual = pMT->GetRestoredSlot(slot);
    PCODE base = streamMT->GetRestoredSlot(slot);

    // If the PCODEs match, then there is no override.
    if (actual == base)
        return false;

    // If CoreLib is JITed, the slots can be patched and thus we need to compare
    // the actual MethodDescs to detect match reliably.
    return MethodTable::GetMethodDescForSlotAddress(actual) != MethodTable::GetMethodDescForSlotAddress(base);
}

extern "C" BOOL QCALLTYPE Stream_HasOverriddenSlow(MethodTable* pMT, BOOL isRead)
{
    QCALL_CONTRACT;
    _ASSERTE(pMT != NULL);

    BOOL readOverride = FALSE;
    BOOL writeOverride = FALSE;

    BEGIN_QCALL;

    MethodTable* pStreamMT = CoreLibBinder::GetClass(CLASS__STREAM);
    WORD slotBeginRead = CoreLibBinder::GetMethod(METHOD__STREAM__BEGIN_READ)->GetSlot();
    WORD slotEndRead = CoreLibBinder::GetMethod(METHOD__STREAM__END_READ)->GetSlot();
    WORD slotBeginWrite = CoreLibBinder::GetMethod(METHOD__STREAM__BEGIN_WRITE)->GetSlot();
    WORD slotEndWrite = CoreLibBinder::GetMethod(METHOD__STREAM__END_WRITE)->GetSlot();

    // Check the current MethodTable for Stream overrides and set state on the MethodTable.
    readOverride = HasOverriddenStreamMethod(pStreamMT, pMT, slotBeginRead) || HasOverriddenStreamMethod(pStreamMT, pMT, slotEndRead);
    writeOverride = HasOverriddenStreamMethod(pStreamMT, pMT, slotBeginWrite) || HasOverriddenStreamMethod(pStreamMT, pMT, slotEndWrite);
    pMT->GetAuxiliaryDataForWrite()->SetStreamOverrideState(readOverride, writeOverride);

    END_QCALL;

    return isRead ? readOverride : writeOverride;
}
