// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.


#include "common.h"
#include "corhdr.h"
#include "runtimehandles.h"
#include "object.h"
#include "class.h"
#include "method.hpp"
#include "typehandle.h"
#include "field.h"
#include "siginfo.hpp"
#include "clsload.hpp"
#include "typestring.h"
#include "typeparse.h"
#include "holder.h"
#include "codeman.h"
#include "corhlpr.h"
#include "jitinterface.h"
#include "eeconfig.h"
#include "eehash.h"
#include "interoputil.h"
#include "comdelegate.h"
#include "typedesc.h"
#include "virtualcallstub.h"
#include "contractimpl.h"
#include "dynamicmethod.h"
#include "peimagelayout.inl"
#include "eventtrace.h"
#include "invokeutil.h"
#include "castcache.h"
#include "encee.h"

extern "C" BOOL QCALLTYPE MdUtf8String_EqualsCaseInsensitive(LPCUTF8 szLhs, LPCUTF8 szRhs, INT32 stringNumBytes)
{
    QCALL_CONTRACT;

    // Important: the string in pSsz isn't null terminated so the length must be used
    // when performing operations on the string.

    BOOL fStringsEqual = FALSE;

    BEGIN_QCALL;

    _ASSERTE(CheckPointer(szLhs));
    _ASSERTE(CheckPointer(szRhs));

    // At this point, both the left and right strings are guaranteed to have the
    // same length.
    StackSString lhs(SString::Utf8, szLhs, stringNumBytes);
    StackSString rhs(SString::Utf8, szRhs, stringNumBytes);

    // We can use SString for simple case insensitive compares
    fStringsEqual = lhs.EqualsCaseInsensitive(rhs);

    END_QCALL;

    return fStringsEqual;
}

static BOOL CheckCAVisibilityFromDecoratedType(MethodTable* pCAMT, MethodDesc* pCACtor, MethodTable* pDecoratedMT, Module* pDecoratedModule)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        PRECONDITION(CheckPointer(pCAMT));
        PRECONDITION(CheckPointer(pCACtor, NULL_OK));
        PRECONDITION(CheckPointer(pDecoratedMT, NULL_OK));
        PRECONDITION(CheckPointer(pDecoratedModule));
    }
    CONTRACTL_END;

    DWORD dwAttr = mdPublic;

    if (pCACtor != NULL)
    {
        _ASSERTE(pCACtor->IsCtor());

        dwAttr = pCACtor->GetAttrs();
    }

    AccessCheckContext accessContext(NULL, pDecoratedMT, pDecoratedModule->GetAssembly());

    return ClassLoader::CanAccess(
        &accessContext,
        pCAMT,
        pCAMT->GetAssembly(),
        dwAttr,
        pCACtor,
        NULL,
        *AccessCheckOptions::s_pNormalAccessChecks);
}

extern "C" BOOL QCALLTYPE RuntimeMethodHandle_IsCAVisibleFromDecoratedType(
    QCall::TypeHandle       targetTypeHandle,
    MethodDesc *            pTargetCtor,
    QCall::TypeHandle       sourceTypeHandle,
    QCall::ModuleHandle     sourceModuleHandle)
{
    QCALL_CONTRACT;

    BOOL bResult = TRUE;

    BEGIN_QCALL;
    TypeHandle sourceHandle = sourceTypeHandle.AsTypeHandle();
    TypeHandle targetHandle = targetTypeHandle.AsTypeHandle();

    _ASSERTE((sourceHandle.IsNull() || !sourceHandle.IsTypeDesc()) &&
             !targetHandle.IsNull() &&
             !targetHandle.IsTypeDesc());

    if (sourceHandle.IsTypeDesc() ||
        targetHandle.IsNull() ||
        targetHandle.IsTypeDesc())
        COMPlusThrowArgumentNull(NULL, W("Arg_InvalidHandle"));

    if (pTargetCtor == NULL)
    {
        MethodTable* pTargetMT = targetHandle.AsMethodTable();

        if (pTargetMT->HasDefaultConstructor())
        {
            pTargetCtor = pTargetMT->GetDefaultConstructor();
        }
        else
        {
            if (!pTargetMT->IsValueType())
                COMPlusThrowNonLocalized(kMissingMethodException, COR_CTOR_METHOD_NAME_W);
        }
    }

    bResult = CheckCAVisibilityFromDecoratedType(targetHandle.AsMethodTable(), pTargetCtor, sourceHandle.AsMethodTable(), sourceModuleHandle);
    END_QCALL;

    return bResult;
}

extern "C" void QCALLTYPE RuntimeTypeHandle_GetRuntimeTypeFromHandleSlow(
    EnregisteredTypeHandle typeHandleRaw,
    QCall::ObjectHandleOnStack result)
{
    QCALL_CONTRACT;

    _ASSERTE(typeHandleRaw != NULL);

    BEGIN_QCALL;

    GCX_COOP();

    TypeHandle typeHandle = TypeHandle::FromPtr(typeHandleRaw);
    result.Set(typeHandle.GetManagedClassObject());
    _ASSERTE(result.Get() != NULL);

    END_QCALL;
}

FCIMPL1(ReflectClassBaseObject*, RuntimeTypeHandle::GetRuntimeTypeFromHandleIfExists, EnregisteredTypeHandle th)
{
    FCALL_CONTRACT;

    _ASSERTE(th != NULL);

    TypeHandle typeHandle = TypeHandle::FromPtr(th);
    return (ReflectClassBaseObject*)OBJECTREFToObject(typeHandle.GetManagedClassObjectIfExists());
}
FCIMPLEND

FCIMPL2(FC_BOOL_RET, RuntimeTypeHandle::IsEquivalentTo, ReflectClassBaseObject *rtType1UNSAFE, ReflectClassBaseObject *rtType2UNSAFE)
{
    FCALL_CONTRACT;

    REFLECTCLASSBASEREF rtType1 = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(rtType1UNSAFE);
    REFLECTCLASSBASEREF rtType2 = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(rtType2UNSAFE);

    BOOL areEquivalent = FALSE;
    HELPER_METHOD_FRAME_BEGIN_RET_2(rtType1, rtType2);

    if (rtType1 != NULL && rtType2 != NULL)
        areEquivalent = rtType1->GetType().IsEquivalentTo(rtType2->GetType());

    HELPER_METHOD_FRAME_END();

    FC_RETURN_BOOL(areEquivalent);
}
FCIMPLEND

FCIMPL1(MethodDesc *, RuntimeTypeHandle::GetFirstIntroducedMethod, ReflectClassBaseObject *pTypeUNSAFE) {
    CONTRACTL {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pTypeUNSAFE));
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);
    TypeHandle typeHandle = refType->GetType();

    if (typeHandle.IsGenericVariable())
        FCThrowRes(kArgumentException, W("Arg_InvalidHandle"));

    if (typeHandle.IsTypeDesc()) {
        return NULL;
    }

    MethodTable* pMT = typeHandle.AsMethodTable();
    if (pMT == NULL)
        return NULL;

    MethodDesc* pMethod = MethodTable::IntroducedMethodIterator::GetFirst(pMT);
    return pMethod;
}
FCIMPLEND

#include <optsmallperfcritical.h>
FCIMPL1(void, RuntimeTypeHandle::GetNextIntroducedMethod, MethodDesc ** ppMethod) {
    CONTRACTL {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(ppMethod));
        PRECONDITION(CheckPointer(*ppMethod));
    }
    CONTRACTL_END;

    MethodDesc *pMethod = MethodTable::IntroducedMethodIterator::GetNext(*ppMethod);

    *ppMethod = pMethod;
}
FCIMPLEND
#include <optdefault.h>

FCIMPL1(AssemblyBaseObject*, RuntimeTypeHandle::GetAssemblyIfExists, ReflectClassBaseObject *pTypeUNSAFE)
{
    FCALL_CONTRACT;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);
    if (refType == NULL)
        return NULL;

    Assembly* pAssembly = refType->GetType().GetAssembly();
    OBJECTREF refAssembly = pAssembly->GetExposedObjectIfExists();
    return (AssemblyBaseObject*)OBJECTREFToObject(refAssembly);
}
FCIMPLEND

extern "C" void QCALLTYPE RuntimeTypeHandle_GetAssemblySlow(QCall::ObjectHandleOnStack type, QCall::ObjectHandleOnStack assembly)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;
    GCX_COOP();

    if (type.Get() == NULL)
        COMPlusThrow(kArgumentNullException, W("Arg_InvalidHandle"));

    Assembly* pAssembly = ((REFLECTCLASSBASEREF)type.Get())->GetType().GetAssembly();
    assembly.Set(pAssembly->GetExposedObject());
    END_QCALL;
}

FCIMPL1(FC_BOOL_RET, RuntimeFieldHandle::AcquiresContextFromThis, FieldDesc* pField)
{
    CONTRACTL {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pField));
    }
    CONTRACTL_END;

    FC_RETURN_BOOL(pField->IsSharedByGenericInstantiations());

}
FCIMPLEND

FCIMPL1(Object*, RuntimeFieldHandle::GetLoaderAllocator, FieldDesc* pField)
{
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    OBJECTREF loaderAllocator = NULL;

    if (!pField)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    HELPER_METHOD_FRAME_BEGIN_RET_PROTECT(loaderAllocator);

    LoaderAllocator *pLoaderAllocator = pField->GetApproxEnclosingMethodTable()->GetLoaderAllocator();
    loaderAllocator = pLoaderAllocator->GetExposedObject();

    HELPER_METHOD_FRAME_END();

    return OBJECTREFToObject(loaderAllocator);
}
FCIMPLEND

FCIMPL1(ReflectModuleBaseObject*, RuntimeTypeHandle::GetModuleIfExists, ReflectClassBaseObject *pTypeUNSAFE)
{
    FCALL_CONTRACT;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);
    if (refType == NULL)
        return NULL;

    Module* pModule = refType->GetType().GetModule();
    OBJECTREF refModule = pModule->GetExposedObjectIfExists();
    return (ReflectModuleBaseObject*)OBJECTREFToObject(refModule);
}
FCIMPLEND

extern "C" void QCALLTYPE RuntimeTypeHandle_GetModuleSlow(QCall::ObjectHandleOnStack type, QCall::ObjectHandleOnStack module)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;
    GCX_COOP();

    Module* pModule = ((REFLECTCLASSBASEREF)type.Get())->GetType().GetModule();
    module.Set(pModule->GetExposedObject());
    END_QCALL;
}

FCIMPL1(EnregisteredTypeHandle, RuntimeTypeHandle::GetElementTypeHandle, EnregisteredTypeHandle th)
{
    FCALL_CONTRACT;

    _ASSERTE(th != NULL);

    TypeHandle typeHandle = TypeHandle::FromPtr(th);
    TypeHandle typeReturn;

    if (!typeHandle.IsTypeDesc())
    {
        if (!typeHandle.AsMethodTable()->IsArray())
            return NULL;

        typeReturn = typeHandle.GetArrayElementTypeHandle();
    }
    else
    {
        if (typeHandle.IsGenericVariable())
            return NULL;

        typeReturn = typeHandle.AsTypeDesc()->GetTypeParam();
    }

    return (EnregisteredTypeHandle)typeReturn.AsTAddr();
}
FCIMPLEND

FCIMPL1(INT32, RuntimeTypeHandle::GetArrayRank, ReflectClassBaseObject *pTypeUNSAFE) {
    CONTRACTL {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pTypeUNSAFE));
        PRECONDITION(pTypeUNSAFE->GetType().IsArray());
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);

    return (INT32)refType->GetType().GetRank();
}
FCIMPLEND

FCIMPL1(INT32, RuntimeTypeHandle::GetNumVirtuals, ReflectClassBaseObject* pTypeUNSAFE) {
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);
    _ASSERTE(refType != NULL);

    TypeHandle typeHandle = refType->GetType();
    _ASSERTE(!typeHandle.IsGenericVariable());

    MethodTable *pMT = typeHandle.GetMethodTable();
    if (pMT == NULL)
        return 0;
    return (INT32)pMT->GetNumVirtuals();
}
FCIMPLEND

extern "C" INT32 QCALLTYPE RuntimeTypeHandle_GetNumVirtualsAndStaticVirtuals(QCall::TypeHandle pTypeHandle)
{
    QCALL_CONTRACT;

    INT32 numVirtuals = 0;

    BEGIN_QCALL;

    TypeHandle typeHandle = pTypeHandle.AsTypeHandle();
    _ASSERTE(!typeHandle.IsGenericVariable());

    MethodTable *pMT = typeHandle.GetMethodTable();
    if (pMT != NULL)
    {
        numVirtuals = (INT32)pMT->GetNumVirtuals();
        if (pMT->HasVirtualStaticMethods())
        {
            for (MethodTable::MethodIterator it(pMT); it.IsValid(); it.Next())
            {
                MethodDesc *pMD = it.GetMethodDesc();
                if (pMD->IsVirtual()
                    && pMD->IsStatic())
                {
                    numVirtuals++;
                }
            }
        }
    }

    END_QCALL;

    return numVirtuals;
}

FCIMPL2(MethodDesc *, RuntimeTypeHandle::GetMethodAt, ReflectClassBaseObject *pTypeUNSAFE, INT32 slot) {
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);

    if (refType == NULL)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    TypeHandle typeHandle = refType->GetType();

    MethodDesc* pRetMethod = NULL;

    if (typeHandle.IsGenericVariable())
        FCThrowRes(kArgumentException, W("Arg_InvalidHandle"));

    HELPER_METHOD_FRAME_BEGIN_RET_1(refType);

    MethodTable *pMT = typeHandle.GetMethodTable();
    INT32 numVirtuals = (INT32)pMT->GetNumVirtuals();

    if (slot < 0)
        COMPlusThrow(kArgumentException, W("Arg_ArgumentOutOfRangeException"));
    else if (slot < numVirtuals)
    {
        pRetMethod = pMT->GetMethodDescForSlot((DWORD)slot);
    }
    else
    {
        // Search for virtual static via linear search
        INT32 curVirtualIndex = numVirtuals;
        if (pMT->HasVirtualStaticMethods() && pMT->IsInterface())
        {
            for (MethodTable::MethodIterator it(pMT); it.IsValid(); it.Next())
            {
                MethodDesc *pMD = it.GetMethodDesc();
                if (pMD->IsVirtual() &&
                    pMD->IsStatic())
                {
                    if (slot == curVirtualIndex)
                    {
                        pRetMethod = pMD;
                        break;
                    }
                    curVirtualIndex++;
                }
            }
        }

        // If search continues past end of virtual static list, fail with exception
        if (pRetMethod == NULL)
        {
            COMPlusThrow(kArgumentException, W("Arg_ArgumentOutOfRangeException"));
        }
    }

    HELPER_METHOD_FRAME_END();

    return pRetMethod;
}

FCIMPLEND

FCIMPL3(FC_BOOL_RET, RuntimeTypeHandle::GetFields, ReflectClassBaseObject *pTypeUNSAFE, INT32 **result, INT32 *pCount) {
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);
    if (refType == NULL)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    TypeHandle typeHandle = refType->GetType();

    if (!pCount || !result)
        FCThrow(kArgumentNullException);

    if (typeHandle.IsGenericVariable())
        FCThrowRes(kArgumentException, W("Arg_InvalidHandle"));

    if (typeHandle.IsTypeDesc() || typeHandle.IsArray()) {
        *pCount = 0;
        FC_RETURN_BOOL(TRUE);
    }

    MethodTable *pMT= typeHandle.GetMethodTable();
    if (!pMT)
        FCThrowRes(kArgumentException, W("Arg_InvalidHandle"));

    BOOL retVal = FALSE;
    HELPER_METHOD_FRAME_BEGIN_RET_1(refType);
    // <TODO>Check this approximation - we may be losing exact type information </TODO>
    EncApproxFieldDescIterator fdIterator(pMT, ApproxFieldDescIterator::ALL_FIELDS, TRUE);
    INT32 count = (INT32)fdIterator.Count();

    if (count > *pCount)
    {
        *pCount = count;
    }
    else
    {
        for(INT32 i = 0; i < count; i ++)
            result[i] = (INT32*)fdIterator.Next();

        *pCount = count;
        retVal = TRUE;
    }
    HELPER_METHOD_FRAME_END();
    FC_RETURN_BOOL(retVal);
}
FCIMPLEND

extern "C" void QCALLTYPE RuntimeMethodHandle_ConstructInstantiation(MethodDesc * pMethod, DWORD format, QCall::StringHandleOnStack retString)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    StackSString ss;
    TypeString::AppendInst(ss, pMethod->LoadMethodInstantiation(), format);
    retString.Set(ss);

    END_QCALL;
}

extern "C" void QCALLTYPE RuntimeTypeHandle_ConstructName(QCall::TypeHandle pTypeHandle, DWORD format, QCall::StringHandleOnStack retString)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    StackSString ss;
    TypeString::AppendType(ss, pTypeHandle.AsTypeHandle(), format);
    retString.Set(ss);

    END_QCALL;
}

PTRARRAYREF CopyRuntimeTypeHandles(TypeHandle * prgTH, INT32 numTypeHandles, BinderClassID arrayElemType)
{
    CONTRACTL {
        THROWS;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
    }
    CONTRACTL_END;

    PTRARRAYREF refReturn = NULL;
    PTRARRAYREF refArray  = NULL;

    if (numTypeHandles == 0)
        return NULL;

    _ASSERTE(prgTH != NULL);

    GCPROTECT_BEGIN(refArray);
    TypeHandle thRuntimeType = TypeHandle(CoreLibBinder::GetClass(arrayElemType));
    TypeHandle arrayHandle = ClassLoader::LoadArrayTypeThrowing(thRuntimeType, ELEMENT_TYPE_SZARRAY);
    refArray = (PTRARRAYREF)AllocateSzArray(arrayHandle, numTypeHandles);

    for (INT32 i = 0; i < numTypeHandles; i++)
    {
        TypeHandle th;

        th = prgTH[i];

        OBJECTREF refType = th.GetManagedClassObject();
        refArray->SetAt(i, refType);
    }

    refReturn = refArray;
    GCPROTECT_END();

    return refReturn;
}

extern "C" void QCALLTYPE RuntimeTypeHandle_GetConstraints(QCall::TypeHandle pTypeHandle, QCall::ObjectHandleOnStack retTypeArray)
{
    QCALL_CONTRACT;

    TypeHandle* constraints = NULL;

    BEGIN_QCALL;

    TypeHandle typeHandle = pTypeHandle.AsTypeHandle();

    if (!typeHandle.IsGenericVariable())
        COMPlusThrow(kArgumentException, W("Arg_InvalidHandle"));

        TypeVarTypeDesc* pGenericVariable = typeHandle.AsGenericVariable();

    DWORD dwCount;
    constraints = pGenericVariable->GetConstraints(&dwCount);

    GCX_COOP();
    retTypeArray.Set(CopyRuntimeTypeHandles(constraints, dwCount, CLASS__TYPE));

    END_QCALL;

    return;
}

FCIMPL1(PtrArray*, RuntimeTypeHandle::GetInterfaces, ReflectClassBaseObject *pTypeUNSAFE) {
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);

    if (refType == NULL)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    TypeHandle typeHandle = refType->GetType();

  if (typeHandle.IsGenericVariable())
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    INT32 ifaceCount = 0;

    PTRARRAYREF refRetVal  = NULL;
    HELPER_METHOD_FRAME_BEGIN_RET_2(refRetVal, refType);
    {
        if (typeHandle.IsTypeDesc())
        {
            ifaceCount = 0;
        }
        else
        {
            ifaceCount = typeHandle.GetMethodTable()->GetNumInterfaces();
        }

        // Allocate the array
        if (ifaceCount > 0)
        {
            TypeHandle arrayHandle = ClassLoader::LoadArrayTypeThrowing(TypeHandle(g_pRuntimeTypeClass), ELEMENT_TYPE_SZARRAY);
            refRetVal = (PTRARRAYREF)AllocateSzArray(arrayHandle, ifaceCount);

            // populate type array
            UINT i = 0;

            MethodTable::InterfaceMapIterator it = typeHandle.GetMethodTable()->IterateInterfaceMap();
            while (it.Next())
            {
                OBJECTREF refInterface = it.GetInterface(typeHandle.GetMethodTable())->GetManagedClassObject();
                refRetVal->SetAt(i, refInterface);
                _ASSERTE(refRetVal->GetAt(i) != NULL);
                i++;
            }
        }
    }
    HELPER_METHOD_FRAME_END();

    return (PtrArray*)OBJECTREFToObject(refRetVal);
}
FCIMPLEND

FCIMPL1(INT32, RuntimeTypeHandle::GetAttributes, ReflectClassBaseObject *pTypeUNSAFE) {
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);

    if (refType == NULL)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    TypeHandle typeHandle = refType->GetType();

    if (typeHandle.IsTypeDesc()) {
        return tdPublic;
    }

#ifdef FEATURE_COMINTEROP
    // __ComObject types are always public.
    if (IsComObjectClass(typeHandle))
        return (typeHandle.GetMethodTable()->GetAttrClass() & tdVisibilityMask) | tdPublic;
#endif // FEATURE_COMINTEROP

    INT32 ret = 0;

    ret = (INT32)typeHandle.GetMethodTable()->GetAttrClass();
    return ret;
}
FCIMPLEND

FCIMPL1(Object *, RuntimeTypeHandle::GetArgumentTypesFromFunctionPointer, ReflectClassBaseObject *pTypeUNSAFE)
{
    CONTRACTL {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pTypeUNSAFE));
    }
    CONTRACTL_END;

    struct
    {
        PTRARRAYREF retVal;
    } gc;

    gc.retVal = NULL;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);
    TypeHandle typeHandle = refType->GetType();
    if (!typeHandle.IsFnPtrType())
        FCThrowRes(kArgumentException, W("Arg_InvalidHandle"));

    FnPtrTypeDesc* fnPtr = typeHandle.AsFnPtrType();

    HELPER_METHOD_FRAME_BEGIN_RET_PROTECT(gc);
    {
        MethodTable *pMT = CoreLibBinder::GetClass(CLASS__TYPE);
        TypeHandle arrayHandle = ClassLoader::LoadArrayTypeThrowing(TypeHandle(pMT), ELEMENT_TYPE_SZARRAY);
        DWORD cArgs = fnPtr->GetNumArgs();
        gc.retVal = (PTRARRAYREF) AllocateSzArray(arrayHandle, cArgs + 1);

        for (DWORD position = 0; position <= cArgs; position++)
        {
            TypeHandle typeHandle = fnPtr->GetRetAndArgTypes()[position];
            OBJECTREF refType = typeHandle.GetManagedClassObject();
            gc.retVal->SetAt(position, refType);
        }
    }
    HELPER_METHOD_FRAME_END();

    return OBJECTREFToObject(gc.retVal);
}
FCIMPLEND

FCIMPL1(FC_BOOL_RET, RuntimeTypeHandle::IsUnmanagedFunctionPointer, ReflectClassBaseObject *pTypeUNSAFE);
{
    CONTRACTL {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pTypeUNSAFE));
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);
    BOOL unmanaged = FALSE;
    TypeHandle typeHandle = refType->GetType();
    if (typeHandle.IsFnPtrType())
    {
        FnPtrTypeDesc* fnPtr = typeHandle.AsFnPtrType();
        unmanaged = (fnPtr->GetCallConv() & IMAGE_CEE_CS_CALLCONV_MASK) == IMAGE_CEE_CS_CALLCONV_UNMANAGED;
    }

    FC_RETURN_BOOL(unmanaged);
}
FCIMPLEND

extern "C" BOOL QCALLTYPE RuntimeTypeHandle_IsVisible(QCall::TypeHandle pTypeHandle)
{
    CONTRACTL
    {
        QCALL_CHECK;
    }
    CONTRACTL_END;

    BOOL fIsExternallyVisible = FALSE;

    BEGIN_QCALL;

    TypeHandle typeHandle = pTypeHandle.AsTypeHandle();

    _ASSERTE(!typeHandle.IsNull());

    fIsExternallyVisible = typeHandle.IsExternallyVisible();

    END_QCALL;

    return fIsExternallyVisible;
}

FCIMPL1(LPCUTF8, RuntimeTypeHandle::GetUtf8Name, MethodTable* pMT)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(pMT != NULL);
    }
    CONTRACTL_END;

    INT32 tkTypeDef = (INT32)pMT->GetCl();
    _ASSERTE(!IsNilToken(tkTypeDef));

    LPCUTF8 name;
    if (FAILED(pMT->GetMDImport()->GetNameOfTypeDef(tkTypeDef, &name, NULL)))
        name = NULL;

    return name;
}
FCIMPLEND

FCIMPL1(INT32, RuntimeTypeHandle::GetToken, ReflectClassBaseObject *pTypeUNSAFE) {
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);

    if (refType == NULL)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    TypeHandle typeHandle = refType->GetType();

    if (typeHandle.IsTypeDesc() || typeHandle.IsArray())
    {
        if (typeHandle.IsGenericVariable())
        {
            INT32 tkTypeDef = typeHandle.AsGenericVariable()->GetToken();

            _ASSERTE(!IsNilToken(tkTypeDef) && TypeFromToken(tkTypeDef) == mdtGenericParam);

            return tkTypeDef;
        }

        return mdTypeDefNil;
    }

    return  (INT32)typeHandle.AsMethodTable()->GetCl();
}
FCIMPLEND

extern "C" PVOID QCALLTYPE QCall_GetGCHandleForTypeHandle(QCall::TypeHandle pTypeHandle, INT32 handleType)
{
    QCALL_CONTRACT;

    OBJECTHANDLE objHandle = NULL;

    BEGIN_QCALL;

    GCX_COOP();

    TypeHandle th = pTypeHandle.AsTypeHandle();
    assert(handleType >= HNDTYPE_WEAK_SHORT && handleType <= HNDTYPE_DEPENDENT);
    objHandle = AppDomain::GetCurrentDomain()->CreateTypedHandle(NULL, static_cast<HandleType>(handleType));
    th.GetLoaderAllocator()->RegisterHandleForCleanup(objHandle);

    END_QCALL;

    return objHandle;
}

extern "C" void QCALLTYPE QCall_FreeGCHandleForTypeHandle(QCall::TypeHandle pTypeHandle, OBJECTHANDLE objHandle)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    GCX_COOP();

    TypeHandle th = pTypeHandle.AsTypeHandle();
    th.GetLoaderAllocator()->UnregisterHandleFromCleanup(objHandle);
    DestroyTypedHandle(objHandle);

    END_QCALL;
}

extern "C" void QCALLTYPE RuntimeTypeHandle_VerifyInterfaceIsImplemented(QCall::TypeHandle pTypeHandle, QCall::TypeHandle pIFaceHandle)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    TypeHandle typeHandle = pTypeHandle.AsTypeHandle();
    TypeHandle ifaceHandle = pIFaceHandle.AsTypeHandle();

    if (typeHandle.IsGenericVariable())
        COMPlusThrow(kArgumentException, W("Arg_InvalidHandle"));

    if (typeHandle.IsTypeDesc()) {
        COMPlusThrow(kArgumentException, W("Arg_NotFoundIFace"));
    }

    if (typeHandle.IsInterface())
        COMPlusThrow(kArgumentException, W("Argument_InterfaceMap"));

    if (!ifaceHandle.IsInterface())
        COMPlusThrow(kArgumentException, W("Arg_MustBeInterface"));

    // First try the cheap check, which amounts to iterating the interface map looking for
    // the ifaceHandle MethodTable.
    if (!typeHandle.GetMethodTable()->ImplementsInterface(ifaceHandle.AsMethodTable()))
    {   // If the cheap check fails, try the more expensive but complete check.
        if (!typeHandle.CanCastTo(ifaceHandle))
        {   // If the complete check fails, we're certain that this type
            // does not implement the interface specified.
        COMPlusThrow(kArgumentException, W("Arg_NotFoundIFace"));
        }
    }

    END_QCALL;
}

extern "C" MethodDesc* QCALLTYPE RuntimeTypeHandle_GetInterfaceMethodImplementation(QCall::TypeHandle pTypeHandle, QCall::TypeHandle pOwner, MethodDesc * pMD)
{
    QCALL_CONTRACT;

    MethodDesc* pResult = nullptr;

    BEGIN_QCALL;

    TypeHandle typeHandle = pTypeHandle.AsTypeHandle();
    TypeHandle thOwnerOfMD = pOwner.AsTypeHandle();

    if (pMD->IsStatic())
    {
        pResult = typeHandle.GetMethodTable()->ResolveVirtualStaticMethod(
            thOwnerOfMD.GetMethodTable(),
            pMD,
            ResolveVirtualStaticMethodFlags::AllowNullResult |
            ResolveVirtualStaticMethodFlags::AllowVariantMatches);
    }
    else
    {
        // Ok to have INVALID_SLOT in the case where abstract class does not implement an interface method.
        // This case can not be reproed using C# "implements" all interface methods
        // with at least an abstract method. b19897_GetInterfaceMap_Abstract.exe tests this case.
        //@TODO:STUBDISPATCH: Don't need to track down the implementation, just the declaration, and this can
        //@TODO:              be done faster - just need to make a function FindDispatchDecl.
        DispatchSlot slot(typeHandle.GetMethodTable()->FindDispatchSlotForInterfaceMD(thOwnerOfMD, pMD, FALSE /* throwOnConflict */));
        if (!slot.IsNull())
            pResult = slot.GetMethodDesc();
    }

    END_QCALL;

    return pResult;
}

FCIMPL1(ReflectMethodObject*, RuntimeTypeHandle::GetDeclaringMethod, ReflectClassBaseObject *pTypeUNSAFE) {
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);

    if (refType == NULL)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    TypeHandle typeHandle = refType->GetType();;

    if (!typeHandle.IsTypeDesc())
        return NULL;

    TypeVarTypeDesc* pGenericVariable = typeHandle.AsGenericVariable();
    mdToken defToken = pGenericVariable->GetTypeOrMethodDef();
    if (TypeFromToken(defToken) != mdtMethodDef)
        return NULL;

    REFLECTMETHODREF pRet = NULL;
    HELPER_METHOD_FRAME_BEGIN_RET_0();
    MethodDesc * pMD = pGenericVariable->LoadOwnerMethod();
    pMD->CheckRestore();
    pRet = pMD->GetStubMethodInfo();
    HELPER_METHOD_FRAME_END();

    return (ReflectMethodObject*)OBJECTREFToObject(pRet);
}
FCIMPLEND

extern "C" EnregisteredTypeHandle QCALLTYPE RuntimeTypeHandle_GetDeclaringTypeHandleForGenericVariable(EnregisteredTypeHandle pTypeHandle)
{
    QCALL_CONTRACT;

    TypeHandle retTypeHandle;

    BEGIN_QCALL;

    TypeHandle typeHandle = TypeHandle::FromPtr(pTypeHandle);
    _ASSERTE(typeHandle.IsGenericVariable());

    TypeVarTypeDesc* pGenericVariable = typeHandle.AsGenericVariable();
    mdToken defToken = pGenericVariable->GetTypeOrMethodDef();

    // Try the fast way first (if the declaring type has been loaded already).
    if (TypeFromToken(defToken) == mdtMethodDef)
    {
        MethodDesc* retMethod = pGenericVariable->GetModule()->LookupMethodDef(defToken);
        if (retMethod != NULL)
            retTypeHandle = retMethod->GetMethodTable();
    }
    else
    {
        retTypeHandle = pGenericVariable->GetModule()->LookupTypeDef(defToken);
    }

    // Check if we need to go the slow way and load the type first.
    if (retTypeHandle.IsNull() || !retTypeHandle.IsFullyLoaded())
    {
        if (TypeFromToken(defToken) == mdtMethodDef)
        {
            retTypeHandle = pGenericVariable->LoadOwnerMethod()->GetMethodTable();
        }
        else
        {
            retTypeHandle = pGenericVariable->LoadOwnerType();
        }
        retTypeHandle.CheckRestore();
    }

    END_QCALL;

    return (EnregisteredTypeHandle)retTypeHandle.AsTAddr();
}

extern "C" EnregisteredTypeHandle QCALLTYPE RuntimeTypeHandle_GetDeclaringTypeHandle(EnregisteredTypeHandle pTypeHandle)
{
    QCALL_CONTRACT;

    TypeHandle retTypeHandle;

    BEGIN_QCALL;

    TypeHandle typeHandle = TypeHandle::FromPtr(pTypeHandle);
    _ASSERTE(!typeHandle.IsTypeDesc());

    MethodTable* pMT = typeHandle.GetMethodTable();
    if (pMT->GetClass()->IsNested())
    {
        mdTypeDef tkTypeDef = pMT->GetCl();
        if (FAILED(typeHandle.GetModule()->GetMDImport()->GetNestedClassProps(tkTypeDef, &tkTypeDef)))
            COMPlusThrow(kBadImageFormatException);

        // Try the fast way first (if the declaring type has been loaded already).
        retTypeHandle = typeHandle.GetModule()->LookupTypeDef(tkTypeDef);
        if (retTypeHandle.IsNull())
        {
            // OK, need to go the slow way and load the type first.
            retTypeHandle = ClassLoader::LoadTypeDefThrowing(typeHandle.GetModule(), tkTypeDef,
                                                            ClassLoader::ThrowIfNotFound,
                                                            ClassLoader::PermitUninstDefOrRef);
        }
    }

    END_QCALL;

    return (EnregisteredTypeHandle)retTypeHandle.AsTAddr();
}

FCIMPL2(FC_BOOL_RET, RuntimeTypeHandle::CanCastTo, ReflectClassBaseObject *pTypeUNSAFE, ReflectClassBaseObject *pTargetUNSAFE) {
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;


    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);
    REFLECTCLASSBASEREF refTarget = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTargetUNSAFE);

    if ((refType == NULL) || (refTarget == NULL))
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    TypeHandle fromHandle = refType->GetType();
    TypeHandle toHandle = refTarget->GetType();

    TypeHandle::CastResult r = fromHandle.CanCastToCached(toHandle);
    if (r != TypeHandle::MaybeCast)
    {
        FC_RETURN_BOOL((BOOL)r);
    }

    BOOL iRetVal = FALSE;
    HELPER_METHOD_FRAME_BEGIN_RET_2(refType, refTarget);
    {
        // We allow T to be cast to Nullable<T>
        if (!fromHandle.IsTypeDesc() && Nullable::IsNullableForType(toHandle, fromHandle.AsMethodTable()))
        {
            // do not put this in the cache (see TypeHandle::CanCastTo and ObjIsInstanceOfCore).
            iRetVal = TRUE;
        }
        else
        {
            if (fromHandle.IsTypeDesc())
            {
                iRetVal = fromHandle.AsTypeDesc()->CanCastTo(toHandle, /* pVisited */ NULL);
            }
            else if (toHandle.IsTypeDesc())
            {
                iRetVal = FALSE;
                CastCache::TryAddToCache(fromHandle, toHandle, FALSE);
            }
            else
            {
                iRetVal = fromHandle.AsMethodTable()->CanCastTo(toHandle.AsMethodTable(), /* pVisited */ NULL);
            }
        }
    }
    HELPER_METHOD_FRAME_END();

    FC_RETURN_BOOL(iRetVal);
}
FCIMPLEND

FCIMPL6(FC_BOOL_RET, RuntimeTypeHandle::SatisfiesConstraints, PTR_ReflectClassBaseObject pParamTypeUNSAFE, TypeHandle *typeContextArgs, INT32 typeContextCount, TypeHandle *methodContextArgs, INT32 methodContextCount, PTR_ReflectClassBaseObject pArgumentTypeUNSAFE);
{
    CONTRACTL {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(typeContextArgs, NULL_OK));
        PRECONDITION(CheckPointer(methodContextArgs, NULL_OK));
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refParamType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pParamTypeUNSAFE);
    REFLECTCLASSBASEREF refArgumentType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pArgumentTypeUNSAFE);

    TypeHandle thGenericParameter = refParamType->GetType();
    TypeHandle thGenericArgument = refArgumentType->GetType();
    BOOL bResult = FALSE;
    SigTypeContext typeContext;

    Instantiation classInst;
    Instantiation methodInst;

    if (typeContextArgs != NULL)
    {
        classInst = Instantiation(typeContextArgs, typeContextCount);
    }

    if (methodContextArgs != NULL)
    {
        methodInst = Instantiation(methodContextArgs, methodContextCount);
    }

    SigTypeContext::InitTypeContext(classInst, methodInst, &typeContext);

    HELPER_METHOD_FRAME_BEGIN_RET_2(refParamType, refArgumentType);
    {
        bResult = thGenericParameter.AsGenericVariable()->SatisfiesConstraints(&typeContext, thGenericArgument);
    }
    HELPER_METHOD_FRAME_END();

    FC_RETURN_BOOL(bResult);
}
FCIMPLEND

extern "C" void QCALLTYPE RuntimeTypeHandle_GetInstantiation(QCall::TypeHandle pType, QCall::ObjectHandleOnStack retTypes, BOOL fAsRuntimeTypeArray)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    TypeHandle typeHandle = pType.AsTypeHandle();
    Instantiation inst = typeHandle.GetInstantiation();
    GCX_COOP();
    retTypes.Set(CopyRuntimeTypeHandles(inst.GetRawArgs(), inst.GetNumArgs(), fAsRuntimeTypeArray ? CLASS__CLASS : CLASS__TYPE));
    END_QCALL;

    return;
}

extern "C" void QCALLTYPE RuntimeTypeHandle_MakeArray(QCall::TypeHandle pTypeHandle, INT32 rank, QCall::ObjectHandleOnStack retType)
{
    QCALL_CONTRACT;

    TypeHandle arrayHandle;

    BEGIN_QCALL;
    arrayHandle = pTypeHandle.AsTypeHandle().MakeArray(rank);
    GCX_COOP();
    retType.Set(arrayHandle.GetManagedClassObject());
    END_QCALL;

    return;
}

extern "C" void QCALLTYPE RuntimeTypeHandle_MakeSZArray(QCall::TypeHandle pTypeHandle, QCall::ObjectHandleOnStack retType)
{
    QCALL_CONTRACT;

    TypeHandle arrayHandle;

    BEGIN_QCALL;
    arrayHandle = pTypeHandle.AsTypeHandle().MakeSZArray();
    GCX_COOP();
    retType.Set(arrayHandle.GetManagedClassObject());
    END_QCALL;

    return;
}

extern "C" void QCALLTYPE RuntimeTypeHandle_MakePointer(QCall::TypeHandle pTypeHandle, QCall::ObjectHandleOnStack retType)
{
    QCALL_CONTRACT;

    TypeHandle pointerHandle;

    BEGIN_QCALL;
    pointerHandle = pTypeHandle.AsTypeHandle().MakePointer();
    GCX_COOP();
    retType.Set(pointerHandle.GetManagedClassObject());
    END_QCALL;

    return;
}

extern "C" void QCALLTYPE RuntimeTypeHandle_MakeByRef(QCall::TypeHandle pTypeHandle, QCall::ObjectHandleOnStack retType)
{
    QCALL_CONTRACT;

    TypeHandle byRefHandle;

    BEGIN_QCALL;
    byRefHandle = pTypeHandle.AsTypeHandle().MakeByRef();
    GCX_COOP();
    retType.Set(byRefHandle.GetManagedClassObject());
    END_QCALL;

    return;
}

extern "C" BOOL QCALLTYPE RuntimeTypeHandle_IsCollectible(QCall::TypeHandle pTypeHandle)
{
    QCALL_CONTRACT;

    BOOL retVal = FALSE;

    BEGIN_QCALL;
    retVal = pTypeHandle.AsTypeHandle().GetLoaderAllocator()->IsCollectible();
    END_QCALL;

    return retVal;
}

extern "C" void QCALLTYPE RuntimeTypeHandle_Instantiate(QCall::TypeHandle pTypeHandle, TypeHandle * pInstArray, INT32 cInstArray, QCall::ObjectHandleOnStack retType)
{
    QCALL_CONTRACT;

    TypeHandle type;

    BEGIN_QCALL;
    type = pTypeHandle.AsTypeHandle().Instantiate(Instantiation(pInstArray, cInstArray));
    GCX_COOP();
    retType.Set(type.GetManagedClassObject());
    END_QCALL;

    return;
}

extern "C" void QCALLTYPE RuntimeTypeHandle_GetGenericTypeDefinition(QCall::TypeHandle pTypeHandle, QCall::ObjectHandleOnStack retType)
{
    QCALL_CONTRACT;

    TypeHandle typeDef;

    BEGIN_QCALL;

    TypeHandle genericType = pTypeHandle.AsTypeHandle();

    typeDef = ClassLoader::LoadTypeDefThrowing(genericType.GetModule(),
                                                       genericType.GetMethodTable()->GetCl(),
                                                       ClassLoader::ThrowIfNotFound,
                                                       ClassLoader::PermitUninstDefOrRef);

    GCX_COOP();
    retType.Set(typeDef.GetManagedClassObject());

    END_QCALL;

    return;
}

FCIMPL2(FC_BOOL_RET, RuntimeTypeHandle::CompareCanonicalHandles, ReflectClassBaseObject *pLeftUNSAFE, ReflectClassBaseObject *pRightUNSAFE)
{
    FCALL_CONTRACT;

    REFLECTCLASSBASEREF refLeft = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pLeftUNSAFE);
    REFLECTCLASSBASEREF refRight = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pRightUNSAFE);

    if ((refLeft == NULL) || (refRight == NULL))
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    FC_RETURN_BOOL(refLeft->GetType().GetCanonicalMethodTable() == refRight->GetType().GetCanonicalMethodTable());
}
FCIMPLEND

FCIMPL1(FC_BOOL_RET, RuntimeTypeHandle::IsGenericVariable, PTR_ReflectClassBaseObject pTypeUNSAFE)
{
    FCALL_CONTRACT;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);

    if (refType == NULL)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    FC_RETURN_BOOL(refType->GetType().IsGenericVariable());
}
FCIMPLEND

FCIMPL1(INT32, RuntimeTypeHandle::GetGenericVariableIndex, PTR_ReflectClassBaseObject pTypeUNSAFE)
{
    FCALL_CONTRACT;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);

    if (refType == NULL)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    return (INT32)refType->GetType().AsGenericVariable()->GetIndex();
}
FCIMPLEND

FCIMPL1(FC_BOOL_RET, RuntimeTypeHandle::ContainsGenericVariables, PTR_ReflectClassBaseObject pTypeUNSAFE)
{
    FCALL_CONTRACT;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);

    if (refType == NULL)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    FC_RETURN_BOOL(refType->GetType().ContainsGenericVariables());
}
FCIMPLEND

extern "C" void* QCALLTYPE RuntimeTypeHandle_AllocateTypeAssociatedMemory(QCall::TypeHandle type, uint32_t size)
{
    QCALL_CONTRACT;

    void *allocatedMemory = nullptr;

    BEGIN_QCALL;

    TypeHandle typeHandle = type.AsTypeHandle();
    _ASSERTE(!typeHandle.IsNull());

    // Get the loader allocator for the associated type.
    // Allocating using the type's associated loader allocator means
    // that the memory will be freed when the type is unloaded.
    PTR_LoaderAllocator loaderAllocator = typeHandle.GetMethodTable()->GetLoaderAllocator();
    LoaderHeap* loaderHeap = loaderAllocator->GetHighFrequencyHeap();
    allocatedMemory = loaderHeap->AllocMem(S_SIZE_T(size));

    END_QCALL;

    return allocatedMemory;
}

extern "C" void QCALLTYPE RuntimeTypeHandle_RegisterCollectibleTypeDependency(QCall::TypeHandle pTypeHandle, QCall::AssemblyHandle pAssembly)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    LoaderAllocator* pLoaderAllocator = pTypeHandle.AsTypeHandle().GetLoaderAllocator();

    if (pLoaderAllocator->IsCollectible())
    {
        if ((pAssembly == NULL) || !pAssembly->GetLoaderAllocator()->IsCollectible())
        {
            COMPlusThrow(kNotSupportedException, W("NotSupported_CollectibleBoundNonCollectible"));
        }
        else
        {
            pAssembly->GetLoaderAllocator()->EnsureReference(pLoaderAllocator);
        }
    }

    END_QCALL;
}

//***********************************************************************************
//***********************************************************************************
//***********************************************************************************

extern "C" void * QCALLTYPE RuntimeMethodHandle_GetFunctionPointer(MethodDesc * pMethod)
{
    QCALL_CONTRACT;

    void* funcPtr = NULL;

    BEGIN_QCALL;

    // Ensure the method is active and all types have been loaded so the function pointer can be used.
    pMethod->EnsureActive();
    pMethod->PrepareForUseAsAFunctionPointer();
    funcPtr = (void*)pMethod->GetMultiCallableAddrOfCode();

    END_QCALL;

    return funcPtr;
}

extern "C" BOOL QCALLTYPE RuntimeMethodHandle_GetIsCollectible(MethodDesc* pMethod)
{
    QCALL_CONTRACT;

    BOOL isCollectible = FALSE;

    BEGIN_QCALL;

    isCollectible = pMethod->GetLoaderAllocator()->IsCollectible();

    END_QCALL;

    return isCollectible;
}

FCIMPL1(LPCUTF8, RuntimeMethodHandle::GetUtf8Name, MethodDesc* pMethod)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(pMethod != NULL);
    }
    CONTRACTL_END;

    return pMethod->GetName();
}
FCIMPLEND

FCIMPL1(INT32, RuntimeMethodHandle::GetAttributes, MethodDesc *pMethod) {
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    if (!pMethod)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    return (INT32)pMethod->GetAttrs();
}
FCIMPLEND

FCIMPL1(INT32, RuntimeMethodHandle::GetImplAttributes, ReflectMethodObject *pMethodUNSAFE) {
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    if (!pMethodUNSAFE)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    MethodDesc* pMethod = pMethodUNSAFE->GetMethod();
    INT32 attributes = 0;

    if (IsNilToken(pMethod->GetMemberDef()))
        return attributes;

    return (INT32)pMethod->GetImplAttrs();
}
FCIMPLEND

FCIMPL1(MethodTable*, RuntimeMethodHandle::GetMethodTable, MethodDesc *pMethod)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(pMethod != NULL);
    }
    CONTRACTL_END;

    return pMethod->GetMethodTable();
}
FCIMPLEND

FCIMPL1(INT32, RuntimeMethodHandle::GetSlot, MethodDesc *pMethod) {
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    if (!pMethod)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    return (INT32)pMethod->GetSlot();
}
FCIMPLEND

FCIMPL2(INT32, SignatureNative::GetParameterOffset, SignatureNative* pSignatureUNSAFE, INT32 parameterIndex)
{
    FCALL_CONTRACT;

    struct
    {
        SIGNATURENATIVEREF pSig;
    } gc;

    gc.pSig = (SIGNATURENATIVEREF)pSignatureUNSAFE;

    INT32 offset = 0;

    HELPER_METHOD_FRAME_BEGIN_RET_PROTECT(gc);
    {
        SigPointer sp(gc.pSig->GetCorSig(), gc.pSig->GetCorSigSize());

        uint32_t callConv = 0;
        IfFailThrow(sp.GetCallingConvInfo(&callConv));

        if ((callConv & IMAGE_CEE_CS_CALLCONV_MASK) != IMAGE_CEE_CS_CALLCONV_FIELD)
        {
            if (callConv & IMAGE_CEE_CS_CALLCONV_GENERIC)
            {
                IfFailThrow(sp.GetData(NULL));
            }

            uint32_t numArgs;
            IfFailThrow(sp.GetData(&numArgs));
            _ASSERTE((uint32_t)parameterIndex <= numArgs);

            for (int i = 0; i < parameterIndex; i++)
                IfFailThrow(sp.SkipExactlyOne());
        }
        else
        {
            _ASSERTE(parameterIndex == 0);
        }

        offset = (INT32)(sp.GetPtr() - gc.pSig->GetCorSig());
    }
    HELPER_METHOD_FRAME_END();

    return offset;
}
FCIMPLEND

FCIMPL3(INT32, SignatureNative::GetTypeParameterOffset, SignatureNative* pSignatureUNSAFE, INT32 offset, INT32 index)
{
    FCALL_CONTRACT;

    struct
    {
        SIGNATURENATIVEREF pSig;
    } gc;

    if (offset < 0)
    {
        _ASSERTE(offset == -1);
        return offset;
    }

    gc.pSig = (SIGNATURENATIVEREF)pSignatureUNSAFE;

    HELPER_METHOD_FRAME_BEGIN_RET_PROTECT(gc);
    {
        SigPointer sp(gc.pSig->GetCorSig() + offset, gc.pSig->GetCorSigSize() - offset);

        CorElementType etype;
        IfFailThrow(sp.GetElemType(&etype));

        uint32_t argCnt;

        switch (etype)
        {
        case ELEMENT_TYPE_FNPTR:
            IfFailThrow(sp.SkipMethodHeaderSignature(&argCnt, /* skipReturnType */ false));
            _ASSERTE((uint32_t)index <= argCnt);
            break;
        case ELEMENT_TYPE_GENERICINST:
            IfFailThrow(sp.SkipExactlyOne());

            IfFailThrow(sp.GetData(&argCnt));
            _ASSERTE((uint32_t)index < argCnt);
            break;
        case ELEMENT_TYPE_ARRAY:
        case ELEMENT_TYPE_SZARRAY:
        case ELEMENT_TYPE_BYREF:
        case ELEMENT_TYPE_PTR:
            _ASSERTE(index == 0);
            break;
        case ELEMENT_TYPE_VAR:
        case ELEMENT_TYPE_MVAR:
            offset = -1; // Use offset -1 to signal method substituted method variable. We do not have full signature for those.
            goto Done;
        default:
            _ASSERTE(false); // Unexpected element type
            offset = -1;
            goto Done;
        }

        for (int i = 0; i < index; i++)
            IfFailThrow(sp.SkipExactlyOne());

        offset = (INT32)(sp.GetPtr() - gc.pSig->GetCorSig());
    Done: ;
    }
    HELPER_METHOD_FRAME_END();

    return offset;
}
FCIMPLEND

FCIMPL2(FC_INT8_RET, SignatureNative::GetCallingConventionFromFunctionPointerAtOffset, SignatureNative* pSignatureUNSAFE, INT32 offset)
{
    FCALL_CONTRACT;

    struct
    {
        SIGNATURENATIVEREF pSig;
    } gc;

    if (offset < 0)
    {
        _ASSERTE(offset == -1);
        return 0;
    }

    gc.pSig = (SIGNATURENATIVEREF)pSignatureUNSAFE;

    uint32_t callConv = 0;

    HELPER_METHOD_FRAME_BEGIN_RET_PROTECT(gc);
    {
        SigPointer sp(gc.pSig->GetCorSig() + offset, gc.pSig->GetCorSigSize() - offset);

        CorElementType etype;
        IfFailThrow(sp.GetElemType(&etype));
        _ASSERTE(etype == ELEMENT_TYPE_FNPTR);

        IfFailThrow(sp.GetCallingConv(&callConv));
    }
    HELPER_METHOD_FRAME_END();

    return (FC_INT8_RET)(callConv);
}
FCIMPLEND

FCIMPL3(Object *, SignatureNative::GetCustomModifiersAtOffset,
    SignatureNative* pSignatureUNSAFE,
    INT32 offset,
    FC_BOOL_ARG fRequired)
{
    FCALL_CONTRACT;

    struct
    {
        SIGNATURENATIVEREF pSig;
        PTRARRAYREF retVal;
    } gc;

    gc.pSig = (SIGNATURENATIVEREF)pSignatureUNSAFE;
    gc.retVal = NULL;

    HELPER_METHOD_FRAME_BEGIN_RET_PROTECT(gc);
    {
        SigTypeContext typeContext;
        gc.pSig->GetTypeContext(&typeContext);

        SigPointer argument(gc.pSig->GetCorSig() + offset, gc.pSig->GetCorSigSize() - offset);

        SigPointer sp = argument;
        Module* pModule = gc.pSig->GetModule();
        INT32 cMods = 0;
        CorElementType cmodType;

        CorElementType cmodTypeExpected = FC_ACCESS_BOOL(fRequired) ? ELEMENT_TYPE_CMOD_REQD : ELEMENT_TYPE_CMOD_OPT;

        // Discover the number of required and optional custom modifiers.
        while(TRUE)
        {
            BYTE data;
            IfFailThrow(sp.GetByte(&data));
            cmodType = (CorElementType)data;

            if (cmodType == ELEMENT_TYPE_CMOD_REQD || cmodType == ELEMENT_TYPE_CMOD_OPT)
            {
                if (cmodType == cmodTypeExpected)
                {
                    cMods ++;
                }

                IfFailThrow(sp.GetToken(NULL));
            }
            else if (cmodType == ELEMENT_TYPE_CMOD_INTERNAL)
            {
                BYTE required;
                IfFailThrow(sp.GetByte(&required));
                if (fRequired == (required != 0))
                {
                    cMods ++;
                }

                IfFailThrow(sp.GetPointer(NULL));
            }
            else if (cmodType != ELEMENT_TYPE_SENTINEL)
            {
                break;
            }
        }

        // Reset sp and populate the arrays for the required and optional custom
        // modifiers now that we know how long they should be.
        sp = argument;

        MethodTable *pMT = CoreLibBinder::GetClass(CLASS__TYPE);
        TypeHandle arrayHandle = ClassLoader::LoadArrayTypeThrowing(TypeHandle(pMT), ELEMENT_TYPE_SZARRAY);

        gc.retVal = (PTRARRAYREF) AllocateSzArray(arrayHandle, cMods);

        while(cMods != 0)
        {
            BYTE data;
            IfFailThrow(sp.GetByte(&data));
            cmodType = (CorElementType)data;

            if (cmodType == ELEMENT_TYPE_CMOD_INTERNAL)
            {
                BYTE required;
                IfFailThrow(sp.GetByte(&required));

                TypeHandle th;
                IfFailThrow(sp.GetPointer((void**)&th));

                if (fRequired == (required != 0))
                {
                    OBJECTREF refType = th.GetManagedClassObject();
                    gc.retVal->SetAt(--cMods, refType);
                }
            }
            else
            {
                mdToken token;
                IfFailThrow(sp.GetToken(&token));

                if (cmodType == cmodTypeExpected)
                {
                    TypeHandle th = ClassLoader::LoadTypeDefOrRefOrSpecThrowing(pModule, token,
                                                                                &typeContext,
                                                                                ClassLoader::ThrowIfNotFound,
                                                                                ClassLoader::FailIfUninstDefOrRef);

                    OBJECTREF refType = th.GetManagedClassObject();
                    gc.retVal->SetAt(--cMods, refType);
                }
            }
        }
    }
    HELPER_METHOD_FRAME_END();

    return OBJECTREFToObject(gc.retVal);
}
FCIMPLEND

FCIMPL1(INT32, RuntimeMethodHandle::GetMethodDef, ReflectMethodObject *pMethodUNSAFE) {
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    if (!pMethodUNSAFE)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    MethodDesc* pMethod = pMethodUNSAFE->GetMethod();

    if (pMethod->HasMethodInstantiation())
    {
        HELPER_METHOD_FRAME_BEGIN_RET_1(pMethodUNSAFE);
        {
            pMethod = pMethod->StripMethodInstantiation();
        }
        HELPER_METHOD_FRAME_END();
    }

    INT32 tkMethodDef = (INT32)pMethod->GetMemberDef();
    _ASSERTE(TypeFromToken(tkMethodDef) == mdtMethodDef);

    if (IsNilToken(tkMethodDef) || TypeFromToken(tkMethodDef) != mdtMethodDef)
        return mdMethodDefNil;

    return tkMethodDef;
}
FCIMPLEND

FCIMPL6(void, SignatureNative::GetSignature,
    SignatureNative* pSignatureNativeUNSAFE,
    PCCOR_SIGNATURE pCorSig, DWORD cCorSig,
    FieldDesc *pFieldDesc, ReflectMethodObject *pMethodUNSAFE, ReflectClassBaseObject *pDeclaringTypeUNSAFE) {
    CONTRACTL {
        FCALL_CHECK;
        PRECONDITION(pDeclaringTypeUNSAFE || pMethodUNSAFE->GetMethod()->IsDynamicMethod());
        PRECONDITION(CheckPointer(pCorSig, NULL_OK));
        PRECONDITION(CheckPointer(pMethodUNSAFE, NULL_OK));
        PRECONDITION(CheckPointer(pFieldDesc, NULL_OK));
    }
    CONTRACTL_END;

    struct
    {
        REFLECTCLASSBASEREF refDeclaringType;
        REFLECTMETHODREF refMethod;
        SIGNATURENATIVEREF pSig;
    } gc;

    gc.refDeclaringType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pDeclaringTypeUNSAFE);
    gc.refMethod = (REFLECTMETHODREF)ObjectToOBJECTREF(pMethodUNSAFE);
    gc.pSig = (SIGNATURENATIVEREF)pSignatureNativeUNSAFE;

    MethodDesc *pMethod;
    TypeHandle declType;

    if (gc.refDeclaringType == NULL)
    {
        // for dynamic method, see precondition
        pMethod = gc.refMethod->GetMethod();
        declType = pMethod->GetMethodTable();
    }
    else
    {
        pMethod = gc.refMethod != NULL ? gc.refMethod->GetMethod() : NULL;
        declType = gc.refDeclaringType->GetType();
    }

    HELPER_METHOD_FRAME_BEGIN_PROTECT(gc);
    {
        Module* pModule = declType.GetModule();

        if (pMethod)
        {
            pMethod->GetSig(&pCorSig, &cCorSig);
            if (pMethod->GetClassification() == mcInstantiated)
            {
                LoaderAllocator *pLoaderAllocator = pMethod->GetLoaderAllocator();
                if (pLoaderAllocator->IsCollectible())
                    gc.pSig->SetKeepAlive(pLoaderAllocator->GetExposedObject());
            }
        }
        else if (pFieldDesc)
            pFieldDesc->GetSig(&pCorSig, &cCorSig);

        gc.pSig->m_sig = pCorSig;
        gc.pSig->m_cSig = cCorSig;
        gc.pSig->m_pMethod = pMethod;

        REFLECTCLASSBASEREF refDeclType = (REFLECTCLASSBASEREF)declType.GetManagedClassObject();
        gc.pSig->SetDeclaringType(refDeclType);

        PREFIX_ASSUME(pCorSig!= NULL);
        BYTE callConv = *(BYTE*)pCorSig;
        SigTypeContext typeContext;
        if (pMethod)
            SigTypeContext::InitTypeContext(
                pMethod, declType.GetClassOrArrayInstantiation(), pMethod->LoadMethodInstantiation(), &typeContext);
        else
            SigTypeContext::InitTypeContext(declType, &typeContext);

        MetaSig msig(pCorSig, cCorSig, pModule, &typeContext,
            (callConv & IMAGE_CEE_CS_CALLCONV_MASK) == IMAGE_CEE_CS_CALLCONV_FIELD ? MetaSig::sigField : MetaSig::sigMember);

        if (callConv == IMAGE_CEE_CS_CALLCONV_FIELD)
        {
            msig.NextArgNormalized();

            OBJECTREF refRetType = msig.GetLastTypeHandleThrowing().GetManagedClassObject();
            gc.pSig->SetReturnType(refRetType);
        }
        else
        {
            gc.pSig->SetCallingConvention(msig.GetCallingConventionInfo());

            OBJECTREF refRetType = msig.GetRetTypeHandleThrowing().GetManagedClassObject();
            gc.pSig->SetReturnType(refRetType);

            INT32 nArgs = msig.NumFixedArgs();
            TypeHandle arrayHandle = ClassLoader::LoadArrayTypeThrowing(TypeHandle(g_pRuntimeTypeClass), ELEMENT_TYPE_SZARRAY);

            PTRARRAYREF ptrArrayarguments = (PTRARRAYREF) AllocateSzArray(arrayHandle, nArgs);
            gc.pSig->SetArgumentArray(ptrArrayarguments);

            for (INT32 i = 0; i < nArgs; i++)
            {
                msig.NextArg();

                OBJECTREF refArgType = msig.GetLastTypeHandleThrowing().GetManagedClassObject();
                gc.pSig->SetArgument(i, refArgType);
            }

            _ASSERTE(gc.pSig->m_returnType != NULL);
        }
    }
    HELPER_METHOD_FRAME_END();
}
FCIMPLEND

FCIMPL2(FC_BOOL_RET, SignatureNative::CompareSig, SignatureNative* pLhsUNSAFE, SignatureNative* pRhsUNSAFE)
{
    FCALL_CONTRACT;

    INT32 ret = 0;

    struct
    {
        SIGNATURENATIVEREF pLhs;
        SIGNATURENATIVEREF pRhs;
    } gc;

    gc.pLhs = (SIGNATURENATIVEREF)pLhsUNSAFE;
    gc.pRhs = (SIGNATURENATIVEREF)pRhsUNSAFE;

    HELPER_METHOD_FRAME_BEGIN_RET_PROTECT(gc);
    {
        ret = MetaSig::CompareMethodSigs(
            gc.pLhs->GetCorSig(), gc.pLhs->GetCorSigSize(), gc.pLhs->GetModule(), NULL,
            gc.pRhs->GetCorSig(), gc.pRhs->GetCorSigSize(), gc.pRhs->GetModule(), NULL,
            FALSE);
    }
    HELPER_METHOD_FRAME_END();
    FC_RETURN_BOOL(ret);
}
FCIMPLEND

extern "C" void QCALLTYPE RuntimeMethodHandle_GetMethodInstantiation(MethodDesc * pMethod, QCall::ObjectHandleOnStack retTypes, BOOL fAsRuntimeTypeArray)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;
    Instantiation inst = pMethod->LoadMethodInstantiation();

    GCX_COOP();
    retTypes.Set(CopyRuntimeTypeHandles(inst.GetRawArgs(), inst.GetNumArgs(), fAsRuntimeTypeArray ? CLASS__CLASS : CLASS__TYPE));
    END_QCALL;

    return;
}

FCIMPL1(FC_BOOL_RET, RuntimeMethodHandle::HasMethodInstantiation, MethodDesc * pMethod)
{
    FCALL_CONTRACT;

    FC_RETURN_BOOL(pMethod->HasMethodInstantiation());
}
FCIMPLEND

FCIMPL1(FC_BOOL_RET, RuntimeMethodHandle::IsGenericMethodDefinition, MethodDesc * pMethod)
{
    FCALL_CONTRACT;

    FC_RETURN_BOOL(pMethod->IsGenericMethodDefinition());
}
FCIMPLEND

FCIMPL1(INT32, RuntimeMethodHandle::GetGenericParameterCount, MethodDesc * pMethod)
{
    FCALL_CONTRACT;

    return pMethod->GetNumGenericMethodArgs();
}
FCIMPLEND

FCIMPL1(FC_BOOL_RET, RuntimeMethodHandle::IsDynamicMethod, MethodDesc * pMethod)
{
    FCALL_CONTRACT;

    FC_RETURN_BOOL(pMethod->IsNoMetadata());
}
FCIMPLEND

FCIMPL1(Object*, RuntimeMethodHandle::GetResolver, MethodDesc * pMethod)
{
    FCALL_CONTRACT;

    if (!pMethod)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    OBJECTREF resolver = NULL;
    if (pMethod->IsLCGMethod())
    {
        resolver = pMethod->AsDynamicMethodDesc()->GetLCGMethodResolver()->GetManagedResolver();
    }
    return OBJECTREFToObject(resolver);
}
FCIMPLEND

extern "C" void QCALLTYPE RuntimeMethodHandle_Destroy(MethodDesc * pMethod)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    if (pMethod == NULL)
        COMPlusThrowArgumentNull(NULL, W("Arg_InvalidHandle"));

    DynamicMethodDesc* pDynamicMethodDesc = pMethod->AsDynamicMethodDesc();

    GCX_COOP();

    // Destroy should be called only if the managed part is gone.
    _ASSERTE(OBJECTREFToObject(pDynamicMethodDesc->GetLCGMethodResolver()->GetManagedResolver()) == NULL);

    // Fire Unload Dynamic Method Event here
    ETW::MethodLog::DynamicMethodDestroyed(pMethod);

    BEGIN_PROFILER_CALLBACK(CORProfilerTrackDynamicFunctionUnloads());
    (&g_profControlBlock)->DynamicMethodUnloaded((FunctionID)pMethod);
    END_PROFILER_CALLBACK();

    pDynamicMethodDesc->Destroy();

    END_QCALL;
    }

FCIMPL1(FC_BOOL_RET, RuntimeMethodHandle::IsTypicalMethodDefinition, ReflectMethodObject *pMethodUNSAFE)
{
    FCALL_CONTRACT;

    if (!pMethodUNSAFE)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    MethodDesc* pMethod = pMethodUNSAFE->GetMethod();

    FC_RETURN_BOOL(pMethod->IsTypicalMethodDefinition());
}
FCIMPLEND

extern "C" void QCALLTYPE RuntimeMethodHandle_GetTypicalMethodDefinition(MethodDesc * pMethod, QCall::ObjectHandleOnStack refMethod)
    {
    QCALL_CONTRACT;

    BEGIN_QCALL;
#ifdef _DEBUG
    {
        GCX_COOP();
        _ASSERTE(((ReflectMethodObject *)(*refMethod.m_ppObject))->GetMethod() == pMethod);
    }
#endif
    MethodDesc *pMethodTypical = pMethod->LoadTypicalMethodDefinition();
    if (pMethodTypical != pMethod)
    {
        GCX_COOP();
        refMethod.Set(pMethodTypical->GetStubMethodInfo());
    }
    END_QCALL;

    return;
}

extern "C" void QCALLTYPE RuntimeMethodHandle_StripMethodInstantiation(MethodDesc * pMethod, QCall::ObjectHandleOnStack refMethod)
    {
    QCALL_CONTRACT;

    BEGIN_QCALL;

    if (!pMethod)
        COMPlusThrowArgumentNull(NULL, W("Arg_InvalidHandle"));

#ifdef _DEBUG
    {
        GCX_COOP();
        _ASSERTE(((ReflectMethodObject *)(*refMethod.m_ppObject))->GetMethod() == pMethod);
    }
#endif
    MethodDesc *pMethodStripped = pMethod->StripMethodInstantiation();
    if (pMethodStripped != pMethod)
    {
        GCX_COOP();
        refMethod.Set(pMethodStripped->GetStubMethodInfo());
    }
    END_QCALL;

    return;
}

// In the VM there might be more than one MethodDescs for a "method"
// examples are methods on generic types which may have additional instantiating stubs
//          and methods on value types which may have additional unboxing stubs.
//
// For generic methods we always hand out an instantiating stub except for a generic method definition
// For non-generic methods on generic types we need an instantiating stub if it's one of the following
//  - static method on a generic class
//  - static or instance method on a generic interface
//  - static or instance method on a generic value type
// The Reflection policy is to always hand out instantiating stubs in these cases
//
// For methods on non-generic value types we can use either the canonical method or the unboxing stub
// The Reflection policy is to always hand out unboxing stubs if the methods are virtual methods
// The reason for this is that in the current implementation of the class loader, the v-table slots for
// those methods point to unboxing stubs already. Note that this is just a implementation choice
// that might change in the future. But we should always keep this Reflection policy an invariant.
//
// For virtual methods on generic value types (intersection of the two cases), reflection will hand
// out an unboxing instantiating stub
//
// GetInstantiatingStub is called to:
// 1. create an InstantiatedMethodDesc for a generic method when calling BindGenericArguments() on a generic
//    method. In this case instArray will not be null.
// 2. create an InstantiatedMethodDesc for a method in a generic class. In this case instArray will be null.
// 3. create an UnboxingStub for a method in a value type. In this case instArray will be null.
// For case 2 and 3, an instantiating stub or unboxing stub might not be needed in which case the original
// MethodDesc is returned.
FCIMPL3(MethodDesc*, RuntimeMethodHandle::GetStubIfNeeded,
    MethodDesc *pMethod,
    ReflectClassBaseObject *pTypeUNSAFE,
    PtrArray* instArrayUNSAFE)
{
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);
    PTRARRAYREF instArray = (PTRARRAYREF)ObjectToOBJECTREF(instArrayUNSAFE);

    if (refType == NULL)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    TypeHandle instType = refType->GetType();
    MethodDesc *pNewMethod = pMethod;

    // error conditions
    if (!pMethod)
        FCThrowRes(kArgumentException, W("Arg_InvalidHandle"));

    if (instType.IsNull())
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    // Perf optimization: this logic is actually duplicated in FindOrCreateAssociatedMethodDescForReflection, but since it
    // is the more common case it's worth the duplicate check here to avoid the helper method frame
    if ( instArray == NULL &&
         ( pMethod->HasMethodInstantiation() ||
           ( !instType.IsValueType() &&
             ( !instType.HasInstantiation() || instType.IsGenericTypeDefinition() ) ) ) )
    {
        return pNewMethod;
    }

    HELPER_METHOD_FRAME_BEGIN_RET_2(refType, instArray);
    {
        TypeHandle *inst = NULL;
        DWORD ntypars = 0;

        if (instArray != NULL)
        {
            ntypars = instArray->GetNumComponents();

            size_t size = ntypars * sizeof(TypeHandle);
            if ((size / sizeof(TypeHandle)) != ntypars) // uint over/underflow
                COMPlusThrow(kArgumentException);
            inst = (TypeHandle*) _alloca(size);

            for (DWORD i = 0; i < ntypars; i++)
            {
                REFLECTCLASSBASEREF instRef = (REFLECTCLASSBASEREF)instArray->GetAt(i);

                if (instRef == NULL)
                    COMPlusThrowArgumentNull(W("inst"), W("ArgumentNull_ArrayElement"));

                inst[i] = instRef->GetType();
            }
        }

        pNewMethod = MethodDesc::FindOrCreateAssociatedMethodDescForReflection(pMethod, instType, Instantiation(inst, ntypars));
    }
    HELPER_METHOD_FRAME_END();

    return pNewMethod;
}
FCIMPLEND


FCIMPL2(MethodDesc*, RuntimeMethodHandle::GetMethodFromCanonical, MethodDesc *pMethod, ReflectClassBaseObject *pTypeUNSAFE)
{
    CONTRACTL {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pMethod));
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pTypeUNSAFE);

    TypeHandle instType = refType->GetType();
    MethodTable* pCanonMT = instType.GetMethodTable()->GetCanonicalMethodTable();
    MethodDesc* pMDescInCanonMT = pCanonMT->GetParallelMethodDesc(pMethod);

    return pMDescInCanonMT;
}
FCIMPLEND


FCIMPL2(RuntimeMethodBody *, RuntimeMethodHandle::GetMethodBody, ReflectMethodObject *pMethodUNSAFE, ReflectClassBaseObject *pDeclaringTypeUNSAFE)
{
    CONTRACTL
    {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    struct _gc
    {
        RUNTIMEMETHODBODYREF MethodBodyObj;
        RUNTIMEEXCEPTIONHANDLINGCLAUSEREF EHClauseObj;
        RUNTIMELOCALVARIABLEINFOREF RuntimeLocalVariableInfoObj;
        U1ARRAYREF                  U1Array;
        BASEARRAYREF                TempArray;
        REFLECTCLASSBASEREF         declaringType;
        REFLECTMETHODREF            refMethod;
    } gc;

    gc.MethodBodyObj = NULL;
    gc.EHClauseObj = NULL;
    gc.RuntimeLocalVariableInfoObj = NULL;
    gc.U1Array              = NULL;
    gc.TempArray            = NULL;
    gc.declaringType        = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pDeclaringTypeUNSAFE);
    gc.refMethod = (REFLECTMETHODREF)ObjectToOBJECTREF(pMethodUNSAFE);


    if (!gc.refMethod)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    MethodDesc* pMethod = gc.refMethod->GetMethod();

    TypeHandle declaringType = gc.declaringType == NULL ? TypeHandle() : gc.declaringType->GetType();

    if (!pMethod->IsIL())
        return NULL;

    HELPER_METHOD_FRAME_BEGIN_RET_PROTECT(gc);
    {
        MethodDesc *pMethodIL = pMethod;
        if (pMethod->IsWrapperStub())
            pMethodIL = pMethod->GetWrappedMethodDesc();

        COR_ILMETHOD* pILHeader = pMethodIL->GetILHeader();

        if (pILHeader)
        {
            MethodTable * pExceptionHandlingClauseMT = CoreLibBinder::GetClass(CLASS__RUNTIME_EH_CLAUSE);
            TypeHandle thEHClauseArray = ClassLoader::LoadArrayTypeThrowing(TypeHandle(pExceptionHandlingClauseMT), ELEMENT_TYPE_SZARRAY);

            MethodTable * pLocalVariableMT = CoreLibBinder::GetClass(CLASS__RUNTIME_LOCAL_VARIABLE_INFO);
            TypeHandle thLocalVariableArray = ClassLoader::LoadArrayTypeThrowing(TypeHandle(pLocalVariableMT), ELEMENT_TYPE_SZARRAY);

            Module* pModule = pMethod->GetModule();
            COR_ILMETHOD_DECODER::DecoderStatus status;
            COR_ILMETHOD_DECODER header(pILHeader, pModule->GetMDImport(), &status);

            if (status != COR_ILMETHOD_DECODER::SUCCESS)
            {
                if (status == COR_ILMETHOD_DECODER::VERIFICATION_ERROR)
                {
                    // Throw a verification HR
                    COMPlusThrowHR(COR_E_VERIFICATION);
                }
                else
                {
                    COMPlusThrowHR(COR_E_BADIMAGEFORMAT);
                }
            }

            gc.MethodBodyObj = (RUNTIMEMETHODBODYREF)AllocateObject(CoreLibBinder::GetClass(CLASS__RUNTIME_METHOD_BODY));

            gc.MethodBodyObj->_maxStackSize = header.GetMaxStack();
            gc.MethodBodyObj->_initLocals = !!(header.GetFlags() & CorILMethod_InitLocals);

            if (header.IsFat())
                gc.MethodBodyObj->_localVarSigToken = header.GetLocalVarSigTok();
            else
                gc.MethodBodyObj->_localVarSigToken = 0;

            // Allocate the array of IL and fill it in from the method header.
            BYTE* pIL = const_cast<BYTE*>(header.Code);
            COUNT_T cIL = header.GetCodeSize();
            gc.U1Array  = (U1ARRAYREF) AllocatePrimitiveArray(ELEMENT_TYPE_U1, cIL);

            SetObjectReference((OBJECTREF*)&gc.MethodBodyObj->_IL, gc.U1Array);
            memcpyNoGCRefs(gc.MethodBodyObj->_IL->GetDataPtr(), pIL, cIL);

            // Allocate the array of exception clauses.
            INT32 cEh = (INT32)header.EHCount();
            const COR_ILMETHOD_SECT_EH* ehInfo = header.EH;
            gc.TempArray = (BASEARRAYREF) AllocateSzArray(thEHClauseArray, cEh);

            SetObjectReference((OBJECTREF*)&gc.MethodBodyObj->_exceptionClauses, gc.TempArray);

            for (INT32 i = 0; i < cEh; i++)
            {
                COR_ILMETHOD_SECT_EH_CLAUSE_FAT ehBuff;
                const COR_ILMETHOD_SECT_EH_CLAUSE_FAT* ehClause =
                    (const COR_ILMETHOD_SECT_EH_CLAUSE_FAT*)ehInfo->EHClause(i, &ehBuff);

                gc.EHClauseObj = (RUNTIMEEXCEPTIONHANDLINGCLAUSEREF) AllocateObject(pExceptionHandlingClauseMT);

                gc.EHClauseObj->_flags = ehClause->GetFlags();
                gc.EHClauseObj->_tryOffset = ehClause->GetTryOffset();
                gc.EHClauseObj->_tryLength = ehClause->GetTryLength();
                gc.EHClauseObj->_handlerOffset = ehClause->GetHandlerOffset();
                gc.EHClauseObj->_handlerLength = ehClause->GetHandlerLength();

                if ((ehClause->GetFlags() & COR_ILEXCEPTION_CLAUSE_FILTER) == 0)
                    gc.EHClauseObj->_catchToken = ehClause->GetClassToken();
                else
                    gc.EHClauseObj->_filterOffset = ehClause->GetFilterOffset();

                gc.MethodBodyObj->_exceptionClauses->SetAt(i, (OBJECTREF) gc.EHClauseObj);
                SetObjectReference((OBJECTREF*)&(gc.EHClauseObj->_methodBody), (OBJECTREF)gc.MethodBodyObj);
            }

            if (header.LocalVarSig != NULL)
            {
                SigTypeContext sigTypeContext(pMethod, declaringType, pMethod->LoadMethodInstantiation());
                MetaSig metaSig(header.LocalVarSig,
                                header.cbLocalVarSig,
                                pModule,
                                &sigTypeContext,
                                MetaSig::sigLocalVars);
                INT32 cLocals = metaSig.NumFixedArgs();
                gc.TempArray  = (BASEARRAYREF) AllocateSzArray(thLocalVariableArray, cLocals);
                SetObjectReference((OBJECTREF*)&gc.MethodBodyObj->_localVariables, gc.TempArray);

                for (INT32 i = 0; i < cLocals; i ++)
                {
                    gc.RuntimeLocalVariableInfoObj = (RUNTIMELOCALVARIABLEINFOREF)AllocateObject(pLocalVariableMT);

                    gc.RuntimeLocalVariableInfoObj->_localIndex = i;

                    metaSig.NextArg();

                    CorElementType eType;
                    IfFailThrow(metaSig.GetArgProps().PeekElemType(&eType));
                    if (ELEMENT_TYPE_PINNED == eType)
                        gc.RuntimeLocalVariableInfoObj->_isPinned = TRUE;

                    TypeHandle  tempType= metaSig.GetArgProps().GetTypeHandleThrowing(pModule, &sigTypeContext);
                    OBJECTREF refLocalType = tempType.GetManagedClassObject();
                    gc.RuntimeLocalVariableInfoObj->SetType(refLocalType);
                    gc.MethodBodyObj->_localVariables->SetAt(i, (OBJECTREF) gc.RuntimeLocalVariableInfoObj);
                }
            }
            else
            {
                INT32 cLocals = 0;
                gc.TempArray  = (BASEARRAYREF) AllocateSzArray(thLocalVariableArray, cLocals);
                SetObjectReference((OBJECTREF*)&gc.MethodBodyObj->_localVariables, gc.TempArray);
            }
        }
    }
    HELPER_METHOD_FRAME_END();

    return (RuntimeMethodBody*)OBJECTREFToObject(gc.MethodBodyObj);
}
FCIMPLEND

FCIMPL1(FC_BOOL_RET, RuntimeMethodHandle::IsConstructor, MethodDesc *pMethod)
{
    CONTRACTL {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pMethod));
    }
    CONTRACTL_END;

    BOOL ret = (BOOL)pMethod->IsClassConstructorOrCtor();
    FC_RETURN_BOOL(ret);
}
FCIMPLEND

FCIMPL1(Object*, RuntimeMethodHandle::GetLoaderAllocator, MethodDesc *pMethod)
{
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    OBJECTREF loaderAllocator = NULL;

    if (!pMethod)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    HELPER_METHOD_FRAME_BEGIN_RET_PROTECT(loaderAllocator);

    LoaderAllocator *pLoaderAllocator = pMethod->GetLoaderAllocator();
    loaderAllocator = pLoaderAllocator->GetExposedObject();

    HELPER_METHOD_FRAME_END();

    return OBJECTREFToObject(loaderAllocator);
}
FCIMPLEND

//*********************************************************************************************
//*********************************************************************************************
//*********************************************************************************************

FCIMPL1(LPCUTF8, RuntimeFieldHandle::GetUtf8Name, FieldDesc *pField)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(pField != NULL);
    }
    CONTRACTL_END;

    LPCUTF8 name;
    if (FAILED(pField->GetName_NoThrow(&name)))
        name = NULL;

    return name;
}
FCIMPLEND

FCIMPL1(INT32, RuntimeFieldHandle::GetAttributes, FieldDesc *pField)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(pField != NULL);
    }
    CONTRACTL_END;

    return (INT32)pField->GetAttributes();
}
FCIMPLEND

FCIMPL1(MethodTable*, RuntimeFieldHandle::GetApproxDeclaringMethodTable, FieldDesc *pField)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(pField != NULL);
    }
    CONTRACTL_END;

    return pField->GetApproxEnclosingMethodTable();
}
FCIMPLEND

FCIMPL1(INT32, RuntimeFieldHandle::GetToken, FieldDesc* pField)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(pField != NULL);
    }
    CONTRACTL_END;

    INT32 tkFieldDef = (INT32)pField->GetMemberDef();
    _ASSERTE(!IsNilToken(tkFieldDef) || tkFieldDef == mdFieldDefNil);
    return tkFieldDef;
}
FCIMPLEND

FCIMPL2(FieldDesc*, RuntimeFieldHandle::GetStaticFieldForGenericType, FieldDesc *pField, ReflectClassBaseObject *pDeclaringTypeUNSAFE)
{
    CONTRACTL {
        FCALL_CHECK;
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refDeclaringType = (REFLECTCLASSBASEREF)ObjectToOBJECTREF(pDeclaringTypeUNSAFE);

    if ((refDeclaringType == NULL) || (pField == NULL))
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    TypeHandle declaringType = refDeclaringType->GetType();

    if (!pField)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));
    if (declaringType.IsTypeDesc() || declaringType.IsArray())
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));
    MethodTable *pMT = declaringType.AsMethodTable();

    _ASSERTE(pField->IsStatic());
    if (pMT->HasGenericsStaticsInfo())
        pField = pMT->GetFieldDescByIndex(pField->GetApproxEnclosingMethodTable()->GetIndexForFieldDesc(pField));
    _ASSERTE(!pField->IsSharedByGenericInstantiations());
    _ASSERTE(pField->GetEnclosingMethodTable() == pMT);

    return pField;
}
FCIMPLEND

FCIMPL1(ReflectModuleBaseObject*, AssemblyHandle::GetManifestModule, AssemblyBaseObject* pAssemblyUNSAFE)
{
    FCALL_CONTRACT;

    ASSEMBLYREF refAssembly = (ASSEMBLYREF)ObjectToOBJECTREF(pAssemblyUNSAFE);
    if (refAssembly == NULL)
        return NULL;

    Module* pModule = refAssembly->GetAssembly()->GetModule();
    OBJECTREF refModule = pModule->GetExposedObjectIfExists();
    return (ReflectModuleBaseObject*)OBJECTREFToObject(refModule);
}
FCIMPLEND

FCIMPL1(INT32, AssemblyHandle::GetToken, AssemblyBaseObject* pAssemblyUNSAFE) {
    FCALL_CONTRACT;

    ASSEMBLYREF refAssembly = (ASSEMBLYREF)ObjectToOBJECTREF(pAssemblyUNSAFE);

    if (refAssembly == NULL)
        FCThrowRes(kArgumentNullException, W("Arg_InvalidHandle"));

    mdAssembly token = mdAssemblyNil;
    IMDInternalImport *mdImport = refAssembly->GetAssembly()->GetMDImport();

    if (mdImport != 0)
    {
        if (FAILED(mdImport->GetAssemblyFromScope(&token)))
        {
            FCThrow(kBadImageFormatException);
        }
    }

    return token;
}
FCIMPLEND

extern "C" void QCALLTYPE AssemblyHandle_GetManifestModuleSlow(QCall::ObjectHandleOnStack assembly, QCall::ObjectHandleOnStack module)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;
    GCX_COOP();

    if (assembly.Get() == NULL)
        COMPlusThrow(kArgumentNullException, W("Arg_InvalidHandle"));

    Module* pModule = ((ASSEMBLYREF)assembly.Get())->GetAssembly()->GetModule();
    module.Set(pModule->GetExposedObject());
    END_QCALL;
}

extern "C" void QCALLTYPE ModuleHandle_GetPEKind(QCall::ModuleHandle pModule, DWORD* pdwPEKind, DWORD* pdwMachine)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;
    pModule->GetPEAssembly()->GetPEKindAndMachine(pdwPEKind, pdwMachine);
    END_QCALL;
}

extern "C" INT32 QCALLTYPE ModuleHandle_GetMDStreamVersion(QCall::ModuleHandle pModule)
{
    QCALL_CONTRACT_NO_GC_TRANSITION;

    return pModule->GetMDImport()->GetMetadataStreamVersion();
}

extern "C" void QCALLTYPE ModuleHandle_GetModuleType(QCall::ModuleHandle pModule, QCall::ObjectHandleOnStack retType)
{
    QCALL_CONTRACT;

    TypeHandle globalTypeHandle = TypeHandle();

    BEGIN_QCALL;

        EX_TRY
        {
            globalTypeHandle = TypeHandle(pModule->GetGlobalMethodTable());
        }
        EX_SWALLOW_NONTRANSIENT;

        if (!globalTypeHandle.IsNull())
        {
            GCX_COOP();
            retType.Set(globalTypeHandle.GetManagedClassObject());
        }

    END_QCALL;

    return;
}

extern "C" INT32 QCALLTYPE ModuleHandle_GetToken(QCall::ModuleHandle pModule)
{
    QCALL_CONTRACT_NO_GC_TRANSITION;

    return pModule->GetMDImport()->GetModuleFromScope();
}

extern "C" void QCALLTYPE ModuleHandle_ResolveType(QCall::ModuleHandle pModule, INT32 tkType, TypeHandle *typeArgs, INT32 typeArgsCount, TypeHandle *methodArgs, INT32 methodArgsCount, QCall::ObjectHandleOnStack retType)
{
    QCALL_CONTRACT;

    TypeHandle typeHandle;

    BEGIN_QCALL;

    SigTypeContext typeContext(Instantiation(typeArgs, typeArgsCount), Instantiation(methodArgs, methodArgsCount));
        typeHandle = ClassLoader::LoadTypeDefOrRefOrSpecThrowing(pModule, tkType, &typeContext,
                                                          ClassLoader::ThrowIfNotFound,
                                                          ClassLoader::PermitUninstDefOrRef);

    GCX_COOP();
    retType.Set(typeHandle.GetManagedClassObject());

    END_QCALL;

    return;
}

extern "C" MethodDesc *QCALLTYPE ModuleHandle_ResolveMethod(QCall::ModuleHandle pModule, INT32 tkMemberRef, TypeHandle *typeArgs, INT32 typeArgsCount, TypeHandle *methodArgs, INT32 methodArgsCount)
{
    QCALL_CONTRACT;

    MethodDesc* pMD = NULL;

    BEGIN_QCALL;

    BOOL strictMetadataChecks = (TypeFromToken(tkMemberRef) == mdtMethodSpec);

    SigTypeContext typeContext(Instantiation(typeArgs, typeArgsCount), Instantiation(methodArgs, methodArgsCount));
    pMD = MemberLoader::GetMethodDescFromMemberDefOrRefOrSpec(pModule, tkMemberRef, &typeContext, strictMetadataChecks, FALSE);

    // This will get us the instantiating or unboxing stub if needed
    pMD = MethodDesc::FindOrCreateAssociatedMethodDescForReflection(pMD, pMD->GetMethodTable(), pMD->GetMethodInstantiation());

    END_QCALL;

    return pMD;
}

extern "C" void QCALLTYPE ModuleHandle_ResolveField(QCall::ModuleHandle pModule, INT32 tkMemberRef, TypeHandle *typeArgs, INT32 typeArgsCount, TypeHandle *methodArgs, INT32 methodArgsCount, QCall::ObjectHandleOnStack retField)
{
    QCALL_CONTRACT;

    FieldDesc* pField = NULL;

    BEGIN_QCALL;

    SigTypeContext typeContext(Instantiation(typeArgs, typeArgsCount), Instantiation(methodArgs, methodArgsCount));
    pField = MemberLoader::GetFieldDescFromMemberDefOrRef(pModule, tkMemberRef, &typeContext, FALSE);
    GCX_COOP();
    retField.Set(pField->GetStubFieldInfo());

    END_QCALL;

    return;
}

extern "C" void QCALLTYPE ModuleHandle_GetDynamicMethod(QCall::ModuleHandle pModule, const char* name, byte* sig, INT32 sigLen, QCall::ObjectHandleOnStack resolver, QCall::ObjectHandleOnStack result)
{
    CONTRACTL
    {
        QCALL_CHECK;
        PRECONDITION(CheckPointer(name));
        PRECONDITION(CheckPointer(sig));
    }
    CONTRACTL_END;

    BEGIN_QCALL;

    // Make a copy of the name
    size_t nameLen = strlen(name) + 1;
    NewArrayHolder<char> pName(new char[nameLen]);
    memcpy(pName, name, nameLen * sizeof(char));

    // Make a copy of the signature
    NewArrayHolder<BYTE> pSig(new BYTE[sigLen]);
    memcpy(pSig, sig, sigLen);

    DynamicMethodTable *pMTForDynamicMethods = pModule->GetDynamicMethodTable();
    DynamicMethodDesc* pNewMD = pMTForDynamicMethods->GetDynamicMethod(pSig, sigLen, pName);
    _ASSERTE(pNewMD != NULL);
    // pNewMD now owns pSig and pName.
    pSig.SuppressRelease();
    pName.SuppressRelease();

    {
        GCX_COOP();
        // create a handle to hold the resolver objectref
        OBJECTHANDLE resolverHandle = AppDomain::GetCurrentDomain()->CreateLongWeakHandle(resolver.Get());
        pNewMD->GetLCGMethodResolver()->SetManagedResolver(resolverHandle);
        result.Set(pNewMD->GetStubMethodInfo());
    }

    LoaderAllocator *pLoaderAllocator = pModule->GetLoaderAllocator();
    if (pLoaderAllocator->IsCollectible())
        pLoaderAllocator->AddReference();

    END_QCALL;
}
