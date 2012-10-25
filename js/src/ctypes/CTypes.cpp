/* -*-  Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2; -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/FloatingPoint.h"

#include "CTypes.h"
#include "Library.h"
#include "jsnum.h"
#include "jscompartment.h"
#include "jsobjinlines.h"
#include <limits>

#include <math.h>
#if defined(XP_WIN) || defined(XP_OS2)
#include <float.h>
#endif

#if defined(SOLARIS)
#include <ieeefp.h>
#endif

#ifdef HAVE_SSIZE_T
#include <sys/types.h>
#endif

#if defined(XP_UNIX)
#include <errno.h>
#elif defined(XP_WIN)
#include <windows.h>
#endif

#include "mozilla/StandardInteger.h"
#include "mozilla/Scoped.h"

using namespace std;

namespace js {
namespace ctypes {

/*******************************************************************************
** JSAPI function prototypes
*******************************************************************************/

static JSBool ConstructAbstract(JSContext* cx, unsigned argc, jsval* vp);

namespace CType {
  static JSBool ConstructData(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool ConstructBasic(JSContext* cx, JSHandleObject obj, unsigned argc, jsval* vp);

  static void Trace(JSTracer* trc, JSObject* obj);
  static void Finalize(JSFreeOp *fop, JSObject* obj);
  static void FinalizeProtoClass(JSFreeOp *fop, JSObject* obj);

  static JSBool PrototypeGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
    MutableHandleValue vp);
  static JSBool NameGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
    MutableHandleValue vp);
  static JSBool SizeGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
    MutableHandleValue vp);
  static JSBool PtrGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp);
  static JSBool CreateArray(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool ToString(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool ToSource(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool HasInstance(JSContext* cx, JSHandleObject obj, const jsval* v, JSBool* bp);


  /*
   * Get the global "ctypes" object.
   *
   * |obj| must be a CType object.
   *
   * This function never returns NULL.
   */
  static JSObject* GetGlobalCTypes(JSContext* cx, JSObject* obj);

}

namespace ABI {
  bool IsABI(JSObject* obj);
  static JSBool ToSource(JSContext* cx, unsigned argc, jsval* vp);
}

namespace PointerType {
  static JSBool Create(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool ConstructData(JSContext* cx, JSHandleObject obj, unsigned argc, jsval* vp);

  static JSBool TargetTypeGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
    MutableHandleValue vp);
  static JSBool ContentsGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
    MutableHandleValue vp);
  static JSBool ContentsSetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, JSBool strict,
    MutableHandleValue vp);
  static JSBool IsNull(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool Increment(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool Decrement(JSContext* cx, unsigned argc, jsval* vp);
  // The following is not an instance function, since we don't want to expose arbitrary
  // pointer arithmetic at this moment.
  static JSBool OffsetBy(JSContext* cx, int offset, jsval* vp);
}

namespace ArrayType {
  static JSBool Create(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool ConstructData(JSContext* cx, JSHandleObject obj, unsigned argc, jsval* vp);

  static JSBool ElementTypeGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
    MutableHandleValue vp);
  static JSBool LengthGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
    MutableHandleValue vp);
  static JSBool Getter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp);
  static JSBool Setter(JSContext* cx, JSHandleObject obj, JSHandleId idval, JSBool strict, MutableHandleValue vp);
  static JSBool AddressOfElement(JSContext* cx, unsigned argc, jsval* vp);
}

namespace StructType {
  static JSBool Create(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool ConstructData(JSContext* cx, JSHandleObject obj, unsigned argc, jsval* vp);

  static JSBool FieldsArrayGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
    MutableHandleValue vp);
  static JSBool FieldGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
    MutableHandleValue vp);
  static JSBool FieldSetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, JSBool strict,
                            MutableHandleValue vp);
  static JSBool AddressOfField(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool Define(JSContext* cx, unsigned argc, jsval* vp);
}

namespace FunctionType {
  static JSBool Create(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool ConstructData(JSContext* cx, JSHandleObject typeObj,
    JSHandleObject dataObj, JSHandleObject fnObj, JSHandleObject thisObj, jsval errVal);

  static JSBool Call(JSContext* cx, unsigned argc, jsval* vp);

  static JSBool ArgTypesGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
    MutableHandleValue vp);
  static JSBool ReturnTypeGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
    MutableHandleValue vp);
  static JSBool ABIGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp);
  static JSBool IsVariadicGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
    MutableHandleValue vp);
}

namespace CClosure {
  static void Trace(JSTracer* trc, JSObject* obj);
  static void Finalize(JSFreeOp *fop, JSObject* obj);

  // libffi callback
  static void ClosureStub(ffi_cif* cif, void* result, void** args,
    void* userData);
}

namespace CData {
  static void Finalize(JSFreeOp *fop, JSObject* obj);

  static JSBool ValueGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
                            MutableHandleValue vp);
  static JSBool ValueSetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
                            JSBool strict, MutableHandleValue vp);
  static JSBool Address(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool ReadString(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool ToSource(JSContext* cx, unsigned argc, jsval* vp);
  static JSString *GetSourceString(JSContext *cx, JSHandleObject typeObj,
                                   void *data);
  static JSBool ErrnoGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
                            MutableHandleValue vp);

#if defined(XP_WIN)
  static JSBool LastErrorGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval,
                                MutableHandleValue vp);
#endif // defined(XP_WIN)
}

namespace CDataFinalizer {
  /*
   * Attach a C function as a finalizer to a JS object.
   *
   * This function is available from JS as |ctypes.withFinalizer|.
   *
   * JavaScript signature:
   * function(CData, CData):   CDataFinalizer
   *          value  finalizer finalizable
   *
   * Where |finalizer| is a one-argument function taking a value
   * with the same type as |value|.
   */
  static JSBool Construct(JSContext* cx, unsigned argc, jsval *vp);

  /*
   * Private data held by |CDataFinalizer|.
   *
   * See also |enum CDataFinalizerSlot| for the slots of
   * |CDataFinalizer|.
   *
   * Note: the private data may be NULL, if |dispose|, |forget| or the
   * finalizer has already been called.
   */
  struct Private {
    /*
     * The C data to pass to the code.
     * Finalization/|dispose|/|forget| release this memory.
     */
    void *cargs;

    /*
     * The total size of the buffer pointed by |cargs|
     */
    size_t cargs_size;

    /*
     * Low-level signature information.
     * Finalization/|dispose|/|forget| release this memory.
     */
    ffi_cif CIF;

    /*
     * The C function to invoke during finalization.
     * Do not deallocate this.
     */
    uintptr_t code;

    /*
     * A buffer for holding the return value.
     * Finalization/|dispose|/|forget| release this memory.
     */
    void *rvalue;
  };

  /*
   * Methods of instances of |CDataFinalizer|
   */
  namespace Methods {
    static JSBool Dispose(JSContext* cx, unsigned argc, jsval *vp);
    static JSBool Forget(JSContext* cx, unsigned argc, jsval *vp);
    static JSBool ToSource(JSContext* cx, unsigned argc, jsval *vp);
    static JSBool ToString(JSContext* cx, unsigned argc, jsval *vp);
  }

  /*
   * Utility functions
   *
   * @return true if |obj| is a CDataFinalizer, false otherwise.
   */
  static bool IsCDataFinalizer(JSObject *obj);

  /*
   * Clean up the finalization information of a CDataFinalizer.
   *
   * Used by |Finalize|, |Dispose| and |Forget|.
   *
   * @param p The private information of the CDataFinalizer. If NULL,
   * this function does nothing.
   * @param obj Either NULL, if the object should not be cleaned up (i.e.
   * during finalization) or a CDataFinalizer JSObject. Always use NULL
   * if you are calling from a finalizer.
   */
  static void Cleanup(Private *p, JSObject *obj);

  /*
   * Perform the actual call to the finalizer code.
   */
  static void CallFinalizer(CDataFinalizer::Private *p,
                            int* errnoStatus,
                            int32_t* lastErrorStatus);

  /*
   * Return the CType of a CDataFinalizer object, or NULL if the object
   * has been cleaned-up already.
   */
  static JSObject *GetCType(JSContext *cx, JSObject *obj);

  /*
   * Perform finalization of a |CDataFinalizer|
   */
  static void Finalize(JSFreeOp *fop, JSObject *obj);

  /*
   * Return the jsval contained by this finalizer.
   *
   * Note that the jsval is actually not recorded, but converted back from C.
   */
  static bool GetValue(JSContext *cx, JSObject *obj, jsval *result);

  static JSObject* GetCData(JSContext *cx, JSObject *obj);
 }


// Int64Base provides functions common to Int64 and UInt64.
namespace Int64Base {
  JSObject* Construct(JSContext* cx, HandleObject proto, uint64_t data,
    bool isUnsigned);

  uint64_t GetInt(JSObject* obj);

  JSBool ToString(JSContext* cx, JSObject* obj, unsigned argc, jsval* vp,
    bool isUnsigned);

  JSBool ToSource(JSContext* cx, JSObject* obj, unsigned argc, jsval* vp,
    bool isUnsigned);

  static void Finalize(JSFreeOp *fop, JSObject* obj);
}

namespace Int64 {
  static JSBool Construct(JSContext* cx, unsigned argc, jsval* vp);

  static JSBool ToString(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool ToSource(JSContext* cx, unsigned argc, jsval* vp);

  static JSBool Compare(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool Lo(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool Hi(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool Join(JSContext* cx, unsigned argc, jsval* vp);
}

namespace UInt64 {
  static JSBool Construct(JSContext* cx, unsigned argc, jsval* vp);

  static JSBool ToString(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool ToSource(JSContext* cx, unsigned argc, jsval* vp);

  static JSBool Compare(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool Lo(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool Hi(JSContext* cx, unsigned argc, jsval* vp);
  static JSBool Join(JSContext* cx, unsigned argc, jsval* vp);
}

/*******************************************************************************
** JSClass definitions and initialization functions
*******************************************************************************/

// Class representing the 'ctypes' object itself. This exists to contain the
// JSCTypesCallbacks set of function pointers.
static JSClass sCTypesGlobalClass = {
  "ctypes",
  JSCLASS_HAS_RESERVED_SLOTS(CTYPESGLOBAL_SLOTS),
  JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub
};

static JSClass sCABIClass = {
  "CABI",
  JSCLASS_HAS_RESERVED_SLOTS(CABI_SLOTS),
  JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub
};

// Class representing ctypes.{C,Pointer,Array,Struct,Function}Type.prototype.
// This exists to give said prototypes a class of "CType", and to provide
// reserved slots for stashing various other prototype objects.
static JSClass sCTypeProtoClass = {
  "CType",
  JSCLASS_HAS_RESERVED_SLOTS(CTYPEPROTO_SLOTS),
  JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, CType::FinalizeProtoClass,
  NULL, ConstructAbstract, NULL, ConstructAbstract
};

// Class representing ctypes.CData.prototype and the 'prototype' properties
// of CTypes. This exists to give said prototypes a class of "CData".
static JSClass sCDataProtoClass = {
  "CData",
  0,
  JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub
};

static JSClass sCTypeClass = {
  "CType",
  JSCLASS_IMPLEMENTS_BARRIERS | JSCLASS_HAS_RESERVED_SLOTS(CTYPE_SLOTS),
  JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, CType::Finalize,
  NULL, CType::ConstructData, CType::HasInstance, CType::ConstructData,
  CType::Trace
};

static JSClass sCDataClass = {
  "CData",
  JSCLASS_HAS_RESERVED_SLOTS(CDATA_SLOTS),
  JS_PropertyStub, JS_PropertyStub, ArrayType::Getter, ArrayType::Setter,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, CData::Finalize,
  NULL, FunctionType::Call, NULL, FunctionType::Call
};

static JSClass sCClosureClass = {
  "CClosure",
  JSCLASS_IMPLEMENTS_BARRIERS | JSCLASS_HAS_RESERVED_SLOTS(CCLOSURE_SLOTS),
  JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, CClosure::Finalize,
  NULL, NULL, NULL, NULL, CClosure::Trace
};

/*
 * Class representing the prototype of CDataFinalizer.
 */
static JSClass sCDataFinalizerProtoClass = {
  "CDataFinalizer",
  0,
  JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub
};

/*
 * Class representing instances of CDataFinalizer.
 *
 * Instances of CDataFinalizer have both private data (with type
 * |CDataFinalizer::Private|) and slots (see |CDataFinalizerSlots|).
 */
static JSClass sCDataFinalizerClass = {
  "CDataFinalizer",
  JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(CDATAFINALIZER_SLOTS),
  JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, CDataFinalizer::Finalize,
};


#define CTYPESFN_FLAGS \
  (JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT)

#define CTYPESCTOR_FLAGS \
  (CTYPESFN_FLAGS | JSFUN_CONSTRUCTOR)

#define CTYPESPROP_FLAGS \
  (JSPROP_SHARED | JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT)

#define CABIFN_FLAGS \
  (JSPROP_READONLY | JSPROP_PERMANENT)

#define CDATAFN_FLAGS \
  (JSPROP_READONLY | JSPROP_PERMANENT)

#define CDATAFINALIZERFN_FLAGS \
  (JSPROP_READONLY | JSPROP_PERMANENT)

static JSPropertySpec sCTypeProps[] = {
  { "name", 0, CTYPESPROP_FLAGS, JSOP_WRAPPER(CType::NameGetter), JSOP_NULLWRAPPER },
  { "size", 0, CTYPESPROP_FLAGS, JSOP_WRAPPER(CType::SizeGetter), JSOP_NULLWRAPPER },
  { "ptr", 0, CTYPESPROP_FLAGS, JSOP_WRAPPER(CType::PtrGetter), JSOP_NULLWRAPPER },
  { "prototype", 0, CTYPESPROP_FLAGS, JSOP_WRAPPER(CType::PrototypeGetter), JSOP_NULLWRAPPER },
  { 0, 0, 0, JSOP_NULLWRAPPER, JSOP_NULLWRAPPER }
};

static JSFunctionSpec sCTypeFunctions[] = {
  JS_FN("array", CType::CreateArray, 0, CTYPESFN_FLAGS),
  JS_FN("toString", CType::ToString, 0, CTYPESFN_FLAGS),
  JS_FN("toSource", CType::ToSource, 0, CTYPESFN_FLAGS),
  JS_FS_END
};

static JSFunctionSpec sCABIFunctions[] = {
  JS_FN("toSource", ABI::ToSource, 0, CABIFN_FLAGS),
  JS_FN("toString", ABI::ToSource, 0, CABIFN_FLAGS),
  JS_FS_END
};

static JSPropertySpec sCDataProps[] = {
  { "value", 0, JSPROP_SHARED | JSPROP_PERMANENT,
    JSOP_WRAPPER(CData::ValueGetter), JSOP_WRAPPER(CData::ValueSetter) },
  { 0, 0, 0, JSOP_NULLWRAPPER, JSOP_NULLWRAPPER }
};

static JSFunctionSpec sCDataFunctions[] = {
  JS_FN("address", CData::Address, 0, CDATAFN_FLAGS),
  JS_FN("readString", CData::ReadString, 0, CDATAFN_FLAGS),
  JS_FN("toSource", CData::ToSource, 0, CDATAFN_FLAGS),
  JS_FN("toString", CData::ToSource, 0, CDATAFN_FLAGS),
  JS_FS_END
};

static JSPropertySpec sCDataFinalizerProps[] = {
  { 0, 0, 0, JSOP_NULLWRAPPER, JSOP_NULLWRAPPER }
};

static JSFunctionSpec sCDataFinalizerFunctions[] = {
  JS_FN("dispose",  CDataFinalizer::Methods::Dispose,  0, CDATAFINALIZERFN_FLAGS),
  JS_FN("forget",   CDataFinalizer::Methods::Forget,   0, CDATAFINALIZERFN_FLAGS),
  JS_FN("readString",CData::ReadString, 0, CDATAFINALIZERFN_FLAGS),
  JS_FN("toString", CDataFinalizer::Methods::ToString, 0, CDATAFINALIZERFN_FLAGS),
  JS_FN("toSource", CDataFinalizer::Methods::ToSource, 0, CDATAFINALIZERFN_FLAGS),
  JS_FS_END
};

static JSFunctionSpec sPointerFunction =
  JS_FN("PointerType", PointerType::Create, 1, CTYPESCTOR_FLAGS);

static JSPropertySpec sPointerProps[] = {
  { "targetType", 0, CTYPESPROP_FLAGS,
    JSOP_WRAPPER(PointerType::TargetTypeGetter), JSOP_NULLWRAPPER },
  { 0, 0, 0, JSOP_NULLWRAPPER, JSOP_NULLWRAPPER }
};

static JSFunctionSpec sPointerInstanceFunctions[] = {
  JS_FN("isNull", PointerType::IsNull, 0, CTYPESFN_FLAGS),
  JS_FN("increment", PointerType::Increment, 0, CTYPESFN_FLAGS),
  JS_FN("decrement", PointerType::Decrement, 0, CTYPESFN_FLAGS),
  JS_FS_END
};

static JSPropertySpec sPointerInstanceProps[] = {
  { "contents", 0, JSPROP_SHARED | JSPROP_PERMANENT,
    JSOP_WRAPPER(PointerType::ContentsGetter),
    JSOP_WRAPPER(PointerType::ContentsSetter) },
  { 0, 0, 0, JSOP_NULLWRAPPER, JSOP_NULLWRAPPER }
};

static JSFunctionSpec sArrayFunction =
  JS_FN("ArrayType", ArrayType::Create, 1, CTYPESCTOR_FLAGS);

static JSPropertySpec sArrayProps[] = {
  { "elementType", 0, CTYPESPROP_FLAGS,
    JSOP_WRAPPER(ArrayType::ElementTypeGetter), JSOP_NULLWRAPPER },
  { "length", 0, CTYPESPROP_FLAGS,
    JSOP_WRAPPER(ArrayType::LengthGetter), JSOP_NULLWRAPPER },
  { 0, 0, 0, JSOP_NULLWRAPPER, JSOP_NULLWRAPPER }
};

static JSFunctionSpec sArrayInstanceFunctions[] = {
  JS_FN("addressOfElement", ArrayType::AddressOfElement, 1, CDATAFN_FLAGS),
  JS_FS_END
};

static JSPropertySpec sArrayInstanceProps[] = {
  { "length", 0, JSPROP_SHARED | JSPROP_READONLY | JSPROP_PERMANENT,
    JSOP_WRAPPER(ArrayType::LengthGetter), JSOP_NULLWRAPPER },
  { 0, 0, 0, JSOP_NULLWRAPPER, JSOP_NULLWRAPPER }
};

static JSFunctionSpec sStructFunction =
  JS_FN("StructType", StructType::Create, 2, CTYPESCTOR_FLAGS);

static JSPropertySpec sStructProps[] = {
  { "fields", 0, CTYPESPROP_FLAGS,
    JSOP_WRAPPER(StructType::FieldsArrayGetter), JSOP_NULLWRAPPER },
  { 0, 0, 0, JSOP_NULLWRAPPER, JSOP_NULLWRAPPER }
};

static JSFunctionSpec sStructFunctions[] = {
  JS_FN("define", StructType::Define, 1, CDATAFN_FLAGS),
  JS_FS_END
};

static JSFunctionSpec sStructInstanceFunctions[] = {
  JS_FN("addressOfField", StructType::AddressOfField, 1, CDATAFN_FLAGS),
  JS_FS_END
};

static JSFunctionSpec sFunctionFunction =
  JS_FN("FunctionType", FunctionType::Create, 2, CTYPESCTOR_FLAGS);

static JSPropertySpec sFunctionProps[] = {
  { "argTypes", 0, CTYPESPROP_FLAGS,
    JSOP_WRAPPER(FunctionType::ArgTypesGetter), JSOP_NULLWRAPPER },
  { "returnType", 0, CTYPESPROP_FLAGS,
    JSOP_WRAPPER(FunctionType::ReturnTypeGetter), JSOP_NULLWRAPPER },
  { "abi", 0, CTYPESPROP_FLAGS,
    JSOP_WRAPPER(FunctionType::ABIGetter), JSOP_NULLWRAPPER },
  { "isVariadic", 0, CTYPESPROP_FLAGS,
    JSOP_WRAPPER(FunctionType::IsVariadicGetter), JSOP_NULLWRAPPER },
  { 0, 0, 0, JSOP_NULLWRAPPER, JSOP_NULLWRAPPER }
};

static JSFunctionSpec sFunctionInstanceFunctions[] = {
  JS_FN("call", js_fun_call, 1, CDATAFN_FLAGS),
  JS_FN("apply", js_fun_apply, 2, CDATAFN_FLAGS),
  JS_FS_END
};

static JSClass sInt64ProtoClass = {
  "Int64",
  0,
  JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub
};

static JSClass sUInt64ProtoClass = {
  "UInt64",
  0,
  JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub
};

static JSClass sInt64Class = {
  "Int64",
  JSCLASS_HAS_RESERVED_SLOTS(INT64_SLOTS),
  JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, Int64Base::Finalize
};

static JSClass sUInt64Class = {
  "UInt64",
  JSCLASS_HAS_RESERVED_SLOTS(INT64_SLOTS),
  JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, Int64Base::Finalize
};

static JSFunctionSpec sInt64StaticFunctions[] = {
  JS_FN("compare", Int64::Compare, 2, CTYPESFN_FLAGS),
  JS_FN("lo", Int64::Lo, 1, CTYPESFN_FLAGS),
  JS_FN("hi", Int64::Hi, 1, CTYPESFN_FLAGS),
  JS_FN("join", Int64::Join, 2, CTYPESFN_FLAGS),
  JS_FS_END
};

static JSFunctionSpec sUInt64StaticFunctions[] = {
  JS_FN("compare", UInt64::Compare, 2, CTYPESFN_FLAGS),
  JS_FN("lo", UInt64::Lo, 1, CTYPESFN_FLAGS),
  JS_FN("hi", UInt64::Hi, 1, CTYPESFN_FLAGS),
  JS_FN("join", UInt64::Join, 2, CTYPESFN_FLAGS),
  JS_FS_END
};

static JSFunctionSpec sInt64Functions[] = {
  JS_FN("toString", Int64::ToString, 0, CTYPESFN_FLAGS),
  JS_FN("toSource", Int64::ToSource, 0, CTYPESFN_FLAGS),
  JS_FS_END
};

static JSFunctionSpec sUInt64Functions[] = {
  JS_FN("toString", UInt64::ToString, 0, CTYPESFN_FLAGS),
  JS_FN("toSource", UInt64::ToSource, 0, CTYPESFN_FLAGS),
  JS_FS_END
};

static JSPropertySpec sModuleProps[] = {
  { "errno", 0, JSPROP_SHARED | JSPROP_PERMANENT,
    JSOP_WRAPPER(CData::ErrnoGetter), JSOP_NULLWRAPPER },
#if defined(XP_WIN)
  { "winLastError", 0, JSPROP_SHARED | JSPROP_PERMANENT,
    JSOP_WRAPPER(CData::LastErrorGetter), JSOP_NULLWRAPPER },
#endif // defined(XP_WIN)
  { 0, 0, 0, JSOP_NULLWRAPPER, JSOP_NULLWRAPPER }
};

static JSFunctionSpec sModuleFunctions[] = {
  JS_FN("CDataFinalizer", CDataFinalizer::Construct, 2, CTYPESFN_FLAGS),
  JS_FN("open", Library::Open, 1, CTYPESFN_FLAGS),
  JS_FN("cast", CData::Cast, 2, CTYPESFN_FLAGS),
  JS_FN("getRuntime", CData::GetRuntime, 1, CTYPESFN_FLAGS),
  JS_FN("libraryName", Library::Name, 1, CTYPESFN_FLAGS),
  JS_FS_END
};

JS_ALWAYS_INLINE JSString*
NewUCString(JSContext* cx, const AutoString& from)
{
  return JS_NewUCStringCopyN(cx, from.begin(), from.length());
}

/*
 * Return a size rounded up to a multiple of a power of two.
 *
 * Note: |align| must be a power of 2.
 */
JS_ALWAYS_INLINE size_t
Align(size_t val, size_t align)
{
  // Ensure that align is a power of two.
  MOZ_ASSERT(align != 0 && (align & (align - 1)) == 0);
  return ((val - 1) | (align - 1)) + 1;
}

static ABICode
GetABICode(JSObject* obj)
{
  // make sure we have an object representing a CABI class,
  // and extract the enumerated class type from the reserved slot.
  if (JS_GetClass(obj) != &sCABIClass)
    return INVALID_ABI;

  jsval result = JS_GetReservedSlot(obj, SLOT_ABICODE);
  return ABICode(JSVAL_TO_INT(result));
}

JSErrorFormatString ErrorFormatString[CTYPESERR_LIMIT] = {
#define MSG_DEF(name, number, count, exception, format) \
  { format, count, exception } ,
#include "ctypes.msg"
#undef MSG_DEF
};

const JSErrorFormatString*
GetErrorMessage(void* userRef, const char* locale, const unsigned errorNumber)
{
  if (0 < errorNumber && errorNumber < CTYPESERR_LIMIT)
    return &ErrorFormatString[errorNumber];
  return NULL;
}

JSBool
TypeError(JSContext* cx, const char* expected, jsval actual)
{
  JSString* str = JS_ValueToSource(cx, actual);
  JSAutoByteString bytes;
  
  const char* src;
  if (str) {
    src = bytes.encode(cx, str);
    if (!src)
      return false;
  } else {
    JS_ClearPendingException(cx);
    src = "<<error converting value to string>>";
  }
  JS_ReportErrorNumber(cx, GetErrorMessage, NULL,
                       CTYPESMSG_TYPE_ERROR, expected, src);
  return false;
}

static JSObject*
InitCTypeClass(JSContext* cx, HandleObject parent)
{
  JSFunction *fun = JS_DefineFunction(cx, parent, "CType", ConstructAbstract, 0,
                                      CTYPESCTOR_FLAGS);
  if (!fun)
    return NULL;

  RootedObject ctor(cx, JS_GetFunctionObject(fun));
  RootedObject fnproto(cx, JS_GetPrototype(ctor));
  JS_ASSERT(ctor);
  JS_ASSERT(fnproto);

  // Set up ctypes.CType.prototype.
  RootedObject prototype(cx, JS_NewObject(cx, &sCTypeProtoClass, fnproto, parent));
  if (!prototype)
    return NULL;

  if (!JS_DefineProperty(cx, ctor, "prototype", OBJECT_TO_JSVAL(prototype),
         NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT))
    return NULL;

  if (!JS_DefineProperty(cx, prototype, "constructor", OBJECT_TO_JSVAL(ctor),
         NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT))
    return NULL;

  // Define properties and functions common to all CTypes.
  if (!JS_DefineProperties(cx, prototype, sCTypeProps) ||
      !JS_DefineFunctions(cx, prototype, sCTypeFunctions))
    return NULL;

  if (!JS_FreezeObject(cx, ctor) || !JS_FreezeObject(cx, prototype))
    return NULL;

  return prototype;
}

static JSObject*
InitABIClass(JSContext* cx, JSObject* parent)
{
  RootedObject obj(cx, JS_NewObject(cx, NULL, NULL, NULL));
  
  if (!obj)
    return NULL;
    
  if (!JS_DefineFunctions(cx, obj, sCABIFunctions))
    return NULL;
  
  return obj;
}


static JSObject*
InitCDataClass(JSContext* cx, HandleObject parent, HandleObject CTypeProto)
{
  JSFunction* fun = JS_DefineFunction(cx, parent, "CData", ConstructAbstract, 0,
                      CTYPESCTOR_FLAGS);
  if (!fun)
    return NULL;

  RootedObject ctor(cx, JS_GetFunctionObject(fun));
  JS_ASSERT(ctor);

  // Set up ctypes.CData.__proto__ === ctypes.CType.prototype.
  // (Note that 'ctypes.CData instanceof Function' is still true, thanks to the
  // prototype chain.)
  if (!JS_SetPrototype(cx, ctor, CTypeProto))
    return NULL;

  // Set up ctypes.CData.prototype.
  RootedObject prototype(cx, JS_NewObject(cx, &sCDataProtoClass, NULL, parent));
  if (!prototype)
    return NULL;

  if (!JS_DefineProperty(cx, ctor, "prototype", OBJECT_TO_JSVAL(prototype),
         NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT))
    return NULL;

  if (!JS_DefineProperty(cx, prototype, "constructor", OBJECT_TO_JSVAL(ctor),
         NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT))
    return NULL;

  // Define properties and functions common to all CDatas.
  if (!JS_DefineProperties(cx, prototype, sCDataProps) ||
      !JS_DefineFunctions(cx, prototype, sCDataFunctions))
    return NULL;

  if (//!JS_FreezeObject(cx, prototype) || // XXX fixme - see bug 541212!
      !JS_FreezeObject(cx, ctor))
    return NULL;

  return prototype;
}

static JSBool
DefineABIConstant(JSContext* cx,
                  HandleObject parent,
                  const char* name,
                  ABICode code,
                  HandleObject prototype)
{
  RootedObject obj(cx, JS_DefineObject(cx, parent, name, &sCABIClass, prototype,
                                       JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT));
  if (!obj)
    return false;
  JS_SetReservedSlot(obj, SLOT_ABICODE, INT_TO_JSVAL(code));
  return JS_FreezeObject(cx, obj);
}

// Set up a single type constructor for
// ctypes.{Pointer,Array,Struct,Function}Type.
static JSBool
InitTypeConstructor(JSContext* cx,
                    HandleObject parent,
                    HandleObject CTypeProto,
                    HandleObject CDataProto,
                    JSFunctionSpec spec,
                    JSFunctionSpec* fns,
                    JSPropertySpec* props,
                    JSFunctionSpec* instanceFns,
                    JSPropertySpec* instanceProps,
                    MutableHandleObject typeProto,
                    MutableHandleObject dataProto)
{
  JSFunction* fun = js::DefineFunctionWithReserved(cx, parent, spec.name, spec.call.op,
                      spec.nargs, spec.flags);
  if (!fun)
    return false;

  RootedObject obj(cx, JS_GetFunctionObject(fun));
  if (!obj)
    return false;

  // Set up the .prototype and .prototype.constructor properties.
  typeProto.set(JS_NewObject(cx, &sCTypeProtoClass, CTypeProto, parent));
  if (!typeProto)
    return false;

  // Define property before proceeding, for GC safety.
  if (!JS_DefineProperty(cx, obj, "prototype", OBJECT_TO_JSVAL(typeProto),
         NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT))
    return false;

  if (fns && !JS_DefineFunctions(cx, typeProto, fns))
    return false;

  if (!JS_DefineProperties(cx, typeProto, props))
    return false;

  if (!JS_DefineProperty(cx, typeProto, "constructor", OBJECT_TO_JSVAL(obj),
         NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT))
    return false;

  // Stash ctypes.{Pointer,Array,Struct}Type.prototype on a reserved slot of
  // the type constructor, for faster lookup.
  js::SetFunctionNativeReserved(obj, SLOT_FN_CTORPROTO, OBJECT_TO_JSVAL(typeProto));

  // Create an object to serve as the common ancestor for all CData objects
  // created from the given type constructor. This has ctypes.CData.prototype
  // as its prototype, such that it inherits the properties and functions
  // common to all CDatas.
  dataProto.set(JS_NewObject(cx, &sCDataProtoClass, CDataProto, parent));
  if (!dataProto)
    return false;

  // Define functions and properties on the 'dataProto' object that are common
  // to all CData objects created from this type constructor. (These will
  // become functions and properties on CData objects created from this type.)
  if (instanceFns && !JS_DefineFunctions(cx, dataProto, instanceFns))
    return false;

  if (instanceProps && !JS_DefineProperties(cx, dataProto, instanceProps))
    return false;

  // Link the type prototype to the data prototype.
  JS_SetReservedSlot(typeProto, SLOT_OURDATAPROTO, OBJECT_TO_JSVAL(dataProto));

  if (!JS_FreezeObject(cx, obj) ||
      //!JS_FreezeObject(cx, dataProto) || // XXX fixme - see bug 541212!
      !JS_FreezeObject(cx, typeProto))
    return false;

  return true;
}

JSObject*
InitInt64Class(JSContext* cx,
               HandleObject parent,
               JSClass* clasp,
               JSNative construct,
               JSFunctionSpec* fs,
               JSFunctionSpec* static_fs)
{
  // Init type class and constructor
  RootedObject prototype(cx, JS_InitClass(cx, parent, NULL, clasp, construct,
                                          0, NULL, fs, NULL, static_fs));
  if (!prototype)
    return NULL;

  RootedObject ctor(cx, JS_GetConstructor(cx, prototype));
  if (!ctor)
    return NULL;
  if (!JS_FreezeObject(cx, ctor))
    return NULL;

  // Redefine the 'join' function as an extended native and stash
  // ctypes.{Int64,UInt64}.prototype in a reserved slot of the new function.
  JS_ASSERT(clasp == &sInt64ProtoClass || clasp == &sUInt64ProtoClass);
  JSNative native = (clasp == &sInt64ProtoClass) ? Int64::Join : UInt64::Join;
  JSFunction* fun = js::DefineFunctionWithReserved(cx, ctor, "join", native,
                      2, CTYPESFN_FLAGS);
  if (!fun)
    return NULL;

  js::SetFunctionNativeReserved(fun, SLOT_FN_INT64PROTO,
    OBJECT_TO_JSVAL(prototype));

  if (!JS_FreezeObject(cx, prototype))
    return NULL;

  return prototype;
}

static void
AttachProtos(JSObject* proto, const AutoObjectVector& protos)
{
  // For a given 'proto' of [[Class]] "CTypeProto", attach each of the 'protos'
  // to the appropriate CTypeProtoSlot. (SLOT_CTYPES is the last slot
  // of [[Class]] "CTypeProto" that we fill in this automated manner.)
  for (uint32_t i = 0; i <= SLOT_CTYPES; ++i)
    JS_SetReservedSlot(proto, i, OBJECT_TO_JSVAL(protos[i]));
}

JSBool
InitTypeClasses(JSContext* cx, HandleObject parent)
{
  // Initialize the ctypes.CType class. This acts as an abstract base class for
  // the various types, and provides the common API functions. It has:
  //   * [[Class]] "Function"
  //   * __proto__ === Function.prototype
  //   * A constructor that throws a TypeError. (You can't construct an
  //     abstract type!)
  //   * 'prototype' property:
  //     * [[Class]] "CTypeProto"
  //     * __proto__ === Function.prototype
  //     * A constructor that throws a TypeError. (You can't construct an
  //       abstract type instance!)
  //     * 'constructor' property === ctypes.CType
  //     * Provides properties and functions common to all CTypes.
  RootedObject CTypeProto(cx, InitCTypeClass(cx, parent));
  if (!CTypeProto)
    return false;

  // Initialize the ctypes.CData class. This acts as an abstract base class for
  // instances of the various types, and provides the common API functions.
  // It has:
  //   * [[Class]] "Function"
  //   * __proto__ === Function.prototype
  //   * A constructor that throws a TypeError. (You can't construct an
  //     abstract type instance!)
  //   * 'prototype' property:
  //     * [[Class]] "CDataProto"
  //     * 'constructor' property === ctypes.CData
  //     * Provides properties and functions common to all CDatas.
  RootedObject CDataProto(cx, InitCDataClass(cx, parent, CTypeProto));
  if (!CDataProto)
    return false;

  // Link CTypeProto to CDataProto.
  JS_SetReservedSlot(CTypeProto, SLOT_OURDATAPROTO, OBJECT_TO_JSVAL(CDataProto));

  // Create and attach the special class constructors: ctypes.PointerType,
  // ctypes.ArrayType, ctypes.StructType, and ctypes.FunctionType.
  // Each of these constructors 'c' has, respectively:
  //   * [[Class]] "Function"
  //   * __proto__ === Function.prototype
  //   * A constructor that creates a user-defined type.
  //   * 'prototype' property:
  //     * [[Class]] "CTypeProto"
  //     * __proto__ === ctypes.CType.prototype
  //     * 'constructor' property === 'c'
  // We also construct an object 'p' to serve, given a type object 't'
  // constructed from one of these type constructors, as
  // 't.prototype.__proto__'. This object has:
  //   * [[Class]] "CDataProto"
  //   * __proto__ === ctypes.CData.prototype
  //   * Properties and functions common to all CDatas.
  // Therefore an instance 't' of ctypes.{Pointer,Array,Struct,Function}Type
  // will have, resp.:
  //   * [[Class]] "CType"
  //   * __proto__ === ctypes.{Pointer,Array,Struct,Function}Type.prototype
  //   * A constructor which creates and returns a CData object, containing
  //     binary data of the given type.
  //   * 'prototype' property:
  //     * [[Class]] "CDataProto"
  //     * __proto__ === 'p', the prototype object from above
  //     * 'constructor' property === 't'
  AutoObjectVector protos(cx);
  protos.resize(CTYPEPROTO_SLOTS);
  if (!InitTypeConstructor(cx, parent, CTypeProto, CDataProto,
         sPointerFunction, NULL, sPointerProps,
         sPointerInstanceFunctions, sPointerInstanceProps,
         protos.handleAt(SLOT_POINTERPROTO), protos.handleAt(SLOT_POINTERDATAPROTO)))
    return false;

  if (!InitTypeConstructor(cx, parent, CTypeProto, CDataProto,
         sArrayFunction, NULL, sArrayProps,
         sArrayInstanceFunctions, sArrayInstanceProps,
         protos.handleAt(SLOT_ARRAYPROTO), protos.handleAt(SLOT_ARRAYDATAPROTO)))
    return false;

  if (!InitTypeConstructor(cx, parent, CTypeProto, CDataProto,
         sStructFunction, sStructFunctions, sStructProps,
         sStructInstanceFunctions, NULL,
         protos.handleAt(SLOT_STRUCTPROTO), protos.handleAt(SLOT_STRUCTDATAPROTO)))
    return false;

  if (!InitTypeConstructor(cx, parent, CTypeProto, protos.handleAt(SLOT_POINTERDATAPROTO),
         sFunctionFunction, NULL, sFunctionProps, sFunctionInstanceFunctions, NULL,
         protos.handleAt(SLOT_FUNCTIONPROTO), protos.handleAt(SLOT_FUNCTIONDATAPROTO)))
    return false;

  protos[SLOT_CDATAPROTO] = CDataProto;

  // Create and attach the ctypes.{Int64,UInt64} constructors.
  // Each of these has, respectively:
  //   * [[Class]] "Function"
  //   * __proto__ === Function.prototype
  //   * A constructor that creates a ctypes.{Int64,UInt64} object, respectively.
  //   * 'prototype' property:
  //     * [[Class]] {"Int64Proto","UInt64Proto"}
  //     * 'constructor' property === ctypes.{Int64,UInt64}
  protos[SLOT_INT64PROTO] = InitInt64Class(cx, parent, &sInt64ProtoClass,
    Int64::Construct, sInt64Functions, sInt64StaticFunctions);
  if (!protos[SLOT_INT64PROTO])
    return false;
  protos[SLOT_UINT64PROTO] = InitInt64Class(cx, parent, &sUInt64ProtoClass,
    UInt64::Construct, sUInt64Functions, sUInt64StaticFunctions);
  if (!protos[SLOT_UINT64PROTO])
    return false;

  // Finally, store a pointer to the global ctypes object.
  // Note that there is no other reliable manner of locating this object.
  protos[SLOT_CTYPES] = parent;

  // Attach the prototypes just created to each of ctypes.CType.prototype,
  // and the special type constructors, so we can access them when constructing
  // instances of those types. 
  AttachProtos(CTypeProto, protos);
  AttachProtos(protos[SLOT_POINTERPROTO], protos);
  AttachProtos(protos[SLOT_ARRAYPROTO], protos);
  AttachProtos(protos[SLOT_STRUCTPROTO], protos);
  AttachProtos(protos[SLOT_FUNCTIONPROTO], protos);

  RootedObject ABIProto(cx, InitABIClass(cx, parent));
  if (!ABIProto)
    return false;

  // Attach objects representing ABI constants.
  if (!DefineABIConstant(cx, parent, "default_abi", ABI_DEFAULT, ABIProto) ||
      !DefineABIConstant(cx, parent, "stdcall_abi", ABI_STDCALL, ABIProto) ||
      !DefineABIConstant(cx, parent, "winapi_abi", ABI_WINAPI, ABIProto))
    return false;

  // Create objects representing the builtin types, and attach them to the
  // ctypes object. Each type object 't' has:
  //   * [[Class]] "CType"
  //   * __proto__ === ctypes.CType.prototype
  //   * A constructor which creates and returns a CData object, containing
  //     binary data of the given type.
  //   * 'prototype' property:
  //     * [[Class]] "CDataProto"
  //     * __proto__ === ctypes.CData.prototype
  //     * 'constructor' property === 't'
#define DEFINE_TYPE(name, type, ffiType)                                       \
  RootedObject typeObj_##name(cx,                                              \
    CType::DefineBuiltin(cx, parent, #name, CTypeProto, CDataProto, #name,     \
      TYPE_##name, INT_TO_JSVAL(sizeof(type)),                                 \
      INT_TO_JSVAL(ffiType.alignment), &ffiType));                             \
  if (!typeObj_##name)                                                         \
    return false;
#include "typedefs.h"

  // Alias 'ctypes.unsigned' as 'ctypes.unsigned_int', since they represent
  // the same type in C.
  if (!JS_DefineProperty(cx, parent, "unsigned",
         OBJECT_TO_JSVAL(typeObj_unsigned_int), NULL, NULL,
         JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT))
    return false;

  // Create objects representing the special types void_t and voidptr_t.
  RootedObject typeObj(cx,
    CType::DefineBuiltin(cx, parent, "void_t", CTypeProto, CDataProto, "void",
                         TYPE_void_t, JSVAL_VOID, JSVAL_VOID, &ffi_type_void));
  if (!typeObj)
    return false;

  typeObj = PointerType::CreateInternal(cx, typeObj);
  if (!typeObj)
    return false;
  if (!JS_DefineProperty(cx, parent, "voidptr_t", OBJECT_TO_JSVAL(typeObj),
         NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT))
    return false;

  return true;
}

bool
IsCTypesGlobal(JSObject* obj)
{
  return JS_GetClass(obj) == &sCTypesGlobalClass;
}

// Get the JSCTypesCallbacks struct from the 'ctypes' object 'obj'.
JSCTypesCallbacks*
GetCallbacks(JSObject* obj)
{
  JS_ASSERT(IsCTypesGlobal(obj));

  jsval result = JS_GetReservedSlot(obj, SLOT_CALLBACKS);
  if (JSVAL_IS_VOID(result))
    return NULL;

  return static_cast<JSCTypesCallbacks*>(JSVAL_TO_PRIVATE(result));
}

// Utility function to access a property of an object as an object
// returns false and sets the error if the property does not exist
// or is not an object
bool GetObjectProperty(JSContext *cx, HandleObject obj,
                       const char *property, MutableHandleObject result)
{
  jsval val;
  if (!JS_GetProperty(cx, obj, property, &val)) {
    return false;
  }

  if (JSVAL_IS_PRIMITIVE(val)) {
    JS_ReportError(cx, "missing or non-object field");
    return false;
  }

  result.set(JSVAL_TO_OBJECT(val));
  return true;
}

JS_BEGIN_EXTERN_C

JS_PUBLIC_API(JSBool)
JS_InitCTypesClass(JSContext* cx, JSObject *globalArg)
{
  RootedObject global(cx, globalArg);

  // attach ctypes property to global object
  RootedObject ctypes(cx, JS_NewObject(cx, &sCTypesGlobalClass, NULL, NULL));
  if (!ctypes)
    return false;

  if (!JS_DefineProperty(cx, global, "ctypes", OBJECT_TO_JSVAL(ctypes),
         JS_PropertyStub, JS_StrictPropertyStub, JSPROP_READONLY | JSPROP_PERMANENT)){
    return false;
  }

  if (!InitTypeClasses(cx, ctypes))
    return false;

  // attach API functions and properties
  if (!JS_DefineFunctions(cx, ctypes, sModuleFunctions) ||
      !JS_DefineProperties(cx, ctypes, sModuleProps))
    return false;

  // Set up ctypes.CDataFinalizer.prototype.
  RootedObject ctor(cx);
  if (!GetObjectProperty(cx, ctypes, "CDataFinalizer", &ctor))
    return false;

  RootedObject prototype(cx, JS_NewObject(cx, &sCDataFinalizerProtoClass, NULL, ctypes));
  if (!prototype)
    return false;

  if (!JS_DefineProperties(cx, prototype, sCDataFinalizerProps) ||
      !JS_DefineFunctions(cx, prototype, sCDataFinalizerFunctions))
    return false;

  if (!JS_DefineProperty(cx, ctor, "prototype", OBJECT_TO_JSVAL(prototype),
                         NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT))
    return false;

  if (!JS_DefineProperty(cx, prototype, "constructor", OBJECT_TO_JSVAL(ctor),
                         NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT))
    return false;


  // Seal the ctypes object, to prevent modification.
  return JS_FreezeObject(cx, ctypes);
}

JS_PUBLIC_API(void)
JS_SetCTypesCallbacks(JSRawObject ctypesObj, JSCTypesCallbacks* callbacks)
{
  JS_ASSERT(callbacks);
  JS_ASSERT(IsCTypesGlobal(ctypesObj));

  // Set the callbacks on a reserved slot.
  JS_SetReservedSlot(ctypesObj, SLOT_CALLBACKS, PRIVATE_TO_JSVAL(callbacks));
}

JS_END_EXTERN_C

/*******************************************************************************
** Type conversion functions
*******************************************************************************/

// Enforce some sanity checks on type widths and properties.
// Where the architecture is 64-bit, make sure it's LP64 or LLP64. (ctypes.int
// autoconverts to a primitive JS number; to support ILP64 architectures, it
// would need to autoconvert to an Int64 object instead. Therefore we enforce
// this invariant here.)
JS_STATIC_ASSERT(sizeof(bool) == 1 || sizeof(bool) == 4);
JS_STATIC_ASSERT(sizeof(char) == 1);
JS_STATIC_ASSERT(sizeof(short) == 2);
JS_STATIC_ASSERT(sizeof(int) == 4);
JS_STATIC_ASSERT(sizeof(unsigned) == 4);
JS_STATIC_ASSERT(sizeof(long) == 4 || sizeof(long) == 8);
JS_STATIC_ASSERT(sizeof(long long) == 8);
JS_STATIC_ASSERT(sizeof(size_t) == sizeof(uintptr_t));
JS_STATIC_ASSERT(sizeof(float) == 4);
JS_STATIC_ASSERT(sizeof(PRFuncPtr) == sizeof(void*));
JS_STATIC_ASSERT(numeric_limits<double>::is_signed);

// Templated helper to convert FromType to TargetType, for the default case
// where the trivial POD constructor will do.
template<class TargetType, class FromType>
struct ConvertImpl {
  static JS_ALWAYS_INLINE TargetType Convert(FromType d) {
    return TargetType(d);
  }
};

#ifdef _MSC_VER
// MSVC can't perform double to unsigned __int64 conversion when the
// double is greater than 2^63 - 1. Help it along a little.
template<>
struct ConvertImpl<uint64_t, double> {
  static JS_ALWAYS_INLINE uint64_t Convert(double d) {
    return d > 0x7fffffffffffffffui64 ?
           uint64_t(d - 0x8000000000000000ui64) + 0x8000000000000000ui64 :
           uint64_t(d);
  }
};
#endif

// C++ doesn't guarantee that exact values are the only ones that will
// round-trip. In fact, on some platforms, including SPARC, there are pairs of
// values, a uint64_t and a double, such that neither value is exactly
// representable in the other type, but they cast to each other.
#ifdef SPARC
// Simulate x86 overflow behavior
template<>
struct ConvertImpl<uint64_t, double> {
  static JS_ALWAYS_INLINE uint64_t Convert(double d) {
    return d >= 0xffffffffffffffff ?
           0x8000000000000000 : uint64_t(d);
  }
};

template<>
struct ConvertImpl<int64_t, double> {
  static JS_ALWAYS_INLINE int64_t Convert(double d) {
    return d >= 0x7fffffffffffffff ?
           0x8000000000000000 : int64_t(d);
  }
};
#endif

template<class TargetType, class FromType>
static JS_ALWAYS_INLINE TargetType Convert(FromType d)
{
  return ConvertImpl<TargetType, FromType>::Convert(d);
}

template<class TargetType, class FromType>
static JS_ALWAYS_INLINE bool IsAlwaysExact()
{
  // Return 'true' if TargetType can always exactly represent FromType.
  // This means that:
  // 1) TargetType must be the same or more bits wide as FromType. For integers
  //    represented in 'n' bits, unsigned variants will have 'n' digits while
  //    signed will have 'n - 1'. For floating point types, 'digits' is the
  //    mantissa width.
  // 2) If FromType is signed, TargetType must also be signed. (Floating point
  //    types are always signed.)
  // 3) If TargetType is an exact integral type, FromType must be also.
  if (numeric_limits<TargetType>::digits < numeric_limits<FromType>::digits)
    return false;

  if (numeric_limits<FromType>::is_signed &&
      !numeric_limits<TargetType>::is_signed)
    return false;

  if (!numeric_limits<FromType>::is_exact &&
      numeric_limits<TargetType>::is_exact)
    return false;

  return true;
}

// Templated helper to determine if FromType 'i' converts losslessly to
// TargetType 'j'. Default case where both types are the same signedness.
template<class TargetType, class FromType, bool TargetSigned, bool FromSigned>
struct IsExactImpl {
  static JS_ALWAYS_INLINE bool Test(FromType i, TargetType j) {
    JS_STATIC_ASSERT(numeric_limits<TargetType>::is_exact);
    return FromType(j) == i;
  }
};

// Specialization where TargetType is unsigned, FromType is signed.
template<class TargetType, class FromType>
struct IsExactImpl<TargetType, FromType, false, true> {
  static JS_ALWAYS_INLINE bool Test(FromType i, TargetType j) {
    JS_STATIC_ASSERT(numeric_limits<TargetType>::is_exact);
    return i >= 0 && FromType(j) == i;
  }
};

// Specialization where TargetType is signed, FromType is unsigned.
template<class TargetType, class FromType>
struct IsExactImpl<TargetType, FromType, true, false> {
  static JS_ALWAYS_INLINE bool Test(FromType i, TargetType j) {
    JS_STATIC_ASSERT(numeric_limits<TargetType>::is_exact);
    return TargetType(i) >= 0 && FromType(j) == i;
  }
};

// Convert FromType 'i' to TargetType 'result', returning true iff 'result'
// is an exact representation of 'i'.
template<class TargetType, class FromType>
static JS_ALWAYS_INLINE bool ConvertExact(FromType i, TargetType* result)
{
  // Require that TargetType is integral, to simplify conversion.
  JS_STATIC_ASSERT(numeric_limits<TargetType>::is_exact);

  *result = Convert<TargetType>(i);

  // See if we can avoid a dynamic check.
  if (IsAlwaysExact<TargetType, FromType>())
    return true;

  // Return 'true' if 'i' is exactly representable in 'TargetType'.
  return IsExactImpl<TargetType,
                     FromType,
                     numeric_limits<TargetType>::is_signed,
                     numeric_limits<FromType>::is_signed>::Test(i, *result);
}

// Templated helper to determine if Type 'i' is negative. Default case
// where IntegerType is unsigned.
template<class Type, bool IsSigned>
struct IsNegativeImpl {
  static JS_ALWAYS_INLINE bool Test(Type i) {
    return false;
  }
};

// Specialization where Type is signed.
template<class Type>
struct IsNegativeImpl<Type, true> {
  static JS_ALWAYS_INLINE bool Test(Type i) {
    return i < 0;
  }
};

// Determine whether Type 'i' is negative.
template<class Type>
static JS_ALWAYS_INLINE bool IsNegative(Type i)
{
  return IsNegativeImpl<Type, numeric_limits<Type>::is_signed>::Test(i);
}

// Implicitly convert val to bool, allowing JSBool, int, and double
// arguments numerically equal to 0 or 1.
static bool
jsvalToBool(JSContext* cx, jsval val, bool* result)
{
  if (JSVAL_IS_BOOLEAN(val)) {
    *result = JSVAL_TO_BOOLEAN(val) != JS_FALSE;
    return true;
  }
  if (JSVAL_IS_INT(val)) {
    int32_t i = JSVAL_TO_INT(val);
    *result = i != 0;
    return i == 0 || i == 1;
  }
  if (JSVAL_IS_DOUBLE(val)) {
    double d = JSVAL_TO_DOUBLE(val);
    *result = d != 0;
    // Allow -0.
    return d == 1 || d == 0;
  }
  // Don't silently convert null to bool. It's probably a mistake.
  return false;
}

// Implicitly convert val to IntegerType, allowing JSBool, int, double,
// Int64, UInt64, and CData integer types 't' where all values of 't' are
// representable by IntegerType.
template<class IntegerType>
static bool
jsvalToInteger(JSContext* cx, jsval val, IntegerType* result)
{
  JS_STATIC_ASSERT(numeric_limits<IntegerType>::is_exact);

  if (JSVAL_IS_INT(val)) {
    // Make sure the integer fits in the alotted precision, and has the right
    // sign.
    int32_t i = JSVAL_TO_INT(val);
    return ConvertExact(i, result);
  }
  if (JSVAL_IS_DOUBLE(val)) {
    // Don't silently lose bits here -- check that val really is an
    // integer value, and has the right sign.
    double d = JSVAL_TO_DOUBLE(val);
    return ConvertExact(d, result);
  }
  if (!JSVAL_IS_PRIMITIVE(val)) {
    JSObject* obj = JSVAL_TO_OBJECT(val);
    if (CData::IsCData(obj)) {
      JSObject* typeObj = CData::GetCType(obj);
      void* data = CData::GetData(obj);

      // Check whether the source type is always representable, with exact
      // precision, by the target type. If it is, convert the value.
      switch (CType::GetTypeCode(typeObj)) {
#define DEFINE_INT_TYPE(name, fromType, ffiType)                               \
      case TYPE_##name:                                                        \
        if (!IsAlwaysExact<IntegerType, fromType>())                           \
          return false;                                                        \
        *result = IntegerType(*static_cast<fromType*>(data));                  \
        return true;
#define DEFINE_WRAPPED_INT_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#include "typedefs.h"
      case TYPE_void_t:
      case TYPE_bool:
      case TYPE_float:
      case TYPE_double:
      case TYPE_float32_t:
      case TYPE_float64_t:
      case TYPE_char:
      case TYPE_signed_char:
      case TYPE_unsigned_char:
      case TYPE_jschar:
      case TYPE_pointer:
      case TYPE_function:
      case TYPE_array:
      case TYPE_struct:
        // Not a compatible number type.
        return false;
      }
    }

    if (Int64::IsInt64(obj)) {
      // Make sure the integer fits in IntegerType.
      int64_t i = Int64Base::GetInt(obj);
      return ConvertExact(i, result);
    }

    if (UInt64::IsUInt64(obj)) {
      // Make sure the integer fits in IntegerType.
      uint64_t i = Int64Base::GetInt(obj);
      return ConvertExact(i, result);
    }

    if (CDataFinalizer::IsCDataFinalizer(obj)) {
      jsval innerData;
      if (!CDataFinalizer::GetValue(cx, obj, &innerData)) {
        return false; // Nothing to convert
      }
      return jsvalToInteger(cx, innerData, result);
    }

    return false;
  }
  if (JSVAL_IS_BOOLEAN(val)) {
    // Implicitly promote boolean values to 0 or 1, like C.
    *result = JSVAL_TO_BOOLEAN(val);
    JS_ASSERT(*result == 0 || *result == 1);
    return true;
  }
  // Don't silently convert null to an integer. It's probably a mistake.
  return false;
}

// Implicitly convert val to FloatType, allowing int, double,
// Int64, UInt64, and CData numeric types 't' where all values of 't' are
// representable by FloatType.
template<class FloatType>
static bool
jsvalToFloat(JSContext *cx, jsval val, FloatType* result)
{
  JS_STATIC_ASSERT(!numeric_limits<FloatType>::is_exact);

  // The following casts may silently throw away some bits, but there's
  // no good way around it. Sternly requiring that the 64-bit double
  // argument be exactly representable as a 32-bit float is
  // unrealistic: it would allow 1/2 to pass but not 1/3.
  if (JSVAL_IS_INT(val)) {
    *result = FloatType(JSVAL_TO_INT(val));
    return true;
  }
  if (JSVAL_IS_DOUBLE(val)) {
    *result = FloatType(JSVAL_TO_DOUBLE(val));
    return true;
  }
  if (!JSVAL_IS_PRIMITIVE(val)) {
    JSObject* obj = JSVAL_TO_OBJECT(val);
    if (CData::IsCData(obj)) {
      JSObject* typeObj = CData::GetCType(obj);
      void* data = CData::GetData(obj);

      // Check whether the source type is always representable, with exact
      // precision, by the target type. If it is, convert the value.
      switch (CType::GetTypeCode(typeObj)) {
#define DEFINE_FLOAT_TYPE(name, fromType, ffiType)                             \
      case TYPE_##name:                                                        \
        if (!IsAlwaysExact<FloatType, fromType>())                             \
          return false;                                                        \
        *result = FloatType(*static_cast<fromType*>(data));                    \
        return true;
#define DEFINE_INT_TYPE(x, y, z) DEFINE_FLOAT_TYPE(x, y, z)
#define DEFINE_WRAPPED_INT_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#include "typedefs.h"
      case TYPE_void_t:
      case TYPE_bool:
      case TYPE_char:
      case TYPE_signed_char:
      case TYPE_unsigned_char:
      case TYPE_jschar:
      case TYPE_pointer:
      case TYPE_function:
      case TYPE_array:
      case TYPE_struct:
        // Not a compatible number type.
        return false;
      }
    }
  }
  // Don't silently convert true to 1.0 or false to 0.0, even though C/C++
  // does it. It's likely to be a mistake.
  return false;
}

template<class IntegerType>
static bool
StringToInteger(JSContext* cx, JSString* string, IntegerType* result)
{
  JS_STATIC_ASSERT(numeric_limits<IntegerType>::is_exact);

  const jschar* cp = string->getChars(NULL);
  if (!cp)
    return false;

  const jschar* end = cp + string->length();
  if (cp == end)
    return false;

  IntegerType sign = 1;
  if (cp[0] == '-') {
    if (!numeric_limits<IntegerType>::is_signed)
      return false;

    sign = -1;
    ++cp;
  }

  // Assume base-10, unless the string begins with '0x' or '0X'.
  IntegerType base = 10;
  if (end - cp > 2 && cp[0] == '0' && (cp[1] == 'x' || cp[1] == 'X')) {
    cp += 2;
    base = 16;
  }

  // Scan the string left to right and build the number,
  // checking for valid characters 0 - 9, a - f, A - F and overflow.
  IntegerType i = 0;
  while (cp != end) {
    jschar c = *cp++;
    if (c >= '0' && c <= '9')
      c -= '0';
    else if (base == 16 && c >= 'a' && c <= 'f')
      c = c - 'a' + 10;
    else if (base == 16 && c >= 'A' && c <= 'F')
      c = c - 'A' + 10;
    else
      return false;

    IntegerType ii = i;
    i = ii * base + sign * c;
    if (i / base != ii) // overflow
      return false;
  }

  *result = i;
  return true;
}

// Implicitly convert val to IntegerType, allowing int, double,
// Int64, UInt64, and optionally a decimal or hexadecimal string argument.
// (This is common code shared by jsvalToSize and the Int64/UInt64 constructors.)
template<class IntegerType>
static bool
jsvalToBigInteger(JSContext* cx,
                  jsval val,
                  bool allowString,
                  IntegerType* result)
{
  JS_STATIC_ASSERT(numeric_limits<IntegerType>::is_exact);

  if (JSVAL_IS_INT(val)) {
    // Make sure the integer fits in the alotted precision, and has the right
    // sign.
    int32_t i = JSVAL_TO_INT(val);
    return ConvertExact(i, result);
  }
  if (JSVAL_IS_DOUBLE(val)) {
    // Don't silently lose bits here -- check that val really is an
    // integer value, and has the right sign.
    double d = JSVAL_TO_DOUBLE(val);
    return ConvertExact(d, result);
  }
  if (allowString && JSVAL_IS_STRING(val)) {
    // Allow conversion from base-10 or base-16 strings, provided the result
    // fits in IntegerType. (This allows an Int64 or UInt64 object to be passed
    // to the JS array element operator, which will automatically call
    // toString() on the object for us.)
    return StringToInteger(cx, JSVAL_TO_STRING(val), result);
  }
  if (!JSVAL_IS_PRIMITIVE(val)) {
    // Allow conversion from an Int64 or UInt64 object directly.
    JSObject* obj = JSVAL_TO_OBJECT(val);

    if (UInt64::IsUInt64(obj)) {
      // Make sure the integer fits in IntegerType.
      uint64_t i = Int64Base::GetInt(obj);
      return ConvertExact(i, result);
    }

    if (Int64::IsInt64(obj)) {
      // Make sure the integer fits in IntegerType.
      int64_t i = Int64Base::GetInt(obj);
      return ConvertExact(i, result);
    }

    if (CDataFinalizer::IsCDataFinalizer(obj)) {
      jsval innerData;
      if (!CDataFinalizer::GetValue(cx, obj, &innerData)) {
        return false; // Nothing to convert
      }
      return jsvalToBigInteger(cx, innerData, allowString, result);
    }

  }
  return false;
}

// Implicitly convert val to a size value, where the size value is represented
// by size_t but must also fit in a double.
static bool
jsvalToSize(JSContext* cx, jsval val, bool allowString, size_t* result)
{
  if (!jsvalToBigInteger(cx, val, allowString, result))
    return false;

  // Also check that the result fits in a double.
  return Convert<size_t>(double(*result)) == *result;
}

// Implicitly convert val to IntegerType, allowing int, double,
// Int64, UInt64, and optionally a decimal or hexadecimal string argument.
// (This is common code shared by jsvalToSize and the Int64/UInt64 constructors.)
template<class IntegerType>
static bool
jsidToBigInteger(JSContext* cx,
                  jsid val,
                  bool allowString,
                  IntegerType* result)
{
  JS_STATIC_ASSERT(numeric_limits<IntegerType>::is_exact);

  if (JSID_IS_INT(val)) {
    // Make sure the integer fits in the alotted precision, and has the right
    // sign.
    int32_t i = JSID_TO_INT(val);
    return ConvertExact(i, result);
  }
  if (allowString && JSID_IS_STRING(val)) {
    // Allow conversion from base-10 or base-16 strings, provided the result
    // fits in IntegerType. (This allows an Int64 or UInt64 object to be passed
    // to the JS array element operator, which will automatically call
    // toString() on the object for us.)
    return StringToInteger(cx, JSID_TO_STRING(val), result);
  }
  if (JSID_IS_OBJECT(val)) {
    // Allow conversion from an Int64 or UInt64 object directly.
    JSObject* obj = JSID_TO_OBJECT(val);

    if (UInt64::IsUInt64(obj)) {
      // Make sure the integer fits in IntegerType.
      uint64_t i = Int64Base::GetInt(obj);
      return ConvertExact(i, result);
    }

    if (Int64::IsInt64(obj)) {
      // Make sure the integer fits in IntegerType.
      int64_t i = Int64Base::GetInt(obj);
      return ConvertExact(i, result);
    }
  }
  return false;
}

// Implicitly convert val to a size value, where the size value is represented
// by size_t but must also fit in a double.
static bool
jsidToSize(JSContext* cx, jsid val, bool allowString, size_t* result)
{
  if (!jsidToBigInteger(cx, val, allowString, result))
    return false;

  // Also check that the result fits in a double.
  return Convert<size_t>(double(*result)) == *result;
}

// Implicitly convert a size value to a jsval, ensuring that the size_t value
// fits in a double.
static JSBool
SizeTojsval(JSContext* cx, size_t size, jsval* result)
{
  if (Convert<size_t>(double(size)) != size) {
    JS_ReportError(cx, "size overflow");
    return false;
  }

  *result = JS_NumberValue(double(size));
  return true;
}

// Forcefully convert val to IntegerType when explicitly requested.
template<class IntegerType>
static bool
jsvalToIntegerExplicit(jsval val, IntegerType* result)
{
  JS_STATIC_ASSERT(numeric_limits<IntegerType>::is_exact);

  if (JSVAL_IS_DOUBLE(val)) {
    // Convert -Inf, Inf, and NaN to 0; otherwise, convert by C-style cast.
    double d = JSVAL_TO_DOUBLE(val);
    *result = MOZ_DOUBLE_IS_FINITE(d) ? IntegerType(d) : 0;
    return true;
  }
  if (!JSVAL_IS_PRIMITIVE(val)) {
    // Convert Int64 and UInt64 values by C-style cast.
    JSObject* obj = JSVAL_TO_OBJECT(val);
    if (Int64::IsInt64(obj)) {
      int64_t i = Int64Base::GetInt(obj);
      *result = IntegerType(i);
      return true;
    }
    if (UInt64::IsUInt64(obj)) {
      uint64_t i = Int64Base::GetInt(obj);
      *result = IntegerType(i);
      return true;
    }
  }
  return false;
}

// Forcefully convert val to a pointer value when explicitly requested.
static bool
jsvalToPtrExplicit(JSContext* cx, jsval val, uintptr_t* result)
{
  if (JSVAL_IS_INT(val)) {
    // int32_t always fits in intptr_t. If the integer is negative, cast through
    // an intptr_t intermediate to sign-extend.
    int32_t i = JSVAL_TO_INT(val);
    *result = i < 0 ? uintptr_t(intptr_t(i)) : uintptr_t(i);
    return true;
  }
  if (JSVAL_IS_DOUBLE(val)) {
    double d = JSVAL_TO_DOUBLE(val);
    if (d < 0) {
      // Cast through an intptr_t intermediate to sign-extend.
      intptr_t i = Convert<intptr_t>(d);
      if (double(i) != d)
        return false;

      *result = uintptr_t(i);
      return true;
    }

    // Don't silently lose bits here -- check that val really is an
    // integer value, and has the right sign.
    *result = Convert<uintptr_t>(d);
    return double(*result) == d;
  }
  if (!JSVAL_IS_PRIMITIVE(val)) {
    JSObject* obj = JSVAL_TO_OBJECT(val);
    if (Int64::IsInt64(obj)) {
      int64_t i = Int64Base::GetInt(obj);
      intptr_t p = intptr_t(i);

      // Make sure the integer fits in the alotted precision.
      if (int64_t(p) != i)
        return false;
      *result = uintptr_t(p);
      return true;
    }

    if (UInt64::IsUInt64(obj)) {
      uint64_t i = Int64Base::GetInt(obj);

      // Make sure the integer fits in the alotted precision.
      *result = uintptr_t(i);
      return uint64_t(*result) == i;
    }
  }
  return false;
}

template<class IntegerType, class CharType, size_t N, class AP>
void
IntegerToString(IntegerType i, int radix, Vector<CharType, N, AP>& result)
{
  JS_STATIC_ASSERT(numeric_limits<IntegerType>::is_exact);

  // The buffer must be big enough for all the bits of IntegerType to fit,
  // in base-2, including '-'.
  CharType buffer[sizeof(IntegerType) * 8 + 1];
  CharType* end = buffer + sizeof(buffer) / sizeof(CharType);
  CharType* cp = end;

  // Build the string in reverse. We use multiplication and subtraction
  // instead of modulus because that's much faster.
  const bool isNegative = IsNegative(i);
  size_t sign = isNegative ? -1 : 1;
  do {
    IntegerType ii = i / IntegerType(radix);
    size_t index = sign * size_t(i - ii * IntegerType(radix));
    *--cp = "0123456789abcdefghijklmnopqrstuvwxyz"[index];
    i = ii;
  } while (i != 0);

  if (isNegative)
    *--cp = '-';

  JS_ASSERT(cp >= buffer);
  result.append(cp, end);
}

template<class CharType>
static size_t
strnlen(const CharType* begin, size_t max)
{
  for (const CharType* s = begin; s != begin + max; ++s)
    if (*s == 0)
      return s - begin;

  return max;
}

// Convert C binary value 'data' of CType 'typeObj' to a JS primitive, where
// possible; otherwise, construct and return a CData object. The following
// semantics apply when constructing a CData object for return:
// * If 'wantPrimitive' is true, the caller indicates that 'result' must be
//   a JS primitive, and ConvertToJS will fail if 'result' would be a CData
//   object. Otherwise:
// * If a CData object 'parentObj' is supplied, the new CData object is
//   dependent on the given parent and its buffer refers to a slice of the
//   parent's buffer.
// * If 'parentObj' is null, the new CData object may or may not own its
//   resulting buffer depending on the 'ownResult' argument.
JSBool
ConvertToJS(JSContext* cx,
            HandleObject typeObj,
            HandleObject parentObj,
            void* data,
            bool wantPrimitive,
            bool ownResult,
            jsval* result)
{
  JS_ASSERT(!parentObj || CData::IsCData(parentObj));
  JS_ASSERT(!parentObj || !ownResult);
  JS_ASSERT(!wantPrimitive || !ownResult);

  TypeCode typeCode = CType::GetTypeCode(typeObj);

  switch (typeCode) {
  case TYPE_void_t:
    *result = JSVAL_VOID;
    break;
  case TYPE_bool:
    *result = *static_cast<bool*>(data) ? JSVAL_TRUE : JSVAL_FALSE;
    break;
#define DEFINE_INT_TYPE(name, type, ffiType)                                   \
  case TYPE_##name: {                                                          \
    type value = *static_cast<type*>(data);                                    \
    if (sizeof(type) < 4)                                                      \
      *result = INT_TO_JSVAL(int32_t(value));                                  \
    else                                                                       \
      *result = JS_NumberValue(double(value));                                 \
    break;                                                                     \
  }
#define DEFINE_WRAPPED_INT_TYPE(name, type, ffiType)                           \
  case TYPE_##name: {                                                          \
    /* Return an Int64 or UInt64 object - do not convert to a JS number. */    \
    uint64_t value;                                                            \
    RootedObject proto(cx);                                                    \
    if (!numeric_limits<type>::is_signed) {                                    \
      value = *static_cast<type*>(data);                                       \
      /* Get ctypes.UInt64.prototype from ctypes.CType.prototype. */           \
      proto = CType::GetProtoFromType(typeObj, SLOT_UINT64PROTO);              \
    } else {                                                                   \
      value = int64_t(*static_cast<type*>(data));                              \
      /* Get ctypes.Int64.prototype from ctypes.CType.prototype. */            \
      proto = CType::GetProtoFromType(typeObj, SLOT_INT64PROTO);               \
    }                                                                          \
                                                                               \
    JSObject* obj = Int64Base::Construct(cx, proto, value,                     \
      !numeric_limits<type>::is_signed);                                       \
    if (!obj)                                                                  \
      return false;                                                            \
    *result = OBJECT_TO_JSVAL(obj);                                            \
    break;                                                                     \
  }
#define DEFINE_FLOAT_TYPE(name, type, ffiType)                                 \
  case TYPE_##name: {                                                          \
    type value = *static_cast<type*>(data);                                    \
    *result = JS_NumberValue(double(value));                                   \
    break;                                                                     \
  }
#define DEFINE_CHAR_TYPE(name, type, ffiType)                                  \
  case TYPE_##name:                                                            \
    /* Convert to an integer. We have no idea what character encoding to */    \
    /* use, if any. */                                                         \
    *result = INT_TO_JSVAL(*static_cast<type*>(data));                         \
    break;
#include "typedefs.h"
  case TYPE_jschar: {
    // Convert the jschar to a 1-character string.
    JSString* str = JS_NewUCStringCopyN(cx, static_cast<jschar*>(data), 1);
    if (!str)
      return false;

    *result = STRING_TO_JSVAL(str);
    break;
  }
  case TYPE_pointer:
  case TYPE_array:
  case TYPE_struct: {
    // We're about to create a new CData object to return. If the caller doesn't
    // want this, return early.
    if (wantPrimitive) {
      JS_ReportError(cx, "cannot convert to primitive value");
      return false;
    }

    JSObject* obj = CData::Create(cx, typeObj, parentObj, data, ownResult);
    if (!obj)
      return false;

    *result = OBJECT_TO_JSVAL(obj);
    break;
  }
  case TYPE_function:
    JS_NOT_REACHED("cannot return a FunctionType");
  }

  return true;
}

// Implicitly convert jsval 'val' to a C binary representation of CType
// 'targetType', storing the result in 'buffer'. Adequate space must be
// provided in 'buffer' by the caller. This function generally does minimal
// coercion between types. There are two cases in which this function is used:
// 1) The target buffer is internal to a CData object; we simply write data
//    into it.
// 2) We are converting an argument for an ffi call, in which case 'isArgument'
//    will be true. This allows us to handle a special case: if necessary,
//    we can autoconvert a JS string primitive to a pointer-to-character type.
//    In this case, ownership of the allocated string is handed off to the
//    caller; 'freePointer' will be set to indicate this.
JSBool
ImplicitConvert(JSContext* cx,
                jsval val,
                JSObject* targetType,
                void* buffer,
                bool isArgument,
                bool* freePointer)
{
  JS_ASSERT(CType::IsSizeDefined(targetType));

  // First, check if val is either a CData object or a CDataFinalizer
  // of type targetType.
  JSObject* sourceData = NULL;
  JSObject* sourceType = NULL;
  RootedObject valObj(cx, NULL);
  if (!JSVAL_IS_PRIMITIVE(val)) {
    valObj = JSVAL_TO_OBJECT(val);
    if (CData::IsCData(valObj)) {
      sourceData = valObj;
      sourceType = CData::GetCType(sourceData);

      // If the types are equal, copy the buffer contained within the CData.
      // (Note that the buffers may overlap partially or completely.)
      if (CType::TypesEqual(sourceType, targetType)) {
        size_t size = CType::GetSize(sourceType);
        memmove(buffer, CData::GetData(sourceData), size);
        return true;
      }
    } else if (CDataFinalizer::IsCDataFinalizer(valObj)) {
      sourceData = valObj;
      sourceType = CDataFinalizer::GetCType(cx, sourceData);

      CDataFinalizer::Private *p = (CDataFinalizer::Private *)
        JS_GetPrivate(sourceData);

      if (!p) {
        // We have called |dispose| or |forget| already.
        JS_ReportError(cx, "Attempting to convert an empty CDataFinalizer");
        return JS_FALSE;
      }

      // If the types are equal, copy the buffer contained within the CData.
      if (CType::TypesEqual(sourceType, targetType)) {
        memmove(buffer, p->cargs, p->cargs_size);
        return true;
      }
    }
  }

  TypeCode targetCode = CType::GetTypeCode(targetType);

  switch (targetCode) {
  case TYPE_bool: {
    // Do not implicitly lose bits, but allow the values 0, 1, and -0.
    // Programs can convert explicitly, if needed, using `Boolean(v)` or `!!v`.
    bool result;
    if (!jsvalToBool(cx, val, &result))
      return TypeError(cx, "boolean", val);
    *static_cast<bool*>(buffer) = result;
    break;
  }
#define DEFINE_INT_TYPE(name, type, ffiType)                                   \
  case TYPE_##name: {                                                          \
    /* Do not implicitly lose bits. */                                         \
    type result;                                                               \
    if (!jsvalToInteger(cx, val, &result))                                     \
      return TypeError(cx, #name, val);                                        \
    *static_cast<type*>(buffer) = result;                                      \
    break;                                                                     \
  }
#define DEFINE_WRAPPED_INT_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#define DEFINE_FLOAT_TYPE(name, type, ffiType)                                 \
  case TYPE_##name: {                                                          \
    type result;                                                               \
    if (!jsvalToFloat(cx, val, &result))                                       \
      return TypeError(cx, #name, val);                                        \
    *static_cast<type*>(buffer) = result;                                      \
    break;                                                                     \
  }
#define DEFINE_CHAR_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#define DEFINE_JSCHAR_TYPE(name, type, ffiType)                                \
  case TYPE_##name: {                                                          \
    /* Convert from a 1-character string, regardless of encoding, */           \
    /* or from an integer, provided the result fits in 'type'. */              \
    type result;                                                               \
    if (JSVAL_IS_STRING(val)) {                                                \
      JSString* str = JSVAL_TO_STRING(val);                                    \
      if (str->length() != 1)                                                  \
        return TypeError(cx, #name, val);                                      \
      const jschar *chars = str->getChars(cx);                                 \
      if (!chars)                                                              \
        return false;                                                          \
      result = chars[0];                                                       \
    } else if (!jsvalToInteger(cx, val, &result)) {                            \
      return TypeError(cx, #name, val);                                        \
    }                                                                          \
    *static_cast<type*>(buffer) = result;                                      \
    break;                                                                     \
  }
#include "typedefs.h"
  case TYPE_pointer: {
    if (JSVAL_IS_NULL(val)) {
      // Convert to a null pointer.
      *static_cast<void**>(buffer) = NULL;
      break;
    }

    JSObject* baseType = PointerType::GetBaseType(targetType);
    if (sourceData) {
      // First, determine if the targetType is ctypes.void_t.ptr.
      TypeCode sourceCode = CType::GetTypeCode(sourceType);
      void* sourceBuffer = CData::GetData(sourceData);
      bool voidptrTarget = CType::GetTypeCode(baseType) == TYPE_void_t;

      if (sourceCode == TYPE_pointer && voidptrTarget) {
        // Autoconvert if targetType is ctypes.voidptr_t.
        *static_cast<void**>(buffer) = *static_cast<void**>(sourceBuffer);
        break;
      }
      if (sourceCode == TYPE_array) {
        // Autoconvert an array to a ctypes.void_t.ptr or to
        // sourceType.elementType.ptr, just like C.
        JSObject* elementType = ArrayType::GetBaseType(sourceType);
        if (voidptrTarget || CType::TypesEqual(baseType, elementType)) {
          *static_cast<void**>(buffer) = sourceBuffer;
          break;
        }
      }

    } else if (isArgument && JSVAL_IS_STRING(val)) {
      // Convert the string for the ffi call. This requires allocating space
      // which the caller assumes ownership of.
      // TODO: Extend this so we can safely convert strings at other times also.
      JSString* sourceString = JSVAL_TO_STRING(val);
      size_t sourceLength = sourceString->length();
      const jschar* sourceChars = sourceString->getChars(cx);
      if (!sourceChars)
        return false;

      switch (CType::GetTypeCode(baseType)) {
      case TYPE_char:
      case TYPE_signed_char:
      case TYPE_unsigned_char: {
        // Convert from UTF-16 to UTF-8.
        size_t nbytes =
          GetDeflatedUTF8StringLength(cx, sourceChars, sourceLength);
        if (nbytes == (size_t) -1)
          return false;

        char** charBuffer = static_cast<char**>(buffer);
        *charBuffer = cx->array_new<char>(nbytes + 1);
        if (!*charBuffer) {
          JS_ReportAllocationOverflow(cx);
          return false;
        }

        ASSERT_OK(DeflateStringToUTF8Buffer(cx, sourceChars, sourceLength,
                    *charBuffer, &nbytes));
        (*charBuffer)[nbytes] = 0;
        *freePointer = true;
        break;
      }
      case TYPE_jschar: {
        // Copy the jschar string data. (We could provide direct access to the
        // JSString's buffer, but this approach is safer if the caller happens
        // to modify the string.)
        jschar** jscharBuffer = static_cast<jschar**>(buffer);
        *jscharBuffer = cx->array_new<jschar>(sourceLength + 1);
        if (!*jscharBuffer) {
          JS_ReportAllocationOverflow(cx);
          return false;
        }

        *freePointer = true;
        memcpy(*jscharBuffer, sourceChars, sourceLength * sizeof(jschar));
        (*jscharBuffer)[sourceLength] = 0;
        break;
      }
      default:
        return TypeError(cx, "string pointer", val);
      }
      break;
    } else if (!JSVAL_IS_PRIMITIVE(val) && JS_IsArrayBufferObject(valObj, cx)) {
      // Convert ArrayBuffer to pointer without any copy.
      // Just as with C arrays, we make no effort to
      // keep the ArrayBuffer alive.
      *static_cast<void**>(buffer) = JS_GetArrayBufferData(valObj, cx);
      break;
    }
    return TypeError(cx, "pointer", val);
  }
  case TYPE_array: {
    JSObject* baseType = ArrayType::GetBaseType(targetType);
    size_t targetLength = ArrayType::GetLength(targetType);

    if (JSVAL_IS_STRING(val)) {
      JSString* sourceString = JSVAL_TO_STRING(val);
      size_t sourceLength = sourceString->length();
      const jschar* sourceChars = sourceString->getChars(cx);
      if (!sourceChars)
        return false;

      switch (CType::GetTypeCode(baseType)) {
      case TYPE_char:
      case TYPE_signed_char:
      case TYPE_unsigned_char: {
        // Convert from UTF-16 to UTF-8.
        size_t nbytes =
          GetDeflatedUTF8StringLength(cx, sourceChars, sourceLength);
        if (nbytes == (size_t) -1)
          return false;

        if (targetLength < nbytes) {
          JS_ReportError(cx, "ArrayType has insufficient length");
          return false;
        }

        char* charBuffer = static_cast<char*>(buffer);
        ASSERT_OK(DeflateStringToUTF8Buffer(cx, sourceChars, sourceLength,
                    charBuffer, &nbytes));

        if (targetLength > nbytes)
          charBuffer[nbytes] = 0;

        break;
      }
      case TYPE_jschar: {
        // Copy the string data, jschar for jschar, including the terminator
        // if there's space.
        if (targetLength < sourceLength) {
          JS_ReportError(cx, "ArrayType has insufficient length");
          return false;
        }

        memcpy(buffer, sourceChars, sourceLength * sizeof(jschar));
        if (targetLength > sourceLength)
          static_cast<jschar*>(buffer)[sourceLength] = 0;

        break;
      }
      default:
        return TypeError(cx, "array", val);
      }

    } else if (!JSVAL_IS_PRIMITIVE(val) && JS_IsArrayObject(cx, valObj)) {
      // Convert each element of the array by calling ImplicitConvert.
      uint32_t sourceLength;
      if (!JS_GetArrayLength(cx, valObj, &sourceLength) ||
          targetLength != size_t(sourceLength)) {
        JS_ReportError(cx, "ArrayType length does not match source array length");
        return false;
      }

      // Convert into an intermediate, in case of failure.
      size_t elementSize = CType::GetSize(baseType);
      size_t arraySize = elementSize * targetLength;
      AutoPtr<char>::Array intermediate(cx->array_new<char>(arraySize));
      if (!intermediate) {
        JS_ReportAllocationOverflow(cx);
        return false;
      }

      for (uint32_t i = 0; i < sourceLength; ++i) {
        js::AutoValueRooter item(cx);
        if (!JS_GetElement(cx, valObj, i, item.jsval_addr()))
          return false;

        char* data = intermediate.get() + elementSize * i;
        if (!ImplicitConvert(cx, item.jsval_value(), baseType, data, false, NULL))
          return false;
      }

      memcpy(buffer, intermediate.get(), arraySize);

    } else if (!JSVAL_IS_PRIMITIVE(val) &&
               JS_IsArrayBufferObject(valObj, cx)) {
      // Check that array is consistent with type, then
      // copy the array. As with C arrays, data is *not*
      // copied back to the ArrayBuffer at the end of a
      // function call, so do not expect this to work
      // as an inout argument.
      uint32_t sourceLength = JS_GetArrayBufferByteLength(valObj, cx);
      size_t elementSize = CType::GetSize(baseType);
      size_t arraySize = elementSize * targetLength;
      if (arraySize != size_t(sourceLength)) {
        JS_ReportError(cx, "ArrayType length does not match source array length");
        return false;
      }
      memcpy(buffer, JS_GetArrayBufferData(valObj, cx), sourceLength);
      break;
    } else {
      // Don't implicitly convert to string. Users can implicitly convert
      // with `String(x)` or `""+x`.
      return TypeError(cx, "array", val);
    }
    break;
  }
  case TYPE_struct: {
    if (!JSVAL_IS_PRIMITIVE(val) && !sourceData) {
      // Enumerate the properties of the object; if they match the struct
      // specification, convert the fields.
      RootedObject iter(cx, JS_NewPropertyIterator(cx, valObj));
      if (!iter)
        return false;

      // Convert into an intermediate, in case of failure.
      size_t structSize = CType::GetSize(targetType);
      AutoPtr<char>::Array intermediate(cx->array_new<char>(structSize));
      if (!intermediate) {
        JS_ReportAllocationOverflow(cx);
        return false;
      }

      jsid id;
      size_t i = 0;
      while (1) {
        if (!JS_NextProperty(cx, iter, &id))
          return false;
        if (JSID_IS_VOID(id))
          break;

        if (!JSID_IS_STRING(id)) {
          JS_ReportError(cx, "property name is not a string");
          return false;
        }

        JSFlatString *name = JSID_TO_FLAT_STRING(id);
        const FieldInfo* field = StructType::LookupField(cx, targetType, name);
        if (!field)
          return false;

        js::AutoValueRooter prop(cx);
        if (!JS_GetPropertyById(cx, valObj, id, prop.jsval_addr()))
          return false;

        // Convert the field via ImplicitConvert().
        char* fieldData = intermediate.get() + field->mOffset;
        if (!ImplicitConvert(cx, prop.jsval_value(), field->mType, fieldData, false, NULL))
          return false;

        ++i;
      }

      const FieldInfoHash* fields = StructType::GetFieldInfo(targetType);
      if (i != fields->count()) {
        JS_ReportError(cx, "missing fields");
        return false;
      }

      memcpy(buffer, intermediate.get(), structSize);
      break;
    }

    return TypeError(cx, "struct", val);
  }
  case TYPE_void_t:
  case TYPE_function:
    JS_NOT_REACHED("invalid type");
    return false;
  }

  return true;
}

// Convert jsval 'val' to a C binary representation of CType 'targetType',
// storing the result in 'buffer'. This function is more forceful than
// ImplicitConvert.
JSBool
ExplicitConvert(JSContext* cx, jsval val, HandleObject targetType, void* buffer)
{
  // If ImplicitConvert succeeds, use that result.
  if (ImplicitConvert(cx, val, targetType, buffer, false, NULL))
    return true;

  // If ImplicitConvert failed, and there is no pending exception, then assume
  // hard failure (out of memory, or some other similarly serious condition).
  // We store any pending exception in case we need to re-throw it.
  js::AutoValueRooter ex(cx);
  if (!JS_GetPendingException(cx, ex.jsval_addr()))
    return false;

  // Otherwise, assume soft failure. Clear the pending exception so that we
  // can throw a different one as required.
  JS_ClearPendingException(cx);

  TypeCode type = CType::GetTypeCode(targetType);

  switch (type) {
  case TYPE_bool: {
    // Convert according to the ECMAScript ToBoolean() function.
    JSBool result;
    ASSERT_OK(JS_ValueToBoolean(cx, val, &result));
    *static_cast<bool*>(buffer) = result != JS_FALSE;
    break;
  }
#define DEFINE_INT_TYPE(name, type, ffiType)                                   \
  case TYPE_##name: {                                                          \
    /* Convert numeric values with a C-style cast, and */                      \
    /* allow conversion from a base-10 or base-16 string. */                   \
    type result;                                                               \
    if (!jsvalToIntegerExplicit(val, &result) &&                               \
        (!JSVAL_IS_STRING(val) ||                                              \
         !StringToInteger(cx, JSVAL_TO_STRING(val), &result)))                 \
      return TypeError(cx, #name, val);                                        \
    *static_cast<type*>(buffer) = result;                                      \
    break;                                                                     \
  }
#define DEFINE_WRAPPED_INT_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#define DEFINE_CHAR_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#define DEFINE_JSCHAR_TYPE(x, y, z) DEFINE_CHAR_TYPE(x, y, z)
#include "typedefs.h"
  case TYPE_pointer: {
    // Convert a number, Int64 object, or UInt64 object to a pointer.
    uintptr_t result;
    if (!jsvalToPtrExplicit(cx, val, &result))
      return TypeError(cx, "pointer", val);
    *static_cast<uintptr_t*>(buffer) = result;
    break;
  }
  case TYPE_float32_t:
  case TYPE_float64_t:
  case TYPE_float:
  case TYPE_double:
  case TYPE_array:
  case TYPE_struct:
    // ImplicitConvert is sufficient. Re-throw the exception it generated.
    JS_SetPendingException(cx, ex.jsval_value());
    return false;
  case TYPE_void_t:
  case TYPE_function:
    JS_NOT_REACHED("invalid type");
    return false;
  }
  return true;
}

// Given a CType 'typeObj', generate a string describing the C type declaration
// corresponding to 'typeObj'. For instance, the CType constructed from
// 'ctypes.int32_t.ptr.array(4).ptr.ptr' will result in the type string
// 'int32_t*(**)[4]'.
static JSString*
BuildTypeName(JSContext* cx, JSObject* typeObj_)
{
  AutoString result;
  RootedObject typeObj(cx, typeObj_);

  // Walk the hierarchy of types, outermost to innermost, building up the type
  // string. This consists of the base type, which goes on the left.
  // Derived type modifiers (* and []) build from the inside outward, with
  // pointers on the left and arrays on the right. An excellent description
  // of the rules for building C type declarations can be found at:
  // http://unixwiz.net/techtips/reading-cdecl.html
  TypeCode prevGrouping = CType::GetTypeCode(typeObj), currentGrouping;
  while (1) {
    currentGrouping = CType::GetTypeCode(typeObj);
    switch (currentGrouping) {
    case TYPE_pointer: {
      // Pointer types go on the left.
      PrependString(result, "*");

      typeObj = PointerType::GetBaseType(typeObj);
      prevGrouping = currentGrouping;
      continue;
    }
    case TYPE_array: {
      if (prevGrouping == TYPE_pointer) {
        // Outer type is pointer, inner type is array. Grouping is required.
        PrependString(result, "(");
        AppendString(result, ")");
      } 

      // Array types go on the right.
      AppendString(result, "[");
      size_t length;
      if (ArrayType::GetSafeLength(typeObj, &length))
        IntegerToString(length, 10, result);

      AppendString(result, "]");

      typeObj = ArrayType::GetBaseType(typeObj);
      prevGrouping = currentGrouping;
      continue;
    }
    case TYPE_function: {
      FunctionInfo* fninfo = FunctionType::GetFunctionInfo(typeObj);

      // Add in the calling convention, if it's not cdecl.
      // There's no trailing or leading space needed here, as none of the
      // modifiers can produce a string beginning with an identifier ---
      // except for TYPE_function itself, which is fine because functions
      // can't return functions.
      ABICode abi = GetABICode(fninfo->mABI);
      if (abi == ABI_STDCALL)
        PrependString(result, "__stdcall");
      else if (abi == ABI_WINAPI)
        PrependString(result, "WINAPI");

      // Function application binds more tightly than dereferencing, so
      // wrap pointer types in parens. Functions can't return functions
      // (only pointers to them), and arrays can't hold functions
      // (similarly), so we don't need to address those cases.
      if (prevGrouping == TYPE_pointer) {
        PrependString(result, "(");
        AppendString(result, ")");
      }

      // Argument list goes on the right.
      AppendString(result, "(");
      for (size_t i = 0; i < fninfo->mArgTypes.length(); ++i) {
        RootedObject argType(cx, fninfo->mArgTypes[i]);
        JSString* argName = CType::GetName(cx, argType);
        AppendString(result, argName);
        if (i != fninfo->mArgTypes.length() - 1 ||
            fninfo->mIsVariadic)
          AppendString(result, ", ");
      }
      if (fninfo->mIsVariadic)
        AppendString(result, "...");
      AppendString(result, ")");

      // Set 'typeObj' to the return type, and let the loop process it.
      // 'prevGrouping' doesn't matter here, because functions cannot return
      // arrays -- thus the parenthetical rules don't get tickled.
      typeObj = fninfo->mReturnType;
      continue;
    }
    default:
      // Either a basic or struct type. Use the type's name as the base type.
      break;
    }
    break;
  }

  // If prepending the base type name directly would splice two
  // identifiers, insert a space.
  if (('a' <= result[0] && result[0] <= 'z') ||
      ('A' <= result[0] && result[0] <= 'Z') ||
      (result[0] == '_'))
    PrependString(result, " ");

  // Stick the base type and derived type parts together.
  JSString* baseName = CType::GetName(cx, typeObj);
  PrependString(result, baseName);
  return NewUCString(cx, result);
}

// Given a CType 'typeObj', generate a string 'result' such that 'eval(result)'
// would construct the same CType. If 'makeShort' is true, assume that any
// StructType 't' is bound to an in-scope variable of name 't.name', and use
// that variable in place of generating a string to construct the type 't'.
// (This means the type comparison function CType::TypesEqual will return true
// when comparing the input and output of BuildTypeSource, since struct
// equality is determined by strict JSObject pointer equality.)
static void
BuildTypeSource(JSContext* cx,
                JSObject* typeObj_, 
                bool makeShort, 
                AutoString& result)
{
  RootedObject typeObj(cx, typeObj_);

  // Walk the types, building up the toSource() string.
  switch (CType::GetTypeCode(typeObj)) {
  case TYPE_void_t:
#define DEFINE_TYPE(name, type, ffiType)  \
  case TYPE_##name:
#include "typedefs.h"
  {
    AppendString(result, "ctypes.");
    JSString* nameStr = CType::GetName(cx, typeObj);
    AppendString(result, nameStr);
    break;
  }
  case TYPE_pointer: {
    RootedObject baseType(cx, PointerType::GetBaseType(typeObj));

    // Specialcase ctypes.voidptr_t.
    if (CType::GetTypeCode(baseType) == TYPE_void_t) {
      AppendString(result, "ctypes.voidptr_t");
      break;
    }

    // Recursively build the source string, and append '.ptr'.
    BuildTypeSource(cx, baseType, makeShort, result);
    AppendString(result, ".ptr");
    break;
  }
  case TYPE_function: {
    FunctionInfo* fninfo = FunctionType::GetFunctionInfo(typeObj);

    AppendString(result, "ctypes.FunctionType(");

    switch (GetABICode(fninfo->mABI)) {
    case ABI_DEFAULT:
      AppendString(result, "ctypes.default_abi, ");
      break;
    case ABI_STDCALL:
      AppendString(result, "ctypes.stdcall_abi, ");
      break;
    case ABI_WINAPI:
      AppendString(result, "ctypes.winapi_abi, ");
      break;
    case INVALID_ABI:
      JS_NOT_REACHED("invalid abi");
      break;
    }

    // Recursively build the source string describing the function return and
    // argument types.
    BuildTypeSource(cx, fninfo->mReturnType, true, result);

    if (fninfo->mArgTypes.length() > 0) {
      AppendString(result, ", [");
      for (size_t i = 0; i < fninfo->mArgTypes.length(); ++i) {
        BuildTypeSource(cx, fninfo->mArgTypes[i], true, result);
        if (i != fninfo->mArgTypes.length() - 1 ||
            fninfo->mIsVariadic)
          AppendString(result, ", ");
      }
      if (fninfo->mIsVariadic)
        AppendString(result, "\"...\"");
      AppendString(result, "]");
    }

    AppendString(result, ")");
    break;
  }
  case TYPE_array: {
    // Recursively build the source string, and append '.array(n)',
    // where n is the array length, or the empty string if the array length
    // is undefined.
    JSObject* baseType = ArrayType::GetBaseType(typeObj);
    BuildTypeSource(cx, baseType, makeShort, result);
    AppendString(result, ".array(");

    size_t length;
    if (ArrayType::GetSafeLength(typeObj, &length))
      IntegerToString(length, 10, result);

    AppendString(result, ")");
    break;
  }
  case TYPE_struct: {
    JSString* name = CType::GetName(cx, typeObj);

    if (makeShort) {
      // Shorten the type declaration by assuming that StructType 't' is bound
      // to an in-scope variable of name 't.name'.
      AppendString(result, name);
      break;
    }

    // Write the full struct declaration.
    AppendString(result, "ctypes.StructType(\"");
    AppendString(result, name);
    AppendString(result, "\"");

    // If it's an opaque struct, we're done.
    if (!CType::IsSizeDefined(typeObj)) {
      AppendString(result, ")");
      break;
    }

    AppendString(result, ", [");

    const FieldInfoHash* fields = StructType::GetFieldInfo(typeObj);
    size_t length = fields->count();
    Array<const FieldInfoHash::Entry*, 64> fieldsArray;
    if (!fieldsArray.resize(length))
      break;

    for (FieldInfoHash::Range r = fields->all(); !r.empty(); r.popFront())
      fieldsArray[r.front().value.mIndex] = &r.front();

    for (size_t i = 0; i < length; ++i) {
      const FieldInfoHash::Entry* entry = fieldsArray[i];
      AppendString(result, "{ \"");
      AppendString(result, entry->key);
      AppendString(result, "\": ");
      BuildTypeSource(cx, entry->value.mType, true, result);
      AppendString(result, " }");
      if (i != length - 1)
        AppendString(result, ", ");
    }

    AppendString(result, "])");
    break;
  }
  }
}

// Given a CData object of CType 'typeObj' with binary value 'data', generate a
// string 'result' such that 'eval(result)' would construct a CData object with
// the same CType and containing the same binary value. This assumes that any
// StructType 't' is bound to an in-scope variable of name 't.name'. (This means
// the type comparison function CType::TypesEqual will return true when
// comparing the types, since struct equality is determined by strict JSObject
// pointer equality.) Further, if 'isImplicit' is true, ensure that the
// resulting string can ImplicitConvert successfully if passed to another data
// constructor. (This is important when called recursively, since fields of
// structs and arrays are converted with ImplicitConvert.)
static JSBool
BuildDataSource(JSContext* cx,
                HandleObject typeObj, 
                void* data, 
                bool isImplicit, 
                AutoString& result)
{
  TypeCode type = CType::GetTypeCode(typeObj);
  switch (type) {
  case TYPE_bool:
    if (*static_cast<bool*>(data))
      AppendString(result, "true");
    else
      AppendString(result, "false");
    break;
#define DEFINE_INT_TYPE(name, type, ffiType)                                   \
  case TYPE_##name:                                                            \
    /* Serialize as a primitive decimal integer. */                            \
    IntegerToString(*static_cast<type*>(data), 10, result);                    \
    break;
#define DEFINE_WRAPPED_INT_TYPE(name, type, ffiType)                           \
  case TYPE_##name:                                                            \
    /* Serialize as a wrapped decimal integer. */                              \
    if (!numeric_limits<type>::is_signed)                                      \
      AppendString(result, "ctypes.UInt64(\"");                                \
    else                                                                       \
      AppendString(result, "ctypes.Int64(\"");                                 \
                                                                               \
    IntegerToString(*static_cast<type*>(data), 10, result);                    \
    AppendString(result, "\")");                                               \
    break;
#define DEFINE_FLOAT_TYPE(name, type, ffiType)                                 \
  case TYPE_##name: {                                                          \
    /* Serialize as a primitive double. */                                     \
    double fp = *static_cast<type*>(data);                                     \
    ToCStringBuf cbuf;                                                         \
    char* str = NumberToCString(cx, &cbuf, fp);                                \
    if (!str) {                                                                \
      JS_ReportOutOfMemory(cx);                                                \
      return false;                                                            \
    }                                                                          \
                                                                               \
    result.append(str, strlen(str));                                           \
    break;                                                                     \
  }
#define DEFINE_CHAR_TYPE(name, type, ffiType)                                  \
  case TYPE_##name:                                                            \
    /* Serialize as an integer. */                                             \
    IntegerToString(*static_cast<type*>(data), 10, result);                    \
    break;
#include "typedefs.h"
  case TYPE_jschar: {
    // Serialize as a 1-character JS string.
    JSString* str = JS_NewUCStringCopyN(cx, static_cast<jschar*>(data), 1);
    if (!str)
      return false;

    // Escape characters, and quote as necessary.
    JSString* src = JS_ValueToSource(cx, STRING_TO_JSVAL(str));
    if (!src)
      return false;

    AppendString(result, src);
    break;
  }
  case TYPE_pointer:
  case TYPE_function: {
    if (isImplicit) {
      // The result must be able to ImplicitConvert successfully.
      // Wrap in a type constructor, then serialize for ExplicitConvert.
      BuildTypeSource(cx, typeObj, true, result);
      AppendString(result, "(");
    }

    // Serialize the pointer value as a wrapped hexadecimal integer.
    uintptr_t ptr = *static_cast<uintptr_t*>(data);
    AppendString(result, "ctypes.UInt64(\"0x");
    IntegerToString(ptr, 16, result);
    AppendString(result, "\")");

    if (isImplicit)
      AppendString(result, ")");

    break;
  }
  case TYPE_array: {
    // Serialize each element of the array recursively. Each element must
    // be able to ImplicitConvert successfully.
    RootedObject baseType(cx, ArrayType::GetBaseType(typeObj));
    AppendString(result, "[");

    size_t length = ArrayType::GetLength(typeObj);
    size_t elementSize = CType::GetSize(baseType);
    for (size_t i = 0; i < length; ++i) {
      char* element = static_cast<char*>(data) + elementSize * i;
      if (!BuildDataSource(cx, baseType, element, true, result))
        return false;

      if (i + 1 < length)
        AppendString(result, ", ");
    }
    AppendString(result, "]");
    break;
  }
  case TYPE_struct: {
    if (isImplicit) {
      // The result must be able to ImplicitConvert successfully.
      // Serialize the data as an object with properties, rather than
      // a sequence of arguments to the StructType constructor.
      AppendString(result, "{");
    }

    // Serialize each field of the struct recursively. Each field must
    // be able to ImplicitConvert successfully.
    const FieldInfoHash* fields = StructType::GetFieldInfo(typeObj);
    size_t length = fields->count();
    Array<const FieldInfoHash::Entry*, 64> fieldsArray;
    if (!fieldsArray.resize(length))
      return false;

    for (FieldInfoHash::Range r = fields->all(); !r.empty(); r.popFront())
      fieldsArray[r.front().value.mIndex] = &r.front();

    for (size_t i = 0; i < length; ++i) {
      const FieldInfoHash::Entry* entry = fieldsArray[i];

      if (isImplicit) {
        AppendString(result, "\"");
        AppendString(result, entry->key);
        AppendString(result, "\": ");
      }

      char* fieldData = static_cast<char*>(data) + entry->value.mOffset;
      RootedObject entryType(cx, entry->value.mType);
      if (!BuildDataSource(cx, entryType, fieldData, true, result))
        return false;

      if (i + 1 != length)
        AppendString(result, ", ");
    }

    if (isImplicit)
      AppendString(result, "}");

    break;
  }
  case TYPE_void_t:
    JS_NOT_REACHED("invalid type");
    break;
  }

  return true;
}

/*******************************************************************************
** JSAPI callback function implementations
*******************************************************************************/

JSBool
ConstructAbstract(JSContext* cx,
                  unsigned argc,
                  jsval* vp)
{
  // Calling an abstract base class constructor is disallowed.
  JS_ReportError(cx, "cannot construct from abstract type");
  return JS_FALSE;
}

/*******************************************************************************
** CType implementation
*******************************************************************************/

JSBool
CType::ConstructData(JSContext* cx,
                     unsigned argc,
                     jsval* vp)
{
  // get the callee object...
  RootedObject obj(cx, JSVAL_TO_OBJECT(JS_CALLEE(cx, vp)));
  if (!CType::IsCType(obj)) {
    JS_ReportError(cx, "not a CType");
    return JS_FALSE;
  }

  // How we construct the CData object depends on what type we represent.
  // An instance 'd' of a CData object of type 't' has:
  //   * [[Class]] "CData"
  //   * __proto__ === t.prototype
  switch (GetTypeCode(obj)) {
  case TYPE_void_t:
    JS_ReportError(cx, "cannot construct from void_t");
    return JS_FALSE;
  case TYPE_function:
    JS_ReportError(cx, "cannot construct from FunctionType; use FunctionType.ptr instead");
    return JS_FALSE;
  case TYPE_pointer:
    return PointerType::ConstructData(cx, obj, argc, vp);
  case TYPE_array:
    return ArrayType::ConstructData(cx, obj, argc, vp);
  case TYPE_struct:
    return StructType::ConstructData(cx, obj, argc, vp);
  default:
    return ConstructBasic(cx, obj, argc, vp);
  }
}

JSBool
CType::ConstructBasic(JSContext* cx,
                      HandleObject obj,
                      unsigned argc,
                      jsval* vp)
{
  if (argc > 1) {
    JS_ReportError(cx, "CType constructor takes zero or one argument");
    return JS_FALSE;
  }

  // construct a CData object
  RootedObject result(cx, CData::Create(cx, obj, NullPtr(), NULL, true));
  if (!result)
    return JS_FALSE;

  if (argc == 1) {
    if (!ExplicitConvert(cx, JS_ARGV(cx, vp)[0], obj, CData::GetData(result)))
      return JS_FALSE;
  }

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));
  return JS_TRUE;
}

JSObject*
CType::Create(JSContext* cx,
              HandleObject typeProto,
              HandleObject dataProto,
              TypeCode type,
              JSString* name_,
              jsval size,
              jsval align,
              ffi_type* ffiType)
{
  RootedString name(cx, name_);
  RootedObject parent(cx, JS_GetParent(typeProto));
  JS_ASSERT(parent);

  // Create a CType object with the properties and slots common to all CTypes.
  // Each type object 't' has:
  //   * [[Class]] "CType"
  //   * __proto__ === 'typeProto'; one of ctypes.{CType,PointerType,ArrayType,
  //     StructType}.prototype
  //   * A constructor which creates and returns a CData object, containing
  //     binary data of the given type.
  //   * 'prototype' property:
  //     * [[Class]] "CDataProto"
  //     * __proto__ === 'dataProto'; an object containing properties and
  //       functions common to all CData objects of types derived from
  //       'typeProto'. (For instance, this could be ctypes.CData.prototype
  //       for simple types, or something representing structs for StructTypes.)
  //     * 'constructor' property === 't'
  //     * Additional properties specified by 'ps', as appropriate for the
  //       specific type instance 't'.
  RootedObject typeObj(cx, JS_NewObject(cx, &sCTypeClass, typeProto, parent));
  if (!typeObj)
    return NULL;

  // Set up the reserved slots.
  JS_SetReservedSlot(typeObj, SLOT_TYPECODE, INT_TO_JSVAL(type));
  if (ffiType)
    JS_SetReservedSlot(typeObj, SLOT_FFITYPE, PRIVATE_TO_JSVAL(ffiType));
  if (name)
    JS_SetReservedSlot(typeObj, SLOT_NAME, STRING_TO_JSVAL(name));
  JS_SetReservedSlot(typeObj, SLOT_SIZE, size);
  JS_SetReservedSlot(typeObj, SLOT_ALIGN, align);

  if (dataProto) {
    // Set up the 'prototype' and 'prototype.constructor' properties.
    RootedObject prototype(cx, JS_NewObject(cx, &sCDataProtoClass, dataProto, parent));
    if (!prototype)
      return NULL;

    if (!JS_DefineProperty(cx, prototype, "constructor", OBJECT_TO_JSVAL(typeObj),
           NULL, NULL, JSPROP_READONLY | JSPROP_PERMANENT))
      return NULL;

    // Set the 'prototype' object.
    //if (!JS_FreezeObject(cx, prototype)) // XXX fixme - see bug 541212!
    //  return NULL;
    JS_SetReservedSlot(typeObj, SLOT_PROTO, OBJECT_TO_JSVAL(prototype));
  }

  if (!JS_FreezeObject(cx, typeObj))
    return NULL;

  // Assert a sanity check on size and alignment: size % alignment should always
  // be zero.
  JS_ASSERT_IF(IsSizeDefined(typeObj),
               GetSize(typeObj) % GetAlignment(typeObj) == 0);

  return typeObj;
}

JSObject*
CType::DefineBuiltin(JSContext* cx,
                     JSObject* parent_,
                     const char* propName,
                     JSObject* typeProto_,
                     JSObject* dataProto_,
                     const char* name,
                     TypeCode type,
                     jsval size,
                     jsval align,
                     ffi_type* ffiType)
{
  RootedObject parent(cx, parent_);
  RootedObject typeProto(cx, typeProto_);
  RootedObject dataProto(cx, dataProto_);

  RootedString nameStr(cx, JS_NewStringCopyZ(cx, name));
  if (!nameStr)
    return NULL;

  // Create a new CType object with the common properties and slots.
  RootedObject typeObj(cx, Create(cx, typeProto, dataProto, type, nameStr, size, align, ffiType));
  if (!typeObj)
    return NULL;

  // Define the CType as a 'propName' property on 'parent'.
  if (!JS_DefineProperty(cx, parent, propName, OBJECT_TO_JSVAL(typeObj),
         NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT))
    return NULL;

  return typeObj;
}

void
CType::Finalize(JSFreeOp *fop, JSObject* obj)
{
  // Make sure our TypeCode slot is legit. If it's not, bail.
  jsval slot = JS_GetReservedSlot(obj, SLOT_TYPECODE);
  if (JSVAL_IS_VOID(slot))
    return;

  // The contents of our slots depends on what kind of type we are.
  switch (TypeCode(JSVAL_TO_INT(slot))) {
  case TYPE_function: {
    // Free the FunctionInfo.
    slot = JS_GetReservedSlot(obj, SLOT_FNINFO);
    if (!JSVAL_IS_VOID(slot))
      FreeOp::get(fop)->delete_(static_cast<FunctionInfo*>(JSVAL_TO_PRIVATE(slot)));
    break;
  }

  case TYPE_struct: {
    // Free the FieldInfoHash table.
    slot = JS_GetReservedSlot(obj, SLOT_FIELDINFO);
    if (!JSVAL_IS_VOID(slot)) {
      void* info = JSVAL_TO_PRIVATE(slot);
      FreeOp::get(fop)->delete_(static_cast<FieldInfoHash*>(info));
    }
  }

    // Fall through.
  case TYPE_array: {
    // Free the ffi_type info.
    slot = JS_GetReservedSlot(obj, SLOT_FFITYPE);
    if (!JSVAL_IS_VOID(slot)) {
      ffi_type* ffiType = static_cast<ffi_type*>(JSVAL_TO_PRIVATE(slot));
      FreeOp::get(fop)->array_delete(ffiType->elements);
      FreeOp::get(fop)->delete_(ffiType);
    }

    break;
  }
  default:
    // Nothing to do here.
    break;
  }
}

void
CType::FinalizeProtoClass(JSFreeOp *fop, JSObject* obj)
{
  // Finalize the CTypeProto class. The only important bit here is our
  // SLOT_CLOSURECX -- it contains the JSContext that was (lazily) instantiated
  // for use with FunctionType closures. And if we're here, in this finalizer,
  // we're guaranteed to not need it anymore. Note that this slot will only
  // be set for the object (of class CTypeProto) ctypes.FunctionType.prototype.
  jsval slot = JS_GetReservedSlot(obj, SLOT_CLOSURECX);
  if (JSVAL_IS_VOID(slot))
    return;

  JSContext* closureCx = static_cast<JSContext*>(JSVAL_TO_PRIVATE(slot));
  JS_DestroyContextNoGC(closureCx);
}

void
CType::Trace(JSTracer* trc, JSObject* obj)
{
  // Make sure our TypeCode slot is legit. If it's not, bail.
  jsval slot = obj->getSlot(SLOT_TYPECODE);
  if (JSVAL_IS_VOID(slot))
    return;

  // The contents of our slots depends on what kind of type we are.
  switch (TypeCode(JSVAL_TO_INT(slot))) {
  case TYPE_struct: {
    slot = obj->getReservedSlot(SLOT_FIELDINFO);
    if (JSVAL_IS_VOID(slot))
      return;

    FieldInfoHash* fields =
      static_cast<FieldInfoHash*>(JSVAL_TO_PRIVATE(slot));
    for (FieldInfoHash::Range r = fields->all(); !r.empty(); r.popFront()) {
      JS_CALL_TRACER(trc, r.front().key, JSTRACE_STRING, "fieldName");
      JS_CALL_TRACER(trc, r.front().value.mType, JSTRACE_OBJECT, "fieldType");
    }

    break;
  }
  case TYPE_function: {
    // Check if we have a FunctionInfo.
    slot = obj->getReservedSlot(SLOT_FNINFO);
    if (JSVAL_IS_VOID(slot))
      return;

    FunctionInfo* fninfo = static_cast<FunctionInfo*>(JSVAL_TO_PRIVATE(slot));
    JS_ASSERT(fninfo);

    // Identify our objects to the tracer.
    JS_CALL_TRACER(trc, fninfo->mABI, JSTRACE_OBJECT, "abi");
    JS_CALL_TRACER(trc, fninfo->mReturnType, JSTRACE_OBJECT, "returnType");
    for (size_t i = 0; i < fninfo->mArgTypes.length(); ++i)
      JS_CALL_TRACER(trc, fninfo->mArgTypes[i], JSTRACE_OBJECT, "argType");

    break;
  }
  default:
    // Nothing to do here.
    break;
  }
}

bool
CType::IsCType(JSObject* obj)
{
  return JS_GetClass(obj) == &sCTypeClass;
}

bool
CType::IsCTypeProto(JSObject* obj)
{
  return JS_GetClass(obj) == &sCTypeProtoClass;
}

TypeCode
CType::GetTypeCode(JSObject* typeObj)
{
  JS_ASSERT(IsCType(typeObj));

  jsval result = JS_GetReservedSlot(typeObj, SLOT_TYPECODE);
  return TypeCode(JSVAL_TO_INT(result));
}

bool
CType::TypesEqual(JSObject* t1, JSObject* t2)
{
  JS_ASSERT(IsCType(t1) && IsCType(t2));

  // Fast path: check for object equality.
  if (t1 == t2)
    return true;

  // First, perform shallow comparison.
  TypeCode c1 = GetTypeCode(t1);
  TypeCode c2 = GetTypeCode(t2);
  if (c1 != c2)
    return false;

  // Determine whether the types require shallow or deep comparison.
  switch (c1) {
  case TYPE_pointer: {
    // Compare base types.
    JSObject* b1 = PointerType::GetBaseType(t1);
    JSObject* b2 = PointerType::GetBaseType(t2);
    return TypesEqual(b1, b2);
  }
  case TYPE_function: {
    FunctionInfo* f1 = FunctionType::GetFunctionInfo(t1);
    FunctionInfo* f2 = FunctionType::GetFunctionInfo(t2);

    // Compare abi, return type, and argument types.
    if (f1->mABI != f2->mABI)
      return false;

    if (!TypesEqual(f1->mReturnType, f2->mReturnType))
      return false;

    if (f1->mArgTypes.length() != f2->mArgTypes.length())
      return false;

    if (f1->mIsVariadic != f2->mIsVariadic)
      return false;

    for (size_t i = 0; i < f1->mArgTypes.length(); ++i) {
      if (!TypesEqual(f1->mArgTypes[i], f2->mArgTypes[i]))
        return false;
    }

    return true;
  }
  case TYPE_array: {
    // Compare length, then base types.
    // An undefined length array matches other undefined length arrays.
    size_t s1 = 0, s2 = 0;
    bool d1 = ArrayType::GetSafeLength(t1, &s1);
    bool d2 = ArrayType::GetSafeLength(t2, &s2);
    if (d1 != d2 || (d1 && s1 != s2))
      return false;

    JSObject* b1 = ArrayType::GetBaseType(t1);
    JSObject* b2 = ArrayType::GetBaseType(t2);
    return TypesEqual(b1, b2);
  }
  case TYPE_struct:
    // Require exact type object equality.
    return false;
  default:
    // Shallow comparison is sufficient.
    return true;
  }
}

bool
CType::GetSafeSize(JSObject* obj, size_t* result)
{
  JS_ASSERT(CType::IsCType(obj));

  jsval size = JS_GetReservedSlot(obj, SLOT_SIZE);

  // The "size" property can be an int, a double, or JSVAL_VOID
  // (for arrays of undefined length), and must always fit in a size_t.
  if (JSVAL_IS_INT(size)) {
    *result = JSVAL_TO_INT(size);
    return true;
  }
  if (JSVAL_IS_DOUBLE(size)) {
    *result = Convert<size_t>(JSVAL_TO_DOUBLE(size));
    return true;
  }

  JS_ASSERT(JSVAL_IS_VOID(size));
  return false;
}

size_t
CType::GetSize(JSObject* obj)
{
  JS_ASSERT(CType::IsCType(obj));

  jsval size = JS_GetReservedSlot(obj, SLOT_SIZE);

  JS_ASSERT(!JSVAL_IS_VOID(size));

  // The "size" property can be an int, a double, or JSVAL_VOID
  // (for arrays of undefined length), and must always fit in a size_t.
  // For callers who know it can never be JSVAL_VOID, return a size_t directly.
  if (JSVAL_IS_INT(size))
    return JSVAL_TO_INT(size);
  return Convert<size_t>(JSVAL_TO_DOUBLE(size));
}

bool
CType::IsSizeDefined(JSObject* obj)
{
  JS_ASSERT(CType::IsCType(obj));

  jsval size = JS_GetReservedSlot(obj, SLOT_SIZE);

  // The "size" property can be an int, a double, or JSVAL_VOID
  // (for arrays of undefined length), and must always fit in a size_t.
  JS_ASSERT(JSVAL_IS_INT(size) || JSVAL_IS_DOUBLE(size) || JSVAL_IS_VOID(size));
  return !JSVAL_IS_VOID(size);
}

size_t
CType::GetAlignment(JSObject* obj)
{
  JS_ASSERT(CType::IsCType(obj));

  jsval slot = JS_GetReservedSlot(obj, SLOT_ALIGN);
  return static_cast<size_t>(JSVAL_TO_INT(slot));
}

ffi_type*
CType::GetFFIType(JSContext* cx, JSObject* obj)
{
  JS_ASSERT(CType::IsCType(obj));

  jsval slot = JS_GetReservedSlot(obj, SLOT_FFITYPE);

  if (!JSVAL_IS_VOID(slot)) {
    return static_cast<ffi_type*>(JSVAL_TO_PRIVATE(slot));
  }

  AutoPtr<ffi_type> result;
  switch (CType::GetTypeCode(obj)) {
  case TYPE_array:
    result = ArrayType::BuildFFIType(cx, obj);
    break;

  case TYPE_struct:
    result = StructType::BuildFFIType(cx, obj);
    break;

  default:
    JS_NOT_REACHED("simple types must have an ffi_type");
  }

  if (!result)
    return NULL;
  JS_SetReservedSlot(obj, SLOT_FFITYPE, PRIVATE_TO_JSVAL(result.get()));
  return result.forget();
}

JSString*
CType::GetName(JSContext* cx, JSHandleObject obj)
{
  JS_ASSERT(CType::IsCType(obj));

  jsval string = JS_GetReservedSlot(obj, SLOT_NAME);
  if (!JSVAL_IS_VOID(string))
    return JSVAL_TO_STRING(string);

  // Build the type name lazily.
  JSString* name = BuildTypeName(cx, obj);
  if (!name)
    return NULL;
  JS_SetReservedSlot(obj, SLOT_NAME, STRING_TO_JSVAL(name));
  return name;
}

JSObject*
CType::GetProtoFromCtor(JSObject* obj, CTypeProtoSlot slot)
{
  // Get ctypes.{Pointer,Array,Struct}Type.prototype from a reserved slot
  // on the type constructor.
  jsval protoslot = js::GetFunctionNativeReserved(obj, SLOT_FN_CTORPROTO);
  JSObject* proto = JSVAL_TO_OBJECT(protoslot);
  JS_ASSERT(proto);
  JS_ASSERT(CType::IsCTypeProto(proto));

  // Get the desired prototype.
  jsval result = JS_GetReservedSlot(proto, slot);
  return JSVAL_TO_OBJECT(result);
}

JSObject*
CType::GetProtoFromType(JSObject* obj, CTypeProtoSlot slot)
{
  JS_ASSERT(IsCType(obj));

  // Get the prototype of the type object.
  JSObject* proto = JS_GetPrototype(obj);
  JS_ASSERT(proto);
  JS_ASSERT(CType::IsCTypeProto(proto));

  // Get the requested ctypes.{Pointer,Array,Struct,Function}Type.prototype.
  jsval result = JS_GetReservedSlot(proto, slot);
  return JSVAL_TO_OBJECT(result);
}

JSBool
CType::PrototypeGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp)
{
  if (!(CType::IsCType(obj) || CType::IsCTypeProto(obj))) {
    JS_ReportError(cx, "not a CType or CTypeProto");
    return JS_FALSE;
  }

  unsigned slot = CType::IsCTypeProto(obj) ? (unsigned) SLOT_OURDATAPROTO
                                           : (unsigned) SLOT_PROTO;
  vp.set(JS_GetReservedSlot(obj, slot));
  JS_ASSERT(!JSVAL_IS_PRIMITIVE(vp) || JSVAL_IS_VOID(vp));
  return JS_TRUE;
}

JSBool
CType::NameGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp)
{
  if (!CType::IsCType(obj)) {
    JS_ReportError(cx, "not a CType");
    return JS_FALSE;
  }

  JSString* name = CType::GetName(cx, obj);
  if (!name)
    return JS_FALSE;

  vp.set(STRING_TO_JSVAL(name));
  return JS_TRUE;
}

JSBool
CType::SizeGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp)
{
  if (!CType::IsCType(obj)) {
    JS_ReportError(cx, "not a CType");
    return JS_FALSE;
  }

  vp.set(JS_GetReservedSlot(obj, SLOT_SIZE));
  JS_ASSERT(JSVAL_IS_NUMBER(vp) || JSVAL_IS_VOID(vp));
  return JS_TRUE;
}

JSBool
CType::PtrGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp)
{
  if (!CType::IsCType(obj)) {
    JS_ReportError(cx, "not a CType");
    return JS_FALSE;
  }

  JSObject* pointerType = PointerType::CreateInternal(cx, obj);
  if (!pointerType)
    return JS_FALSE;

  vp.set(OBJECT_TO_JSVAL(pointerType));
  return JS_TRUE;
}

JSBool
CType::CreateArray(JSContext* cx, unsigned argc, jsval* vp)
{
  RootedObject baseType(cx, JS_THIS_OBJECT(cx, vp));
  if (!baseType)
    return JS_FALSE;
  if (!CType::IsCType(baseType)) {
    JS_ReportError(cx, "not a CType");
    return JS_FALSE;
  }

  // Construct and return a new ArrayType object.
  if (argc > 1) {
    JS_ReportError(cx, "array takes zero or one argument");
    return JS_FALSE;
  }

  // Convert the length argument to a size_t.
  jsval* argv = JS_ARGV(cx, vp);
  size_t length = 0;
  if (argc == 1 && !jsvalToSize(cx, argv[0], false, &length)) {
    JS_ReportError(cx, "argument must be a nonnegative integer");
    return JS_FALSE;
  }

  JSObject* result = ArrayType::CreateInternal(cx, baseType, length, argc == 1);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));
  return JS_TRUE;
}

JSBool
CType::ToString(JSContext* cx, unsigned argc, jsval* vp)
{
  RootedObject obj(cx, JS_THIS_OBJECT(cx, vp));
  if (!obj)
    return JS_FALSE;
  if (!CType::IsCType(obj) && !CType::IsCTypeProto(obj)) {
    JS_ReportError(cx, "not a CType");
    return JS_FALSE;
  }

  // Create the appropriate string depending on whether we're sCTypeClass or
  // sCTypeProtoClass.
  JSString* result;
  if (CType::IsCType(obj)) {
    AutoString type;
    AppendString(type, "type ");
    AppendString(type, GetName(cx, obj));
    result = NewUCString(cx, type);
  }
  else {
    result = JS_NewStringCopyZ(cx, "[CType proto object]");
  }
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, STRING_TO_JSVAL(result));
  return JS_TRUE;
}

JSBool
CType::ToSource(JSContext* cx, unsigned argc, jsval* vp)
{
  JSObject* obj = JS_THIS_OBJECT(cx, vp);
  if (!obj)
    return JS_FALSE;
  if (!CType::IsCType(obj) && !CType::IsCTypeProto(obj))
  {
    JS_ReportError(cx, "not a CType");
    return JS_FALSE;
  }

  // Create the appropriate string depending on whether we're sCTypeClass or
  // sCTypeProtoClass.
  JSString* result;
  if (CType::IsCType(obj)) {
    AutoString source;
    BuildTypeSource(cx, obj, false, source);
    result = NewUCString(cx, source);
  } else {
    result = JS_NewStringCopyZ(cx, "[CType proto object]");
  }
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, STRING_TO_JSVAL(result));
  return JS_TRUE;
}

JSBool
CType::HasInstance(JSContext* cx, JSHandleObject obj, const jsval* v, JSBool* bp)
{
  JS_ASSERT(CType::IsCType(obj));

  jsval slot = JS_GetReservedSlot(obj, SLOT_PROTO);
  JSObject* prototype = JSVAL_TO_OBJECT(slot);
  JS_ASSERT(prototype);
  JS_ASSERT(CData::IsCDataProto(prototype));

  *bp = JS_FALSE;
  if (JSVAL_IS_PRIMITIVE(*v))
    return JS_TRUE;

  JSObject* proto = JSVAL_TO_OBJECT(*v);
  while ((proto = JS_GetPrototype(proto))) {
    if (proto == prototype) {
      *bp = JS_TRUE;
      break;
    }
  }
  return JS_TRUE;
}

static JSObject*
CType::GetGlobalCTypes(JSContext* cx, JSObject* obj)
{
  JS_ASSERT(CType::IsCType(obj));

  JSObject *objTypeProto = JS_GetPrototype(obj);
  if (!objTypeProto) {
  }
  JS_ASSERT(objTypeProto);
  JS_ASSERT(CType::IsCTypeProto(objTypeProto));

  jsval valCTypes = JS_GetReservedSlot(objTypeProto, SLOT_CTYPES);
  JS_ASSERT(!JSVAL_IS_PRIMITIVE(valCTypes));

  return JSVAL_TO_OBJECT(valCTypes);
}

/*******************************************************************************
** ABI implementation
*******************************************************************************/

bool
ABI::IsABI(JSObject* obj)
{
  return JS_GetClass(obj) == &sCABIClass;
}

JSBool
ABI::ToSource(JSContext* cx, unsigned argc, jsval* vp)
{
  if (argc != 0) {
    JS_ReportError(cx, "toSource takes zero arguments");
    return JS_FALSE;
  }
  
  JSObject* obj = JS_THIS_OBJECT(cx, vp);
  if (!obj)
    return JS_FALSE;
  if (!ABI::IsABI(obj)) {
    JS_ReportError(cx, "not an ABI");
    return JS_FALSE;
  }
  
  JSString* result;
  switch (GetABICode(obj)) {
    case ABI_DEFAULT:
      result = JS_NewStringCopyZ(cx, "ctypes.default_abi");
      break;
    case ABI_STDCALL:
      result = JS_NewStringCopyZ(cx, "ctypes.stdcall_abi");
      break;
    case ABI_WINAPI:
      result = JS_NewStringCopyZ(cx, "ctypes.winapi_abi");
      break;
    default:
      JS_ReportError(cx, "not a valid ABICode");
      return JS_FALSE;
  }
  if (!result)
    return JS_FALSE;
  
  JS_SET_RVAL(cx, vp, STRING_TO_JSVAL(result));
  return JS_TRUE;
}


/*******************************************************************************
** PointerType implementation
*******************************************************************************/

JSBool
PointerType::Create(JSContext* cx, unsigned argc, jsval* vp)
{
  // Construct and return a new PointerType object.
  if (argc != 1) {
    JS_ReportError(cx, "PointerType takes one argument");
    return JS_FALSE;
  }

  jsval arg = JS_ARGV(cx, vp)[0];
  RootedObject obj(cx);
  if (JSVAL_IS_PRIMITIVE(arg) || !CType::IsCType(obj = JSVAL_TO_OBJECT(arg))) {
    JS_ReportError(cx, "first argument must be a CType");
    return JS_FALSE;
  }

  JSObject* result = CreateInternal(cx, obj);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));
  return JS_TRUE;
}

JSObject*
PointerType::CreateInternal(JSContext* cx, HandleObject baseType)
{
  // check if we have a cached PointerType on our base CType.
  jsval slot = JS_GetReservedSlot(baseType, SLOT_PTR);
  if (!JSVAL_IS_VOID(slot))
    return JSVAL_TO_OBJECT(slot);

  // Get ctypes.PointerType.prototype and the common prototype for CData objects
  // of this type, or ctypes.FunctionType.prototype for function pointers.
  CTypeProtoSlot slotId = CType::GetTypeCode(baseType) == TYPE_function ?
    SLOT_FUNCTIONDATAPROTO : SLOT_POINTERDATAPROTO;
  RootedObject dataProto(cx, CType::GetProtoFromType(baseType, slotId));
  RootedObject typeProto(cx, CType::GetProtoFromType(baseType, SLOT_POINTERPROTO));

  // Create a new CType object with the common properties and slots.
  JSObject* typeObj = CType::Create(cx, typeProto, dataProto, TYPE_pointer,
                        NULL, INT_TO_JSVAL(sizeof(void*)),
                        INT_TO_JSVAL(ffi_type_pointer.alignment),
                        &ffi_type_pointer);
  if (!typeObj)
    return NULL;

  // Set the target type. (This will be 'null' for an opaque pointer type.)
  JS_SetReservedSlot(typeObj, SLOT_TARGET_T, OBJECT_TO_JSVAL(baseType));

  // Finally, cache our newly-created PointerType on our pointed-to CType.
  JS_SetReservedSlot(baseType, SLOT_PTR, OBJECT_TO_JSVAL(typeObj));

  return typeObj;
}

JSBool
PointerType::ConstructData(JSContext* cx,
                           JSHandleObject obj,
                           unsigned argc,
                           jsval* vp)
{
  if (!CType::IsCType(obj) || CType::GetTypeCode(obj) != TYPE_pointer) {
    JS_ReportError(cx, "not a PointerType");
    return JS_FALSE;
  }

  if (argc > 3) {
    JS_ReportError(cx, "constructor takes 0, 1, 2, or 3 arguments");
    return JS_FALSE;
  }

  RootedObject result(cx, CData::Create(cx, obj, NullPtr(), NULL, true));
  if (!result)
    return JS_FALSE;

  // Set return value early, must not observe *vp after
  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));

  // There are 3 things that we might be creating here:
  // 1 - A null pointer (no arguments)
  // 2 - An initialized pointer (1 argument)
  // 3 - A closure (1-3 arguments)
  //
  // The API doesn't give us a perfect way to distinguish 2 and 3, but the
  // heuristics we use should be fine.

  //
  // Case 1 - Null pointer
  //
  if (argc == 0)
    return JS_TRUE;

  // Analyze the arguments a bit to decide what to do next.
  jsval* argv = JS_ARGV(cx, vp);
  RootedObject baseObj(cx, PointerType::GetBaseType(obj));
  bool looksLikeClosure = CType::GetTypeCode(baseObj) == TYPE_function &&
                          argv[0].isObject() &&
                          JS_ObjectIsCallable(cx, &argv[0].toObject());

  //
  // Case 2 - Initialized pointer
  //
  if (!looksLikeClosure) {
    if (argc != 1) {
      JS_ReportError(cx, "first argument must be a function");
      return JS_FALSE;
    }
    return ExplicitConvert(cx, argv[0], obj, CData::GetData(result));
  }

  //
  // Case 3 - Closure
  //

  // The second argument is an optional 'this' parameter with which to invoke
  // the given js function. Callers may leave this blank, or pass null if they
  // wish to pass the third argument.
  RootedObject thisObj(cx, NULL);
  if (argc >= 2) {
    if (JSVAL_IS_NULL(argv[1])) {
      thisObj = NULL;
    } else if (!JSVAL_IS_PRIMITIVE(argv[1])) {
      thisObj = JSVAL_TO_OBJECT(argv[1]);
    } else if (!JS_ValueToObject(cx, argv[1], thisObj.address())) {
      return JS_FALSE;
    }
  }

  // The third argument is an optional error sentinel that js-ctypes will return
  // if an exception is raised while executing the closure. The type must match
  // the return type of the callback.
  jsval errVal = JSVAL_VOID;
  if (argc == 3)
    errVal = argv[2];

  RootedObject fnObj(cx, JSVAL_TO_OBJECT(argv[0]));
  return FunctionType::ConstructData(cx, baseObj, result, fnObj, thisObj, errVal);
}

JSObject*
PointerType::GetBaseType(JSObject* obj)
{
  JS_ASSERT(CType::GetTypeCode(obj) == TYPE_pointer);

  jsval type = JS_GetReservedSlot(obj, SLOT_TARGET_T);
  JS_ASSERT(!JSVAL_IS_NULL(type));
  return JSVAL_TO_OBJECT(type);
}

JSBool
PointerType::TargetTypeGetter(JSContext* cx,
                              JSHandleObject obj,
                              JSHandleId idval,
                              MutableHandleValue vp)
{
  if (!CType::IsCType(obj) || CType::GetTypeCode(obj) != TYPE_pointer) {
    JS_ReportError(cx, "not a PointerType");
    return JS_FALSE;
  }

  vp.set(JS_GetReservedSlot(obj, SLOT_TARGET_T));
  JS_ASSERT(vp.isObject());
  return JS_TRUE;
}

JSBool
PointerType::IsNull(JSContext* cx, unsigned argc, jsval* vp)
{
  JSObject* obj = JS_THIS_OBJECT(cx, vp);
  if (!obj)
    return JS_FALSE;
  if (!CData::IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  // Get pointer type and base type.
  JSObject* typeObj = CData::GetCType(obj);
  if (CType::GetTypeCode(typeObj) != TYPE_pointer) {
    JS_ReportError(cx, "not a PointerType");
    return JS_FALSE;
  }

  void* data = *static_cast<void**>(CData::GetData(obj));
  jsval result = BOOLEAN_TO_JSVAL(data == NULL);
  JS_SET_RVAL(cx, vp, result);
  return JS_TRUE;
}

JSBool
PointerType::OffsetBy(JSContext* cx, int offset, jsval* vp)
{
  JSObject* obj = JS_THIS_OBJECT(cx, vp);
  if (!obj)
    return JS_FALSE;
  if (!CData::IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  RootedObject typeObj(cx, CData::GetCType(obj));
  if (CType::GetTypeCode(typeObj) != TYPE_pointer) {
    JS_ReportError(cx, "not a PointerType");
    return JS_FALSE;
  }

  RootedObject baseType(cx, PointerType::GetBaseType(typeObj));
  if (!CType::IsSizeDefined(baseType)) {
    JS_ReportError(cx, "cannot modify pointer of undefined size");
    return JS_FALSE;
  }

  size_t elementSize = CType::GetSize(baseType);
  char* data = static_cast<char*>(*static_cast<void**>(CData::GetData(obj)));
  void* address = data + offset * elementSize;

  // Create a PointerType CData object containing the new address.
  JSObject* result = CData::Create(cx, typeObj, NullPtr(), &address, true);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));
  return JS_TRUE;
}

JSBool
PointerType::Increment(JSContext* cx, unsigned argc, jsval* vp)
{
  return OffsetBy(cx, 1, vp);
}

JSBool
PointerType::Decrement(JSContext* cx, unsigned argc, jsval* vp)
{
  return OffsetBy(cx, -1, vp);
}

JSBool
PointerType::ContentsGetter(JSContext* cx,
                            JSHandleObject obj,
                            JSHandleId idval,
                            MutableHandleValue vp)
{
  if (!CData::IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  // Get pointer type and base type.
  JSObject* typeObj = CData::GetCType(obj);
  if (CType::GetTypeCode(typeObj) != TYPE_pointer) {
    JS_ReportError(cx, "not a PointerType");
    return JS_FALSE;
  }

  RootedObject baseType(cx, GetBaseType(typeObj));
  if (!CType::IsSizeDefined(baseType)) {
    JS_ReportError(cx, "cannot get contents of undefined size");
    return JS_FALSE;
  }

  void* data = *static_cast<void**>(CData::GetData(obj));
  if (data == NULL) {
    JS_ReportError(cx, "cannot read contents of null pointer");
    return JS_FALSE;
  }

  jsval result;
  if (!ConvertToJS(cx, baseType, NullPtr(), data, false, false, &result))
    return JS_FALSE;

  JS_SET_RVAL(cx, vp.address(), result);
  return JS_TRUE;
}

JSBool
PointerType::ContentsSetter(JSContext* cx,
                            JSHandleObject obj,
                            JSHandleId idval,
                            JSBool strict,
                            MutableHandleValue vp)
{
  if (!CData::IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  // Get pointer type and base type.
  JSObject* typeObj = CData::GetCType(obj);
  if (CType::GetTypeCode(typeObj) != TYPE_pointer) {
    JS_ReportError(cx, "not a PointerType");
    return JS_FALSE;
  }

  JSObject* baseType = GetBaseType(typeObj);
  if (!CType::IsSizeDefined(baseType)) {
    JS_ReportError(cx, "cannot set contents of undefined size");
    return JS_FALSE;
  }

  void* data = *static_cast<void**>(CData::GetData(obj));
  if (data == NULL) {
    JS_ReportError(cx, "cannot write contents to null pointer");
    return JS_FALSE;
  }

  return ImplicitConvert(cx, vp, baseType, data, false, NULL);
}

/*******************************************************************************
** ArrayType implementation
*******************************************************************************/

JSBool
ArrayType::Create(JSContext* cx, unsigned argc, jsval* vp)
{
  // Construct and return a new ArrayType object.
  if (argc < 1 || argc > 2) {
    JS_ReportError(cx, "ArrayType takes one or two arguments");
    return JS_FALSE;
  }

  jsval* argv = JS_ARGV(cx, vp);
  if (JSVAL_IS_PRIMITIVE(argv[0]) ||
      !CType::IsCType(JSVAL_TO_OBJECT(argv[0]))) {
    JS_ReportError(cx, "first argument must be a CType");
    return JS_FALSE;
  }

  // Convert the length argument to a size_t.
  size_t length = 0;
  if (argc == 2 && !jsvalToSize(cx, argv[1], false, &length)) {
    JS_ReportError(cx, "second argument must be a nonnegative integer");
    return JS_FALSE;
  }

  RootedObject baseType(cx, JSVAL_TO_OBJECT(argv[0]));
  JSObject* result = CreateInternal(cx, baseType, length, argc == 2);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));
  return JS_TRUE;
}

JSObject*
ArrayType::CreateInternal(JSContext* cx,
                          HandleObject baseType,
                          size_t length,
                          bool lengthDefined)
{
  // Get ctypes.ArrayType.prototype and the common prototype for CData objects
  // of this type, from ctypes.CType.prototype.
  RootedObject typeProto(cx, CType::GetProtoFromType(baseType, SLOT_ARRAYPROTO));
  RootedObject dataProto(cx, CType::GetProtoFromType(baseType, SLOT_ARRAYDATAPROTO));

  // Determine the size of the array from the base type, if possible.
  // The size of the base type must be defined.
  // If our length is undefined, both our size and length will be undefined.
  size_t baseSize;
  if (!CType::GetSafeSize(baseType, &baseSize)) {
    JS_ReportError(cx, "base size must be defined");
    return NULL;
  }

  jsval sizeVal = JSVAL_VOID;
  jsval lengthVal = JSVAL_VOID;
  if (lengthDefined) {
    // Check for overflow, and convert to an int or double as required.
    size_t size = length * baseSize;
    if (length > 0 && size / length != baseSize) {
      JS_ReportError(cx, "size overflow");
      return NULL;
    }
    if (!SizeTojsval(cx, size, &sizeVal) ||
        !SizeTojsval(cx, length, &lengthVal))
      return NULL;
  }

  size_t align = CType::GetAlignment(baseType);

  // Create a new CType object with the common properties and slots.
  JSObject* typeObj = CType::Create(cx, typeProto, dataProto, TYPE_array, NULL,
                        sizeVal, INT_TO_JSVAL(align), NULL);
  if (!typeObj)
    return NULL;

  // Set the element type.
  JS_SetReservedSlot(typeObj, SLOT_ELEMENT_T, OBJECT_TO_JSVAL(baseType));

  // Set the length.
  JS_SetReservedSlot(typeObj, SLOT_LENGTH, lengthVal);

  return typeObj;
}

JSBool
ArrayType::ConstructData(JSContext* cx,
                         JSHandleObject obj_,
                         unsigned argc,
                         jsval* vp)
{
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject obj(cx, obj_); // Make a mutable version

  if (!CType::IsCType(obj) || CType::GetTypeCode(obj) != TYPE_array) {
    JS_ReportError(cx, "not an ArrayType");
    return JS_FALSE;
  }

  // Decide whether we have an object to initialize from. We'll override this
  // if we get a length argument instead.
  bool convertObject = argc == 1;

  // Check if we're an array of undefined length. If we are, allow construction
  // with a length argument, or with an actual JS array.
  if (CType::IsSizeDefined(obj)) {
    if (argc > 1) {
      JS_ReportError(cx, "constructor takes zero or one argument");
      return JS_FALSE;
    }

  } else {
    if (argc != 1) {
      JS_ReportError(cx, "constructor takes one argument");
      return JS_FALSE;
    }

    RootedObject baseType(cx, GetBaseType(obj));

    size_t length;
    if (jsvalToSize(cx, args[0], false, &length)) {
      // Have a length, rather than an object to initialize from.
      convertObject = false;

    } else if (!JSVAL_IS_PRIMITIVE(args[0])) {
      // We were given an object with a .length property.
      // This could be a JS array, or a CData array.
      RootedObject arg(cx, JSVAL_TO_OBJECT(args[0]));
      js::AutoValueRooter lengthVal(cx);
      if (!JS_GetProperty(cx, arg, "length", lengthVal.jsval_addr()) ||
          !jsvalToSize(cx, lengthVal.jsval_value(), false, &length)) {
        JS_ReportError(cx, "argument must be an array object or length");
        return JS_FALSE;
      }

    } else if (JSVAL_IS_STRING(args[0])) {
      // We were given a string. Size the array to the appropriate length,
      // including space for the terminator.
      JSString* sourceString = JSVAL_TO_STRING(args[0]);
      size_t sourceLength = sourceString->length();
      const jschar* sourceChars = sourceString->getChars(cx);
      if (!sourceChars)
        return false;

      switch (CType::GetTypeCode(baseType)) {
      case TYPE_char:
      case TYPE_signed_char:
      case TYPE_unsigned_char: {
        // Determine the UTF-8 length.
        length = GetDeflatedUTF8StringLength(cx, sourceChars, sourceLength);
        if (length == (size_t) -1)
          return false;

        ++length;
        break;
      }
      case TYPE_jschar:
        length = sourceLength + 1;
        break;
      default:
        return TypeError(cx, "array", args[0]);
      }

    } else {
      JS_ReportError(cx, "argument must be an array object or length");
      return JS_FALSE;
    }

    // Construct a new ArrayType of defined length, for the new CData object.
    obj = CreateInternal(cx, baseType, length, true);
    if (!obj)
      return JS_FALSE;
  }

  // Root the CType object, in case we created one above.
  js::AutoObjectRooter root(cx, obj);

  JSObject* result = CData::Create(cx, obj, NullPtr(), NULL, true);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));

  if (convertObject) {
    if (!ExplicitConvert(cx, args[0], obj, CData::GetData(result)))
      return JS_FALSE;
  }

  return JS_TRUE;
}

JSObject*
ArrayType::GetBaseType(JSObject* obj)
{
  JS_ASSERT(CType::IsCType(obj));
  JS_ASSERT(CType::GetTypeCode(obj) == TYPE_array);

  jsval type = JS_GetReservedSlot(obj, SLOT_ELEMENT_T);
  JS_ASSERT(!JSVAL_IS_NULL(type));
  return JSVAL_TO_OBJECT(type);
}

bool
ArrayType::GetSafeLength(JSObject* obj, size_t* result)
{
  JS_ASSERT(CType::IsCType(obj));
  JS_ASSERT(CType::GetTypeCode(obj) == TYPE_array);

  jsval length = JS_GetReservedSlot(obj, SLOT_LENGTH);

  // The "length" property can be an int, a double, or JSVAL_VOID
  // (for arrays of undefined length), and must always fit in a size_t.
  if (JSVAL_IS_INT(length)) {
    *result = JSVAL_TO_INT(length);
    return true;
  }
  if (JSVAL_IS_DOUBLE(length)) {
    *result = Convert<size_t>(JSVAL_TO_DOUBLE(length));
    return true;
  }

  JS_ASSERT(JSVAL_IS_VOID(length));
  return false;
}

size_t
ArrayType::GetLength(JSObject* obj)
{
  JS_ASSERT(CType::IsCType(obj));
  JS_ASSERT(CType::GetTypeCode(obj) == TYPE_array);

  jsval length = JS_GetReservedSlot(obj, SLOT_LENGTH);

  JS_ASSERT(!JSVAL_IS_VOID(length));

  // The "length" property can be an int, a double, or JSVAL_VOID
  // (for arrays of undefined length), and must always fit in a size_t.
  // For callers who know it can never be JSVAL_VOID, return a size_t directly.
  if (JSVAL_IS_INT(length))
    return JSVAL_TO_INT(length);
  return Convert<size_t>(JSVAL_TO_DOUBLE(length));
}

ffi_type*
ArrayType::BuildFFIType(JSContext* cx, JSObject* obj)
{
  JS_ASSERT(CType::IsCType(obj));
  JS_ASSERT(CType::GetTypeCode(obj) == TYPE_array);
  JS_ASSERT(CType::IsSizeDefined(obj));

  JSObject* baseType = ArrayType::GetBaseType(obj);
  ffi_type* ffiBaseType = CType::GetFFIType(cx, baseType);
  if (!ffiBaseType)
    return NULL;

  size_t length = ArrayType::GetLength(obj);

  // Create an ffi_type to represent the array. This is necessary for the case
  // where the array is part of a struct. Since libffi has no intrinsic
  // support for array types, we approximate it by creating a struct type
  // with elements of type 'baseType' and with appropriate size and alignment
  // values. It would be nice to not do all the work of setting up 'elements',
  // but some libffi platforms currently require that it be meaningful. I'm
  // looking at you, x86_64.
  AutoPtr<ffi_type> ffiType(cx->new_<ffi_type>());
  if (!ffiType) {
    JS_ReportOutOfMemory(cx);
    return NULL;
  }

  ffiType->type = FFI_TYPE_STRUCT;
  ffiType->size = CType::GetSize(obj);
  ffiType->alignment = CType::GetAlignment(obj);
  ffiType->elements = cx->array_new<ffi_type*>(length + 1);
  if (!ffiType->elements) {
    JS_ReportAllocationOverflow(cx);
    return NULL;
  }

  for (size_t i = 0; i < length; ++i)
    ffiType->elements[i] = ffiBaseType;
  ffiType->elements[length] = NULL;

  return ffiType.forget();
}

JSBool
ArrayType::ElementTypeGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp)
{
  if (!CType::IsCType(obj) || CType::GetTypeCode(obj) != TYPE_array) {
    JS_ReportError(cx, "not an ArrayType");
    return JS_FALSE;
  }

  vp.set(JS_GetReservedSlot(obj, SLOT_ELEMENT_T));
  JS_ASSERT(!JSVAL_IS_PRIMITIVE(vp));
  return JS_TRUE;
}

JSBool
ArrayType::LengthGetter(JSContext* cx, JSHandleObject obj_, JSHandleId idval, MutableHandleValue vp)
{
  JSObject *obj = obj_;

  // This getter exists for both CTypes and CDatas of the ArrayType persuasion.
  // If we're dealing with a CData, get the CType from it.
  if (CData::IsCData(obj))
    obj = CData::GetCType(obj);

  if (!CType::IsCType(obj) || CType::GetTypeCode(obj) != TYPE_array) {
    JS_ReportError(cx, "not an ArrayType");
    return JS_FALSE;
  }

  vp.set(JS_GetReservedSlot(obj, SLOT_LENGTH));
  JS_ASSERT(JSVAL_IS_NUMBER(vp) || JSVAL_IS_VOID(vp));
  return JS_TRUE;
}

JSBool
ArrayType::Getter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp)
{
  // This should never happen, but we'll check to be safe.
  if (!CData::IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  // Bail early if we're not an ArrayType. (This setter is present for all
  // CData, regardless of CType.)
  JSObject* typeObj = CData::GetCType(obj);
  if (CType::GetTypeCode(typeObj) != TYPE_array)
    return JS_TRUE;

  // Convert the index to a size_t and bounds-check it.
  size_t index;
  size_t length = GetLength(typeObj);
  bool ok = jsidToSize(cx, idval, true, &index);
  int32_t dummy;
  if (!ok && JSID_IS_STRING(idval) && !StringToInteger(cx, JSID_TO_STRING(idval), &dummy)) {
    // String either isn't a number, or doesn't fit in size_t.
    // Chances are it's a regular property lookup, so return.
    return JS_TRUE;
  }
  if (!ok || index >= length) {
    JS_ReportError(cx, "invalid index");
    return JS_FALSE;
  }

  RootedObject baseType(cx, GetBaseType(typeObj));
  size_t elementSize = CType::GetSize(baseType);
  char* data = static_cast<char*>(CData::GetData(obj)) + elementSize * index;
  return ConvertToJS(cx, baseType, obj, data, false, false, vp.address());
}

JSBool
ArrayType::Setter(JSContext* cx, JSHandleObject obj, JSHandleId idval, JSBool strict, MutableHandleValue vp)
{
  // This should never happen, but we'll check to be safe.
  if (!CData::IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  // Bail early if we're not an ArrayType. (This setter is present for all
  // CData, regardless of CType.)
  JSObject* typeObj = CData::GetCType(obj);
  if (CType::GetTypeCode(typeObj) != TYPE_array)
    return JS_TRUE;

  // Convert the index to a size_t and bounds-check it.
  size_t index;
  size_t length = GetLength(typeObj);
  bool ok = jsidToSize(cx, idval, true, &index);
  int32_t dummy;
  if (!ok && JSID_IS_STRING(idval) && !StringToInteger(cx, JSID_TO_STRING(idval), &dummy)) {
    // String either isn't a number, or doesn't fit in size_t.
    // Chances are it's a regular property lookup, so return.
    return JS_TRUE;
  }
  if (!ok || index >= length) {
    JS_ReportError(cx, "invalid index");
    return JS_FALSE;
  }

  JSObject* baseType = GetBaseType(typeObj);
  size_t elementSize = CType::GetSize(baseType);
  char* data = static_cast<char*>(CData::GetData(obj)) + elementSize * index;
  return ImplicitConvert(cx, vp, baseType, data, false, NULL);
}

JSBool
ArrayType::AddressOfElement(JSContext* cx, unsigned argc, jsval* vp)
{
  JSObject* obj = JS_THIS_OBJECT(cx, vp);
  if (!obj)
    return JS_FALSE;
  if (!CData::IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  RootedObject typeObj(cx, CData::GetCType(obj));
  if (CType::GetTypeCode(typeObj) != TYPE_array) {
    JS_ReportError(cx, "not an ArrayType");
    return JS_FALSE;
  }

  if (argc != 1) {
    JS_ReportError(cx, "addressOfElement takes one argument");
    return JS_FALSE;
  }

  RootedObject baseType(cx, GetBaseType(typeObj));
  RootedObject pointerType(cx, PointerType::CreateInternal(cx, baseType));
  if (!pointerType)
    return JS_FALSE;

  // Create a PointerType CData object containing null.
  JSObject* result = CData::Create(cx, pointerType, NullPtr(), NULL, true);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));

  // Convert the index to a size_t and bounds-check it.
  size_t index;
  size_t length = GetLength(typeObj);
  if (!jsvalToSize(cx, JS_ARGV(cx, vp)[0], false, &index) ||
      index >= length) {
    JS_ReportError(cx, "invalid index");
    return JS_FALSE;
  }

  // Manually set the pointer inside the object, so we skip the conversion step.
  void** data = static_cast<void**>(CData::GetData(result));
  size_t elementSize = CType::GetSize(baseType);
  *data = static_cast<char*>(CData::GetData(obj)) + elementSize * index;
  return JS_TRUE;
}

/*******************************************************************************
** StructType implementation
*******************************************************************************/

// For a struct field descriptor 'val' of the form { name : type }, extract
// 'name' and 'type'.
static JSFlatString*
ExtractStructField(JSContext* cx, jsval val, JSObject** typeObj)
{
  if (JSVAL_IS_PRIMITIVE(val)) {
    JS_ReportError(cx, "struct field descriptors require a valid name and type");
    return NULL;
  }

  RootedObject obj(cx, JSVAL_TO_OBJECT(val));
  RootedObject iter(cx, JS_NewPropertyIterator(cx, obj));
  if (!iter)
    return NULL;
  js::AutoObjectRooter iterroot(cx, iter);

  jsid nameid;
  if (!JS_NextProperty(cx, iter, &nameid))
    return NULL;
  if (JSID_IS_VOID(nameid)) {
    JS_ReportError(cx, "struct field descriptors require a valid name and type");
    return NULL;
  }

  if (!JSID_IS_STRING(nameid)) {
    JS_ReportError(cx, "struct field descriptors require a valid name and type");
    return NULL;
  }

  // make sure we have one, and only one, property
  jsid id;
  if (!JS_NextProperty(cx, iter, &id))
    return NULL;
  if (!JSID_IS_VOID(id)) {
    JS_ReportError(cx, "struct field descriptors must contain one property");
    return NULL;
  }

  js::AutoValueRooter propVal(cx);
  if (!JS_GetPropertyById(cx, obj, nameid, propVal.jsval_addr()))
    return NULL;

  if (propVal.value().isPrimitive() ||
      !CType::IsCType(JSVAL_TO_OBJECT(propVal.jsval_value()))) {
    JS_ReportError(cx, "struct field descriptors require a valid name and type");
    return NULL;
  }

  // Undefined size or zero size struct members are illegal.
  // (Zero-size arrays are legal as struct members in C++, but libffi will
  // choke on a zero-size struct, so we disallow them.)
  *typeObj = JSVAL_TO_OBJECT(propVal.jsval_value());
  size_t size;
  if (!CType::GetSafeSize(*typeObj, &size) || size == 0) {
    JS_ReportError(cx, "struct field types must have defined and nonzero size");
    return NULL;
  }

  return JSID_TO_FLAT_STRING(nameid);
}

// For a struct field with 'name' and 'type', add an element of the form
// { name : type }.
static JSBool
AddFieldToArray(JSContext* cx,
                jsval* element,
                JSFlatString* name_,
                JSObject* typeObj_)
{
  RootedObject typeObj(cx, typeObj_);
  Rooted<JSFlatString*> name(cx, name_);
  RootedObject fieldObj(cx, JS_NewObject(cx, NULL, NULL, NULL));
  if (!fieldObj)
    return false;

  *element = OBJECT_TO_JSVAL(fieldObj);

  if (!JS_DefineUCProperty(cx, fieldObj,
         name->chars(), name->length(),
         OBJECT_TO_JSVAL(typeObj), NULL, NULL,
         JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT))
    return false;

  return JS_FreezeObject(cx, fieldObj);
}

JSBool
StructType::Create(JSContext* cx, unsigned argc, jsval* vp)
{
  CallArgs args = CallArgsFromVp(argc, vp);

  // Construct and return a new StructType object.
  if (argc < 1 || argc > 2) {
    JS_ReportError(cx, "StructType takes one or two arguments");
    return JS_FALSE;
  }

  jsval name = args[0];
  if (!JSVAL_IS_STRING(name)) {
    JS_ReportError(cx, "first argument must be a string");
    return JS_FALSE;
  }

  // Get ctypes.StructType.prototype from the ctypes.StructType constructor.
  RootedObject typeProto(cx, CType::GetProtoFromCtor(&args.callee(), SLOT_STRUCTPROTO));

  // Create a simple StructType with no defined fields. The result will be
  // non-instantiable as CData, will have no 'prototype' property, and will
  // have undefined size and alignment and no ffi_type.
  RootedObject result(cx, CType::Create(cx, typeProto, NullPtr(), TYPE_struct,
                                        JSVAL_TO_STRING(name), JSVAL_VOID, JSVAL_VOID, NULL));
  if (!result)
    return JS_FALSE;

  if (argc == 2) {
    RootedObject arr(cx, JSVAL_IS_PRIMITIVE(args[1]) ? NULL : &args[1].toObject());
    if (!arr || !JS_IsArrayObject(cx, arr)) {
      JS_ReportError(cx, "second argument must be an array");
      return JS_FALSE;
    }

    // Define the struct fields.
    if (!DefineInternal(cx, result, arr))
      return JS_FALSE;
  }

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));
  return JS_TRUE;
}

JSBool
StructType::DefineInternal(JSContext* cx, JSObject* typeObj_, JSObject* fieldsObj_)
{
  RootedObject typeObj(cx, typeObj_);
  RootedObject fieldsObj(cx, fieldsObj_);

  uint32_t len;
  ASSERT_OK(JS_GetArrayLength(cx, fieldsObj, &len));

  // Get the common prototype for CData objects of this type from
  // ctypes.CType.prototype.
  RootedObject dataProto(cx, CType::GetProtoFromType(typeObj, SLOT_STRUCTDATAPROTO));

  // Set up the 'prototype' and 'prototype.constructor' properties.
  // The prototype will reflect the struct fields as properties on CData objects
  // created from this type.
  RootedObject prototype(cx, JS_NewObject(cx, &sCDataProtoClass, dataProto, NULL));
  if (!prototype)
    return JS_FALSE;

  if (!JS_DefineProperty(cx, prototype, "constructor", OBJECT_TO_JSVAL(typeObj),
         NULL, NULL, JSPROP_READONLY | JSPROP_PERMANENT))
    return JS_FALSE;

  // Create a FieldInfoHash to stash on the type object, and an array to root
  // its constituents. (We cannot simply stash the hash in a reserved slot now
  // to get GC safety for free, since if anything in this function fails we
  // do not want to mutate 'typeObj'.)
  AutoPtr<FieldInfoHash> fields(cx->new_<FieldInfoHash>());
  Array<jsval, 16> fieldRootsArray;
  if (!fields || !fields->init(len) || !fieldRootsArray.appendN(JSVAL_VOID, len)) {
    JS_ReportOutOfMemory(cx);
    return JS_FALSE;
  }
  js::AutoArrayRooter fieldRoots(cx, fieldRootsArray.length(), 
    fieldRootsArray.begin());

  // Process the field types.
  size_t structSize, structAlign;
  if (len != 0) {
    structSize = 0;
    structAlign = 0;

    for (uint32_t i = 0; i < len; ++i) {
      js::AutoValueRooter item(cx);
      if (!JS_GetElement(cx, fieldsObj, i, item.jsval_addr()))
        return JS_FALSE;

      RootedObject fieldType(cx, NULL);
      JSFlatString* name = ExtractStructField(cx, item.jsval_value(), fieldType.address());
      if (!name)
        return JS_FALSE;
      fieldRootsArray[i] = OBJECT_TO_JSVAL(fieldType);

      // Make sure each field name is unique, and add it to the hash.
      FieldInfoHash::AddPtr entryPtr = fields->lookupForAdd(name);
      if (entryPtr) {
        JS_ReportError(cx, "struct fields must have unique names");
        return JS_FALSE;
      }
      ASSERT_OK(fields->add(entryPtr, name, FieldInfo()));
      FieldInfo& info = entryPtr->value;
      info.mType = fieldType;
      info.mIndex = i;

      // Add the field to the StructType's 'prototype' property.
      if (!JS_DefineUCProperty(cx, prototype,
             name->chars(), name->length(), JSVAL_VOID,
             StructType::FieldGetter, StructType::FieldSetter,
             JSPROP_SHARED | JSPROP_ENUMERATE | JSPROP_PERMANENT))
        return JS_FALSE;

      size_t fieldSize = CType::GetSize(fieldType);
      size_t fieldAlign = CType::GetAlignment(fieldType);
      size_t fieldOffset = Align(structSize, fieldAlign);
      // Check for overflow. Since we hold invariant that fieldSize % fieldAlign
      // be zero, we can safely check fieldOffset + fieldSize without first
      // checking fieldOffset for overflow.
      if (fieldOffset + fieldSize < structSize) {
        JS_ReportError(cx, "size overflow");
        return JS_FALSE;
      }
      info.mOffset = fieldOffset;
      structSize = fieldOffset + fieldSize;

      if (fieldAlign > structAlign)
        structAlign = fieldAlign;
    }

    // Pad the struct tail according to struct alignment.
    size_t structTail = Align(structSize, structAlign);
    if (structTail < structSize) {
      JS_ReportError(cx, "size overflow");
      return JS_FALSE;
    }
    structSize = structTail;

  } else {
    // Empty structs are illegal in C, but are legal and have a size of
    // 1 byte in C++. We're going to allow them, and trick libffi into
    // believing this by adding a char member. The resulting struct will have
    // no getters or setters, and will be initialized to zero.
    structSize = 1;
    structAlign = 1;
  }

  jsval sizeVal;
  if (!SizeTojsval(cx, structSize, &sizeVal))
    return JS_FALSE;

  JS_SetReservedSlot(typeObj, SLOT_FIELDINFO, PRIVATE_TO_JSVAL(fields.forget()));

  JS_SetReservedSlot(typeObj, SLOT_SIZE, sizeVal);
  JS_SetReservedSlot(typeObj, SLOT_ALIGN, INT_TO_JSVAL(structAlign));
  //if (!JS_FreezeObject(cx, prototype)0 // XXX fixme - see bug 541212!
  //  return false;
  JS_SetReservedSlot(typeObj, SLOT_PROTO, OBJECT_TO_JSVAL(prototype));
  return JS_TRUE;
}

ffi_type*
StructType::BuildFFIType(JSContext* cx, JSObject* obj)
{
  JS_ASSERT(CType::IsCType(obj));
  JS_ASSERT(CType::GetTypeCode(obj) == TYPE_struct);
  JS_ASSERT(CType::IsSizeDefined(obj));

  const FieldInfoHash* fields = GetFieldInfo(obj);
  size_t len = fields->count();

  size_t structSize = CType::GetSize(obj);
  size_t structAlign = CType::GetAlignment(obj);

  AutoPtr<ffi_type> ffiType(cx->new_<ffi_type>());
  if (!ffiType) {
    JS_ReportOutOfMemory(cx);
    return NULL;
  }
  ffiType->type = FFI_TYPE_STRUCT;

  AutoPtr<ffi_type*>::Array elements;
  if (len != 0) {
    elements = cx->array_new<ffi_type*>(len + 1);
    if (!elements) {
      JS_ReportOutOfMemory(cx);
      return NULL;
    }
    elements[len] = NULL;

    for (FieldInfoHash::Range r = fields->all(); !r.empty(); r.popFront()) {
      const FieldInfoHash::Entry& entry = r.front();
      ffi_type* fieldType = CType::GetFFIType(cx, entry.value.mType);
      if (!fieldType)
        return NULL;
      elements[entry.value.mIndex] = fieldType;
    }

  } else {
    // Represent an empty struct as having a size of 1 byte, just like C++.
    JS_ASSERT(structSize == 1);
    JS_ASSERT(structAlign == 1);
    elements = cx->array_new<ffi_type*>(2);
    if (!elements) {
      JS_ReportOutOfMemory(cx);
      return NULL;
    }
    elements[0] = &ffi_type_uint8;
    elements[1] = NULL;
  }

  ffiType->elements = elements.get();

#ifdef DEBUG
  // Perform a sanity check: the result of our struct size and alignment
  // calculations should match libffi's. We force it to do this calculation
  // by calling ffi_prep_cif.
  ffi_cif cif;
  ffiType->size = 0;
  ffiType->alignment = 0;
  ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 0, ffiType.get(), NULL);
  JS_ASSERT(status == FFI_OK);
  JS_ASSERT(structSize == ffiType->size);
  JS_ASSERT(structAlign == ffiType->alignment);
#else
  // Fill in the ffi_type's size and align fields. This makes libffi treat the
  // type as initialized; it will not recompute the values. (We assume
  // everything agrees; if it doesn't, we really want to know about it, which
  // is the purpose of the above debug-only check.)
  ffiType->size = structSize;
  ffiType->alignment = structAlign;
#endif

  elements.forget();
  return ffiType.forget();
}

JSBool
StructType::Define(JSContext* cx, unsigned argc, jsval* vp)
{
  JSObject* obj = JS_THIS_OBJECT(cx, vp);
  if (!obj)
    return JS_FALSE;
  if (!CType::IsCType(obj) ||
      CType::GetTypeCode(obj) != TYPE_struct) {
    JS_ReportError(cx, "not a StructType");
    return JS_FALSE;
  }

  if (CType::IsSizeDefined(obj)) {
    JS_ReportError(cx, "StructType has already been defined");
    return JS_FALSE;
  }

  if (argc != 1) {
    JS_ReportError(cx, "define takes one argument");
    return JS_FALSE;
  }

  jsval arg = JS_ARGV(cx, vp)[0];
  if (JSVAL_IS_PRIMITIVE(arg)) {
    JS_ReportError(cx, "argument must be an array");
    return JS_FALSE;
  }
  RootedObject arr(cx, JSVAL_TO_OBJECT(arg));
  if (!JS_IsArrayObject(cx, arr)) {
    JS_ReportError(cx, "argument must be an array");
    return JS_FALSE;
  }

  return DefineInternal(cx, obj, arr);
}

JSBool
StructType::ConstructData(JSContext* cx,
                          HandleObject obj,
                          unsigned argc,
                          jsval* vp)
{
  if (!CType::IsCType(obj) || CType::GetTypeCode(obj) != TYPE_struct) {
    JS_ReportError(cx, "not a StructType");
    return JS_FALSE;
  }

  if (!CType::IsSizeDefined(obj)) {
    JS_ReportError(cx, "cannot construct an opaque StructType");
    return JS_FALSE;
  }

  JSObject* result = CData::Create(cx, obj, NullPtr(), NULL, true);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));

  if (argc == 0)
    return JS_TRUE;

  char* buffer = static_cast<char*>(CData::GetData(result));
  const FieldInfoHash* fields = GetFieldInfo(obj);

  jsval* argv = JS_ARGV(cx, vp);
  if (argc == 1) {
    // There are two possible interpretations of the argument:
    // 1) It may be an object '{ ... }' with properties representing the
    //    struct fields intended to ExplicitConvert wholesale to our StructType.
    // 2) If the struct contains one field, the arg may be intended to
    //    ImplicitConvert directly to that arg's CType.
    // Thankfully, the conditions for these two possibilities to succeed
    // are mutually exclusive, so we can pick the right one.

    // Try option 1) first.
    if (ExplicitConvert(cx, argv[0], obj, buffer))
      return JS_TRUE;

    if (fields->count() != 1)
      return JS_FALSE;

    // If ExplicitConvert failed, and there is no pending exception, then assume
    // hard failure (out of memory, or some other similarly serious condition).
    if (!JS_IsExceptionPending(cx))
      return JS_FALSE;

    // Otherwise, assume soft failure, and clear the pending exception so that we
    // can throw a different one as required.
    JS_ClearPendingException(cx);

    // Fall through to try option 2).
  }

  // We have a type constructor of the form 'ctypes.StructType(a, b, c, ...)'.
  // ImplicitConvert each field.
  if (argc == fields->count()) {
    for (FieldInfoHash::Range r = fields->all(); !r.empty(); r.popFront()) {
      const FieldInfo& field = r.front().value;
      STATIC_ASSUME(field.mIndex < fields->count());  /* Quantified invariant */
      if (!ImplicitConvert(cx, argv[field.mIndex], field.mType,
             buffer + field.mOffset,
             false, NULL))
        return JS_FALSE;
    }

    return JS_TRUE;
  }

  JS_ReportError(cx, "constructor takes 0, 1, or %u arguments",
    fields->count());
  return JS_FALSE;
}

const FieldInfoHash*
StructType::GetFieldInfo(JSObject* obj)
{
  JS_ASSERT(CType::IsCType(obj));
  JS_ASSERT(CType::GetTypeCode(obj) == TYPE_struct);

  jsval slot = JS_GetReservedSlot(obj, SLOT_FIELDINFO);
  JS_ASSERT(!JSVAL_IS_VOID(slot) && JSVAL_TO_PRIVATE(slot));

  return static_cast<const FieldInfoHash*>(JSVAL_TO_PRIVATE(slot));
}

const FieldInfo*
StructType::LookupField(JSContext* cx, JSObject* obj, JSFlatString *name)
{
  JS_ASSERT(CType::IsCType(obj));
  JS_ASSERT(CType::GetTypeCode(obj) == TYPE_struct);

  FieldInfoHash::Ptr ptr = GetFieldInfo(obj)->lookup(name);
  if (ptr)
    return &ptr->value;

  JSAutoByteString bytes(cx, name);
  if (!bytes)
    return NULL;

  JS_ReportError(cx, "%s does not name a field", bytes.ptr());
  return NULL;
}

JSObject*
StructType::BuildFieldsArray(JSContext* cx, JSObject* obj)
{
  JS_ASSERT(CType::IsCType(obj));
  JS_ASSERT(CType::GetTypeCode(obj) == TYPE_struct);
  JS_ASSERT(CType::IsSizeDefined(obj));

  const FieldInfoHash* fields = GetFieldInfo(obj);
  size_t len = fields->count();

  // Prepare a new array for the 'fields' property of the StructType.
  Array<jsval, 16> fieldsVec;
  if (!fieldsVec.appendN(JSVAL_VOID, len))
    return NULL;
  js::AutoArrayRooter root(cx, fieldsVec.length(), fieldsVec.begin());

  for (FieldInfoHash::Range r = fields->all(); !r.empty(); r.popFront()) {
    const FieldInfoHash::Entry& entry = r.front();
    // Add the field descriptor to the array.
    if (!AddFieldToArray(cx, &fieldsVec[entry.value.mIndex],
                         entry.key, entry.value.mType))
      return NULL;
  }

  RootedObject fieldsProp(cx, JS_NewArrayObject(cx, len, fieldsVec.begin()));
  if (!fieldsProp)
    return NULL;

  // Seal the fields array.
  if (!JS_FreezeObject(cx, fieldsProp))
    return NULL;

  return fieldsProp;
}

JSBool
StructType::FieldsArrayGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp)
{
  if (!CType::IsCType(obj) || CType::GetTypeCode(obj) != TYPE_struct) {
    JS_ReportError(cx, "not a StructType");
    return JS_FALSE;
  }

  vp.set(JS_GetReservedSlot(obj, SLOT_FIELDS));

  if (!CType::IsSizeDefined(obj)) {
    JS_ASSERT(JSVAL_IS_VOID(vp));
    return JS_TRUE;
  }

  if (JSVAL_IS_VOID(vp)) {
    // Build the 'fields' array lazily.
    JSObject* fields = BuildFieldsArray(cx, obj);
    if (!fields)
      return JS_FALSE;
    JS_SetReservedSlot(obj, SLOT_FIELDS, OBJECT_TO_JSVAL(fields));

    vp.set(OBJECT_TO_JSVAL(fields));
  }

  JS_ASSERT(!JSVAL_IS_PRIMITIVE(vp) &&
            JS_IsArrayObject(cx, JSVAL_TO_OBJECT(vp)));
  return JS_TRUE;
}

JSBool
StructType::FieldGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp)
{
  if (!CData::IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  JSObject* typeObj = CData::GetCType(obj);
  if (CType::GetTypeCode(typeObj) != TYPE_struct) {
    JS_ReportError(cx, "not a StructType");
    return JS_FALSE;
  }

  const FieldInfo* field = LookupField(cx, typeObj, JSID_TO_FLAT_STRING(idval));
  if (!field)
    return JS_FALSE;

  char* data = static_cast<char*>(CData::GetData(obj)) + field->mOffset;
  RootedObject fieldType(cx, field->mType);
  return ConvertToJS(cx, fieldType, obj, data, false, false, vp.address());
}

JSBool
StructType::FieldSetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, JSBool strict, MutableHandleValue vp)
{
  if (!CData::IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  JSObject* typeObj = CData::GetCType(obj);
  if (CType::GetTypeCode(typeObj) != TYPE_struct) {
    JS_ReportError(cx, "not a StructType");
    return JS_FALSE;
  }

  const FieldInfo* field = LookupField(cx, typeObj, JSID_TO_FLAT_STRING(idval));
  if (!field)
    return JS_FALSE;

  char* data = static_cast<char*>(CData::GetData(obj)) + field->mOffset;
  return ImplicitConvert(cx, vp, field->mType, data, false, NULL);
}

JSBool
StructType::AddressOfField(JSContext* cx, unsigned argc, jsval* vp)
{
  JSObject* obj = JS_THIS_OBJECT(cx, vp);
  if (!obj)
    return JS_FALSE;
  if (!CData::IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  JSObject* typeObj = CData::GetCType(obj);
  if (CType::GetTypeCode(typeObj) != TYPE_struct) {
    JS_ReportError(cx, "not a StructType");
    return JS_FALSE;
  }

  if (argc != 1) {
    JS_ReportError(cx, "addressOfField takes one argument");
    return JS_FALSE;
  }

  JSFlatString *str = JS_FlattenString(cx, JSVAL_TO_STRING(JS_ARGV(cx, vp)[0]));
  if (!str)
    return JS_FALSE;

  const FieldInfo* field = LookupField(cx, typeObj, str);
  if (!field)
    return JS_FALSE;

  RootedObject baseType(cx, field->mType);
  RootedObject pointerType(cx, PointerType::CreateInternal(cx, baseType));
  if (!pointerType)
    return JS_FALSE;

  // Create a PointerType CData object containing null.
  JSObject* result = CData::Create(cx, pointerType, NullPtr(), NULL, true);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));

  // Manually set the pointer inside the object, so we skip the conversion step.
  void** data = static_cast<void**>(CData::GetData(result));
  *data = static_cast<char*>(CData::GetData(obj)) + field->mOffset;
  return JS_TRUE;
}

/*******************************************************************************
** FunctionType implementation
*******************************************************************************/

// Helper class for handling allocation of function arguments.
struct AutoValue
{
  AutoValue() : mData(NULL) { }

  ~AutoValue()
  {
    UnwantedForeground::array_delete(static_cast<char*>(mData));
  }

  bool SizeToType(JSContext* cx, JSObject* type)
  {
    // Allocate a minimum of sizeof(ffi_arg) to handle small integers.
    size_t size = Align(CType::GetSize(type), sizeof(ffi_arg));
    mData = cx->array_new<char>(size);
    if (mData)
      memset(mData, 0, size);
    return mData != NULL;
  }

  void* mData;
};

static bool
GetABI(JSContext* cx, jsval abiType, ffi_abi* result)
{
  if (JSVAL_IS_PRIMITIVE(abiType))
    return false;

  ABICode abi = GetABICode(JSVAL_TO_OBJECT(abiType));

  // determine the ABI from the subset of those available on the
  // given platform. ABI_DEFAULT specifies the default
  // C calling convention (cdecl) on each platform.
  switch (abi) {
  case ABI_DEFAULT:
    *result = FFI_DEFAULT_ABI;
    return true;
  case ABI_STDCALL:
  case ABI_WINAPI:
#if (defined(_WIN32) && !defined(_WIN64)) || defined(_OS2)
    *result = FFI_STDCALL;
    return true;
#elif (defined(_WIN64))
    // We'd like the same code to work across Win32 and Win64, so stdcall_api
    // and winapi_abi become aliases to the lone Win64 ABI.
    *result = FFI_WIN64;
    return true;
#endif
  case INVALID_ABI:
    break;
  }
  return false;
}

static JSObject*
PrepareType(JSContext* cx, jsval type)
{
  if (JSVAL_IS_PRIMITIVE(type) ||
      !CType::IsCType(JSVAL_TO_OBJECT(type))) {
    JS_ReportError(cx, "not a ctypes type");
    return NULL;
  }

  JSObject* result = JSVAL_TO_OBJECT(type);
  TypeCode typeCode = CType::GetTypeCode(result);

  if (typeCode == TYPE_array) {
    // convert array argument types to pointers, just like C.
    // ImplicitConvert will do the same, when passing an array as data.
    RootedObject baseType(cx, ArrayType::GetBaseType(result));
    result = PointerType::CreateInternal(cx, baseType);
    if (!result)
      return NULL;

  } else if (typeCode == TYPE_void_t || typeCode == TYPE_function) {
    // disallow void or function argument types
    JS_ReportError(cx, "Cannot have void or function argument type");
    return NULL;
  }

  if (!CType::IsSizeDefined(result)) {
    JS_ReportError(cx, "Argument type must have defined size");
    return NULL;
  }

  // libffi cannot pass types of zero size by value.
  JS_ASSERT(CType::GetSize(result) != 0);

  return result;
}

static JSObject*
PrepareReturnType(JSContext* cx, jsval type)
{
  if (JSVAL_IS_PRIMITIVE(type) ||
      !CType::IsCType(JSVAL_TO_OBJECT(type))) {
    JS_ReportError(cx, "not a ctypes type");
    return NULL;
  }

  JSObject* result = JSVAL_TO_OBJECT(type);
  TypeCode typeCode = CType::GetTypeCode(result);

  // Arrays and functions can never be return types.
  if (typeCode == TYPE_array || typeCode == TYPE_function) {
    JS_ReportError(cx, "Return type cannot be an array or function");
    return NULL;
  }

  if (typeCode != TYPE_void_t && !CType::IsSizeDefined(result)) {
    JS_ReportError(cx, "Return type must have defined size");
    return NULL;
  }

  // libffi cannot pass types of zero size by value.
  JS_ASSERT(typeCode == TYPE_void_t || CType::GetSize(result) != 0);

  return result;
}

static JS_ALWAYS_INLINE JSBool
IsEllipsis(JSContext* cx, jsval v, bool* isEllipsis)
{
  *isEllipsis = false;
  if (!JSVAL_IS_STRING(v))
    return true;
  JSString* str = JSVAL_TO_STRING(v);
  if (str->length() != 3)
    return true;
  const jschar* chars = str->getChars(cx);
  if (!chars)
    return false;
  jschar dot = '.';
  *isEllipsis = (chars[0] == dot &&
                 chars[1] == dot &&
                 chars[2] == dot);
  return true;
}

static JSBool
PrepareCIF(JSContext* cx,
           FunctionInfo* fninfo)
{
  ffi_abi abi;
  if (!GetABI(cx, OBJECT_TO_JSVAL(fninfo->mABI), &abi)) {
    JS_ReportError(cx, "Invalid ABI specification");
    return false;
  }

  ffi_type* rtype = CType::GetFFIType(cx, fninfo->mReturnType);
  if (!rtype)
    return false;

  ffi_status status =
    ffi_prep_cif(&fninfo->mCIF,
                 abi,
                 fninfo->mFFITypes.length(),
                 rtype,
                 fninfo->mFFITypes.begin());

  switch (status) {
  case FFI_OK:
    return true;
  case FFI_BAD_ABI:
    JS_ReportError(cx, "Invalid ABI specification");
    return false;
  case FFI_BAD_TYPEDEF:
    JS_ReportError(cx, "Invalid type specification");
    return false;
  default:
    JS_ReportError(cx, "Unknown libffi error");
    return false;
  }
}

void
FunctionType::BuildSymbolName(JSString* name,
                              JSObject* typeObj,
                              AutoCString& result)
{
  FunctionInfo* fninfo = GetFunctionInfo(typeObj);

  switch (GetABICode(fninfo->mABI)) {
  case ABI_DEFAULT:
  case ABI_WINAPI:
    // For cdecl or WINAPI functions, no mangling is necessary.
    AppendString(result, name);
    break;

  case ABI_STDCALL: {
#if (defined(_WIN32) && !defined(_WIN64)) || defined(_OS2)
    // On WIN32, stdcall functions look like:
    //   _foo@40
    // where 'foo' is the function name, and '40' is the aligned size of the
    // arguments.
    AppendString(result, "_");
    AppendString(result, name);
    AppendString(result, "@");

    // Compute the suffix by aligning each argument to sizeof(ffi_arg).
    size_t size = 0;
    for (size_t i = 0; i < fninfo->mArgTypes.length(); ++i) {
      JSObject* argType = fninfo->mArgTypes[i];
      size += Align(CType::GetSize(argType), sizeof(ffi_arg));
    }

    IntegerToString(size, 10, result);
#elif defined(_WIN64)
    // On Win64, stdcall is an alias to the default ABI for compatibility, so no
    // mangling is done.
    AppendString(result, name);
#endif
    break;
  }

  case INVALID_ABI:
    JS_NOT_REACHED("invalid abi");
    break;
  }
}

static FunctionInfo*
NewFunctionInfo(JSContext* cx,
                jsval abiType,
                jsval returnType,
                jsval* argTypes,
                unsigned argLength)
{
  AutoPtr<FunctionInfo> fninfo(cx->new_<FunctionInfo>());
  if (!fninfo) {
    JS_ReportOutOfMemory(cx);
    return NULL;
  }

  ffi_abi abi;
  if (!GetABI(cx, abiType, &abi)) {
    JS_ReportError(cx, "Invalid ABI specification");
    return NULL;
  }
  fninfo->mABI = JSVAL_TO_OBJECT(abiType);

  // prepare the result type
  fninfo->mReturnType = PrepareReturnType(cx, returnType);
  if (!fninfo->mReturnType)
    return NULL;

  // prepare the argument types
  if (!fninfo->mArgTypes.reserve(argLength) ||
      !fninfo->mFFITypes.reserve(argLength)) {
    JS_ReportOutOfMemory(cx);
    return NULL;
  }

  fninfo->mIsVariadic = false;

  for (uint32_t i = 0; i < argLength; ++i) {
    bool isEllipsis;
    if (!IsEllipsis(cx, argTypes[i], &isEllipsis))
      return NULL;
    if (isEllipsis) {
      fninfo->mIsVariadic = true;
      if (i < 1) {
        JS_ReportError(cx, "\"...\" may not be the first and only parameter "
                       "type of a variadic function declaration");
        return NULL;
      }
      if (i < argLength - 1) {
        JS_ReportError(cx, "\"...\" must be the last parameter type of a "
                       "variadic function declaration");
        return NULL;
      }
      if (GetABICode(fninfo->mABI) != ABI_DEFAULT) {
        JS_ReportError(cx, "Variadic functions must use the __cdecl calling "
                       "convention");
        return NULL;
      }
      break;
    }

    JSObject* argType = PrepareType(cx, argTypes[i]);
    if (!argType)
      return NULL;

    ffi_type* ffiType = CType::GetFFIType(cx, argType);
    if (!ffiType)
      return NULL;

    fninfo->mArgTypes.infallibleAppend(argType);
    fninfo->mFFITypes.infallibleAppend(ffiType);
  }

  if (fninfo->mIsVariadic)
    // wait to PrepareCIF until function is called
    return fninfo.forget();

  if (!PrepareCIF(cx, fninfo.get()))
    return NULL;

  return fninfo.forget();
}

JSBool
FunctionType::Create(JSContext* cx, unsigned argc, jsval* vp)
{
  // Construct and return a new FunctionType object.
  if (argc < 2 || argc > 3) {
    JS_ReportError(cx, "FunctionType takes two or three arguments");
    return JS_FALSE;
  }

  jsval* argv = JS_ARGV(cx, vp);
  Array<jsval, 16> argTypes;
  RootedObject arrayObj(cx, NULL);

  if (argc == 3) {
    // Prepare an array of jsvals for the arguments.
    if (!JSVAL_IS_PRIMITIVE(argv[2]))
      arrayObj = JSVAL_TO_OBJECT(argv[2]);
    if (!arrayObj || !JS_IsArrayObject(cx, arrayObj)) {
      JS_ReportError(cx, "third argument must be an array");
      return JS_FALSE;
    }

    uint32_t len;
    ASSERT_OK(JS_GetArrayLength(cx, arrayObj, &len));

    if (!argTypes.appendN(JSVAL_VOID, len)) {
      JS_ReportOutOfMemory(cx);
      return JS_FALSE;
    }
  }

  // Pull out the argument types from the array, if any.
  JS_ASSERT(!argTypes.length() || arrayObj);
  js::AutoArrayRooter items(cx, argTypes.length(), argTypes.begin());
  for (uint32_t i = 0; i < argTypes.length(); ++i) {
    if (!JS_GetElement(cx, arrayObj, i, &argTypes[i]))
      return JS_FALSE;
  }

  JSObject* result = CreateInternal(cx, argv[0], argv[1],
      argTypes.begin(), argTypes.length());
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));
  return JS_TRUE;
}

JSObject*
FunctionType::CreateInternal(JSContext* cx,
                             jsval abi,
                             jsval rtype,
                             jsval* argtypes,
                             unsigned arglen)
{
  // Determine and check the types, and prepare the function CIF.
  AutoPtr<FunctionInfo> fninfo(NewFunctionInfo(cx, abi, rtype, argtypes, arglen));
  if (!fninfo)
    return NULL;

  // Get ctypes.FunctionType.prototype and the common prototype for CData objects
  // of this type, from ctypes.CType.prototype.
  RootedObject typeProto(cx, CType::GetProtoFromType(fninfo->mReturnType,
                                                     SLOT_FUNCTIONPROTO));
  RootedObject dataProto(cx, CType::GetProtoFromType(fninfo->mReturnType,
                                                     SLOT_FUNCTIONDATAPROTO));

  // Create a new CType object with the common properties and slots.
  JSObject* typeObj = CType::Create(cx, typeProto, dataProto, TYPE_function,
                        NULL, JSVAL_VOID, JSVAL_VOID, NULL);
  if (!typeObj)
    return NULL;
  js::AutoObjectRooter root(cx, typeObj);

  // Stash the FunctionInfo in a reserved slot.
  JS_SetReservedSlot(typeObj, SLOT_FNINFO, PRIVATE_TO_JSVAL(fninfo.forget()));

  return typeObj;
}

// Construct a function pointer to a JS function (see CClosure::Create()).
// Regular function pointers are constructed directly in
// PointerType::ConstructData().
JSBool
FunctionType::ConstructData(JSContext* cx,
                            JSHandleObject typeObj,
                            JSHandleObject dataObj,
                            JSHandleObject fnObj,
                            JSHandleObject thisObj,
                            jsval errVal)
{
  JS_ASSERT(CType::GetTypeCode(typeObj) == TYPE_function);

  PRFuncPtr* data = static_cast<PRFuncPtr*>(CData::GetData(dataObj));

  FunctionInfo* fninfo = FunctionType::GetFunctionInfo(typeObj);
  if (fninfo->mIsVariadic) {
    JS_ReportError(cx, "Can't declare a variadic callback function");
    return JS_FALSE;
  }
  if (GetABICode(fninfo->mABI) == ABI_WINAPI) {
    JS_ReportError(cx, "Can't declare a ctypes.winapi_abi callback function, "
                   "use ctypes.stdcall_abi instead");
    return JS_FALSE;
  }

  JSObject* closureObj = CClosure::Create(cx, typeObj, fnObj, thisObj, errVal, data);
  if (!closureObj)
    return JS_FALSE;
  js::AutoObjectRooter root(cx, closureObj);

  // Set the closure object as the referent of the new CData object.
  JS_SetReservedSlot(dataObj, SLOT_REFERENT, OBJECT_TO_JSVAL(closureObj));

  // Seal the CData object, to prevent modification of the function pointer.
  // This permanently associates this object with the closure, and avoids
  // having to do things like reset SLOT_REFERENT when someone tries to
  // change the pointer value.
  // XXX This will need to change when bug 541212 is fixed -- CData::ValueSetter
  // could be called on a frozen object.
  return JS_FreezeObject(cx, dataObj);
}

typedef Array<AutoValue, 16> AutoValueAutoArray;

static JSBool
ConvertArgument(JSContext* cx,
                jsval arg,
                JSObject* type,
                AutoValue* value,
                AutoValueAutoArray* strings)
{
  if (!value->SizeToType(cx, type)) {
    JS_ReportAllocationOverflow(cx);
    return false;
  }

  bool freePointer = false;
  if (!ImplicitConvert(cx, arg, type, value->mData, true, &freePointer))
    return false;

  if (freePointer) {
    // ImplicitConvert converted a string for us, which we have to free.
    // Keep track of it.
    if (!strings->growBy(1)) {
      JS_ReportOutOfMemory(cx);
      return false;
    }
    strings->back().mData = *static_cast<char**>(value->mData);
  }

  return true;
}

JSBool
FunctionType::Call(JSContext* cx,
                   unsigned argc,
                   jsval* vp)
{
  // get the callee object...
  JSObject* obj = JSVAL_TO_OBJECT(JS_CALLEE(cx, vp));
  if (!CData::IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return false;
  }

  JSObject* typeObj = CData::GetCType(obj);
  if (CType::GetTypeCode(typeObj) != TYPE_pointer) {
    JS_ReportError(cx, "not a FunctionType.ptr");
    return false;
  }

  typeObj = PointerType::GetBaseType(typeObj);
  if (CType::GetTypeCode(typeObj) != TYPE_function) {
    JS_ReportError(cx, "not a FunctionType.ptr");
    return false;
  }

  FunctionInfo* fninfo = GetFunctionInfo(typeObj);
  uint32_t argcFixed = fninfo->mArgTypes.length();

  if ((!fninfo->mIsVariadic && argc != argcFixed) ||
      (fninfo->mIsVariadic && argc < argcFixed)) {
    JS_ReportError(cx, "Number of arguments does not match declaration");
    return false;
  }

  // Check if we have a Library object. If we do, make sure it's open.
  jsval slot = JS_GetReservedSlot(obj, SLOT_REFERENT);
  if (!JSVAL_IS_VOID(slot) && Library::IsLibrary(JSVAL_TO_OBJECT(slot))) {
    PRLibrary* library = Library::GetLibrary(JSVAL_TO_OBJECT(slot));
    if (!library) {
      JS_ReportError(cx, "library is not open");
      return false;
    }
  }

  // prepare the values for each argument
  AutoValueAutoArray values;
  AutoValueAutoArray strings;
  if (!values.resize(argc)) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  jsval* argv = JS_ARGV(cx, vp);
  for (unsigned i = 0; i < argcFixed; ++i)
    if (!ConvertArgument(cx, argv[i], fninfo->mArgTypes[i], &values[i], &strings))
      return false;

  if (fninfo->mIsVariadic) {
    if (!fninfo->mFFITypes.resize(argc)) {
      JS_ReportOutOfMemory(cx);
      return false;
    }

    JSObject* obj;  // Could reuse obj instead of declaring a second
    JSObject* type; // JSObject*, but readability would suffer.

    for (uint32_t i = argcFixed; i < argc; ++i) {
      if (JSVAL_IS_PRIMITIVE(argv[i]) ||
          !CData::IsCData(obj = JSVAL_TO_OBJECT(argv[i]))) {
        // Since we know nothing about the CTypes of the ... arguments,
        // they absolutely must be CData objects already.
        JS_ReportError(cx, "argument %d of type %s is not a CData object",
                       i, JS_GetTypeName(cx, JS_TypeOfValue(cx, argv[i])));
        return false;
      }
      if (!(type = CData::GetCType(obj)) ||
          !(type = PrepareType(cx, OBJECT_TO_JSVAL(type))) ||
          // Relying on ImplicitConvert only for the limited purpose of
          // converting one CType to another (e.g., T[] to T*).
          !ConvertArgument(cx, argv[i], type, &values[i], &strings) ||
          !(fninfo->mFFITypes[i] = CType::GetFFIType(cx, type))) {
        // These functions report their own errors.
        return false;
      }
    }
    if (!PrepareCIF(cx, fninfo))
      return false;
  }

  // initialize a pointer to an appropriate location, for storing the result
  AutoValue returnValue;
  TypeCode typeCode = CType::GetTypeCode(fninfo->mReturnType);
  if (typeCode != TYPE_void_t &&
      !returnValue.SizeToType(cx, fninfo->mReturnType)) {
    JS_ReportAllocationOverflow(cx);
    return false;
  }

  uintptr_t fn = *reinterpret_cast<uintptr_t*>(CData::GetData(obj));

#if defined(XP_WIN)
  int32_t lastErrorStatus; // The status as defined by |GetLastError|
  int32_t savedLastError = GetLastError();
  SetLastError(0);
#endif //defined(XP_WIN)
  int errnoStatus;         // The status as defined by |errno|
  int savedErrno = errno;
  errno = 0;

  // suspend the request before we call into the function, since the call
  // may block or otherwise take a long time to return.
  {
    JSAutoSuspendRequest suspend(cx);
    ffi_call(&fninfo->mCIF, FFI_FN(fn), returnValue.mData,
             reinterpret_cast<void**>(values.begin()));

    // Save error value.
    // We need to save it before leaving the scope of |suspend| as destructing
    // |suspend| has the side-effect of clearing |GetLastError|
    // (see bug 684017).

    errnoStatus = errno;
#if defined(XP_WIN)
    lastErrorStatus = GetLastError();
#endif // defined(XP_WIN)
  }
#if defined(XP_WIN)
  SetLastError(savedLastError);
#endif // defined(XP_WIN)

  errno = savedErrno;

  // Store the error value for later consultation with |ctypes.getStatus|
  JSObject *objCTypes = CType::GetGlobalCTypes(cx, typeObj);

  JS_SetReservedSlot(objCTypes, SLOT_ERRNO, INT_TO_JSVAL(errnoStatus));
#if defined(XP_WIN)
  JS_SetReservedSlot(objCTypes, SLOT_LASTERROR, INT_TO_JSVAL(lastErrorStatus));
#endif // defined(XP_WIN)

  // Small integer types get returned as a word-sized ffi_arg. Coerce it back
  // into the correct size for ConvertToJS.
  switch (typeCode) {
#define DEFINE_INT_TYPE(name, type, ffiType)                                   \
  case TYPE_##name:                                                            \
    if (sizeof(type) < sizeof(ffi_arg)) {                                      \
      ffi_arg data = *static_cast<ffi_arg*>(returnValue.mData);                \
      *static_cast<type*>(returnValue.mData) = static_cast<type>(data);        \
    }                                                                          \
    break;
#define DEFINE_WRAPPED_INT_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#define DEFINE_BOOL_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#define DEFINE_CHAR_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#define DEFINE_JSCHAR_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#include "typedefs.h"
  default:
    break;
  }

  // prepare a JS object from the result
  RootedObject returnType(cx, fninfo->mReturnType);
  return ConvertToJS(cx, returnType, NullPtr(), returnValue.mData, false, true, vp);
}

FunctionInfo*
FunctionType::GetFunctionInfo(JSObject* obj)
{
  JS_ASSERT(CType::IsCType(obj));
  JS_ASSERT(CType::GetTypeCode(obj) == TYPE_function);

  jsval slot = JS_GetReservedSlot(obj, SLOT_FNINFO);
  JS_ASSERT(!JSVAL_IS_VOID(slot) && JSVAL_TO_PRIVATE(slot));

  return static_cast<FunctionInfo*>(JSVAL_TO_PRIVATE(slot));
}

static JSBool
CheckFunctionType(JSContext* cx, JSObject* obj)
{
  if (!CType::IsCType(obj) || CType::GetTypeCode(obj) != TYPE_function) {
    JS_ReportError(cx, "not a FunctionType");
    return JS_FALSE;
  }
  return JS_TRUE;
}

JSBool
FunctionType::ArgTypesGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp)
{
  if (!CheckFunctionType(cx, obj))
    return JS_FALSE;

  // Check if we have a cached argTypes array.
  vp.set(JS_GetReservedSlot(obj, SLOT_ARGS_T));
  if (!JSVAL_IS_VOID(vp))
    return JS_TRUE;

  FunctionInfo* fninfo = GetFunctionInfo(obj);
  size_t len = fninfo->mArgTypes.length();

  // Prepare a new array.
  Array<jsval, 16> vec;
  if (!vec.resize(len))
    return JS_FALSE;

  for (size_t i = 0; i < len; ++i)
    vec[i] = OBJECT_TO_JSVAL(fninfo->mArgTypes[i]);

  RootedObject argTypes(cx, JS_NewArrayObject(cx, len, vec.begin()));
  if (!argTypes)
    return JS_FALSE;

  // Seal and cache it.
  if (!JS_FreezeObject(cx, argTypes))
    return JS_FALSE;
  JS_SetReservedSlot(obj, SLOT_ARGS_T, OBJECT_TO_JSVAL(argTypes));

  vp.set(OBJECT_TO_JSVAL(argTypes));
  return JS_TRUE;
}

JSBool
FunctionType::ReturnTypeGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp)
{
  if (!CheckFunctionType(cx, obj))
    return JS_FALSE;

  // Get the returnType object from the FunctionInfo.
  vp.set(OBJECT_TO_JSVAL(GetFunctionInfo(obj)->mReturnType));
  return JS_TRUE;
}

JSBool
FunctionType::ABIGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp)
{
  if (!CheckFunctionType(cx, obj))
    return JS_FALSE;

  // Get the abi object from the FunctionInfo.
  vp.set(OBJECT_TO_JSVAL(GetFunctionInfo(obj)->mABI));
  return JS_TRUE;
}

JSBool
FunctionType::IsVariadicGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp)
{
  if (!CheckFunctionType(cx, obj))
    return JS_FALSE;

  vp.set(BOOLEAN_TO_JSVAL(GetFunctionInfo(obj)->mIsVariadic));
  return JS_TRUE;
}

/*******************************************************************************
** CClosure implementation
*******************************************************************************/

JSObject*
CClosure::Create(JSContext* cx,
                 HandleObject typeObj,
                 HandleObject fnObj,
                 HandleObject thisObj,
                 jsval errVal,
                 PRFuncPtr* fnptr)
{
  JS_ASSERT(fnObj);

  RootedObject result(cx, JS_NewObject(cx, &sCClosureClass, NULL, NULL));
  if (!result)
    return NULL;

  // Get the FunctionInfo from the FunctionType.
  FunctionInfo* fninfo = FunctionType::GetFunctionInfo(typeObj);
  JS_ASSERT(!fninfo->mIsVariadic);
  JS_ASSERT(GetABICode(fninfo->mABI) != ABI_WINAPI);

  AutoPtr<ClosureInfo> cinfo(cx->new_<ClosureInfo>(JS_GetRuntime(cx)));
  if (!cinfo) {
    JS_ReportOutOfMemory(cx);
    return NULL;
  }

  // Get the prototype of the FunctionType object, of class CTypeProto,
  // which stores our JSContext for use with the closure.
  JSObject* proto = JS_GetPrototype(typeObj);
  JS_ASSERT(proto);
  JS_ASSERT(CType::IsCTypeProto(proto));

  // Get a JSContext for use with the closure.
  jsval slot = JS_GetReservedSlot(proto, SLOT_CLOSURECX);
  if (!JSVAL_IS_VOID(slot)) {
    // Use the existing JSContext.
    cinfo->cx = static_cast<JSContext*>(JSVAL_TO_PRIVATE(slot));
    JS_ASSERT(cinfo->cx);
  } else {
    // Lazily instantiate a new JSContext, and stash it on
    // ctypes.FunctionType.prototype.
    JSRuntime* runtime = JS_GetRuntime(cx);
    cinfo->cx = JS_NewContext(runtime, 8192);
    if (!cinfo->cx) {
      JS_ReportOutOfMemory(cx);
      return NULL;
    }

    JS_SetReservedSlot(proto, SLOT_CLOSURECX, PRIVATE_TO_JSVAL(cinfo->cx));
  }

  // Prepare the error sentinel value. It's important to do this now, because
  // we might be unable to convert the value to the proper type. If so, we want
  // the caller to know about it _now_, rather than some uncertain time in the
  // future when the error sentinel is actually needed.
  if (!JSVAL_IS_VOID(errVal)) {

    // Make sure the callback returns something.
    if (CType::GetTypeCode(fninfo->mReturnType) == TYPE_void_t) {
      JS_ReportError(cx, "A void callback can't pass an error sentinel");
      return NULL;
    }

    // With the exception of void, the FunctionType constructor ensures that
    // the return type has a defined size.
    JS_ASSERT(CType::IsSizeDefined(fninfo->mReturnType));

    // Allocate a buffer for the return value.
    size_t rvSize = CType::GetSize(fninfo->mReturnType);
    cinfo->errResult = cx->malloc_(rvSize);
    if (!cinfo->errResult)
      return NULL;

    // Do the value conversion. This might fail, in which case we throw.
    if (!ImplicitConvert(cx, errVal, fninfo->mReturnType, cinfo->errResult,
                         false, NULL))
      return NULL;
  } else {
    cinfo->errResult = NULL;
  }

  // Copy the important bits of context into cinfo.
  cinfo->closureObj = result;
  cinfo->typeObj = typeObj;
  cinfo->thisObj = thisObj;
  cinfo->jsfnObj = fnObj;

  // Create an ffi_closure object and initialize it.
  void* code;
  cinfo->closure =
    static_cast<ffi_closure*>(ffi_closure_alloc(sizeof(ffi_closure), &code));
  if (!cinfo->closure || !code) {
    JS_ReportError(cx, "couldn't create closure - libffi error");
    return NULL;
  }

  ffi_status status = ffi_prep_closure_loc(cinfo->closure, &fninfo->mCIF,
    CClosure::ClosureStub, cinfo.get(), code);
  if (status != FFI_OK) {
    JS_ReportError(cx, "couldn't create closure - libffi error");
    return NULL;
  }

  // Stash the ClosureInfo struct on our new object.
  JS_SetReservedSlot(result, SLOT_CLOSUREINFO, PRIVATE_TO_JSVAL(cinfo.forget()));

  // Casting between void* and a function pointer is forbidden in C and C++.
  // Do it via an integral type.
  *fnptr = reinterpret_cast<PRFuncPtr>(reinterpret_cast<uintptr_t>(code));
  return result;
}

void
CClosure::Trace(JSTracer* trc, JSObject* obj)
{
  // Make sure our ClosureInfo slot is legit. If it's not, bail.
  jsval slot = JS_GetReservedSlot(obj, SLOT_CLOSUREINFO);
  if (JSVAL_IS_VOID(slot))
    return;

  ClosureInfo* cinfo = static_cast<ClosureInfo*>(JSVAL_TO_PRIVATE(slot));

  // Identify our objects to the tracer. (There's no need to identify
  // 'closureObj', since that's us.)
  JS_CALL_OBJECT_TRACER(trc, cinfo->typeObj, "typeObj");
  JS_CALL_OBJECT_TRACER(trc, cinfo->jsfnObj, "jsfnObj");
  if (cinfo->thisObj)
    JS_CALL_OBJECT_TRACER(trc, cinfo->thisObj, "thisObj");
}

void
CClosure::Finalize(JSFreeOp *fop, JSObject* obj)
{
  // Make sure our ClosureInfo slot is legit. If it's not, bail.
  jsval slot = JS_GetReservedSlot(obj, SLOT_CLOSUREINFO);
  if (JSVAL_IS_VOID(slot))
    return;

  ClosureInfo* cinfo = static_cast<ClosureInfo*>(JSVAL_TO_PRIVATE(slot));
  FreeOp::get(fop)->delete_(cinfo);
}

void
CClosure::ClosureStub(ffi_cif* cif, void* result, void** args, void* userData)
{
  JS_ASSERT(cif);
  JS_ASSERT(result);
  JS_ASSERT(args);
  JS_ASSERT(userData);

  // Retrieve the essentials from our closure object.
  ClosureInfo* cinfo = static_cast<ClosureInfo*>(userData);
  JSContext* cx = cinfo->cx;
  RootedObject typeObj(cx, cinfo->typeObj);
  RootedObject thisObj(cx, cinfo->thisObj);
  RootedObject jsfnObj(cx, cinfo->jsfnObj);

  JS_AbortIfWrongThread(JS_GetRuntime(cx));

  JSAutoRequest ar(cx);
  JSAutoCompartment ac(cx, jsfnObj);

  // Assert that our CIFs agree.
  FunctionInfo* fninfo = FunctionType::GetFunctionInfo(typeObj);
  JS_ASSERT(cif == &fninfo->mCIF);

  TypeCode typeCode = CType::GetTypeCode(fninfo->mReturnType);

  // Initialize the result to zero, in case something fails. Small integer types
  // are promoted to a word-sized ffi_arg, so we must be careful to zero the
  // whole word.
  size_t rvSize = 0;
  if (cif->rtype != &ffi_type_void) {
    rvSize = cif->rtype->size;
    switch (typeCode) {
#define DEFINE_INT_TYPE(name, type, ffiType)                                   \
    case TYPE_##name:
#define DEFINE_WRAPPED_INT_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#define DEFINE_BOOL_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#define DEFINE_CHAR_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#define DEFINE_JSCHAR_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#include "typedefs.h"
      rvSize = Align(rvSize, sizeof(ffi_arg));
      break;
    default:
      break;
    }
    memset(result, 0, rvSize);
  }

  // Get a death grip on 'closureObj'.
  js::AutoObjectRooter root(cx, cinfo->closureObj);

  // Set up an array for converted arguments.
  Array<jsval, 16> argv;
  if (!argv.appendN(JSVAL_VOID, cif->nargs)) {
    JS_ReportOutOfMemory(cx);
    return;
  }

  js::AutoArrayRooter roots(cx, argv.length(), argv.begin());
  for (uint32_t i = 0; i < cif->nargs; ++i) {
    // Convert each argument, and have any CData objects created depend on
    // the existing buffers.
    RootedObject argType(cx, fninfo->mArgTypes[i]);
    if (!ConvertToJS(cx, argType, NullPtr(), args[i], false, false, &argv[i]))
      return;
  }

  // Call the JS function. 'thisObj' may be NULL, in which case the JS engine
  // will find an appropriate object to use.
  jsval rval;
  JSBool success = JS_CallFunctionValue(cx, thisObj, OBJECT_TO_JSVAL(jsfnObj),
                                        cif->nargs, argv.begin(), &rval);

  // Convert the result. Note that we pass 'isArgument = false', such that
  // ImplicitConvert will *not* autoconvert a JS string into a pointer-to-char
  // type, which would require an allocation that we can't track. The JS
  // function must perform this conversion itself and return a PointerType
  // CData; thusly, the burden of freeing the data is left to the user.
  if (success && cif->rtype != &ffi_type_void)
    success = ImplicitConvert(cx, rval, fninfo->mReturnType, result, false,
                              NULL);

  if (!success) {
    // Something failed. The callee may have thrown, or it may not have
    // returned a value that ImplicitConvert() was happy with. Depending on how
    // prudent the consumer has been, we may or may not have a recovery plan.

    // In any case, a JS exception cannot be passed to C code, so report the
    // exception if any and clear it from the cx.
    if (JS_IsExceptionPending(cx))
      JS_ReportPendingException(cx);

    if (cinfo->errResult) {
      // Good case: we have a sentinel that we can return. Copy it in place of
      // the actual return value, and then proceed.

      // The buffer we're returning might be larger than the size of the return
      // type, due to libffi alignment issues (see above). But it should never
      // be smaller.
      size_t copySize = CType::GetSize(fninfo->mReturnType);
      JS_ASSERT(copySize <= rvSize);
      memcpy(result, cinfo->errResult, copySize);
    } else {
      // Bad case: not much we can do here. The rv is already zeroed out, so we
      // just report (another) error and hope for the best. JS_ReportError will
      // actually throw an exception here, so then we have to report it. Again.
      // Ugh.
      JS_ReportError(cx, "JavaScript callback failed, and an error sentinel "
                         "was not specified.");
      if (JS_IsExceptionPending(cx))
        JS_ReportPendingException(cx);

      return;
    }
  }

  // Small integer types must be returned as a word-sized ffi_arg. Coerce it
  // back into the size libffi expects.
  switch (typeCode) {
#define DEFINE_INT_TYPE(name, type, ffiType)                                   \
  case TYPE_##name:                                                            \
    if (sizeof(type) < sizeof(ffi_arg)) {                                      \
      ffi_arg data = *static_cast<type*>(result);                              \
      *static_cast<ffi_arg*>(result) = data;                                   \
    }                                                                          \
    break;
#define DEFINE_WRAPPED_INT_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#define DEFINE_BOOL_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#define DEFINE_CHAR_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#define DEFINE_JSCHAR_TYPE(x, y, z) DEFINE_INT_TYPE(x, y, z)
#include "typedefs.h"
  default:
    break;
  }
}

/*******************************************************************************
** CData implementation
*******************************************************************************/

// Create a new CData object of type 'typeObj' containing binary data supplied
// in 'source', optionally with a referent object 'refObj'.
//
// * 'typeObj' must be a CType of defined (but possibly zero) size.
//
// * If an object 'refObj' is supplied, the new CData object stores the
//   referent object in a reserved slot for GC safety, such that 'refObj' will
//   be held alive by the resulting CData object. 'refObj' may or may not be
//   a CData object; merely an object we want to keep alive.
//   * If 'refObj' is a CData object, 'ownResult' must be false.
//   * Otherwise, 'refObj' is a Library or CClosure object, and 'ownResult'
//     may be true or false.
// * Otherwise 'refObj' is NULL. In this case, 'ownResult' may be true or false.
//
// * If 'ownResult' is true, the CData object will allocate an appropriately
//   sized buffer, and free it upon finalization. If 'source' data is
//   supplied, the data will be copied from 'source' into the buffer;
//   otherwise, the entirety of the new buffer will be initialized to zero.
// * If 'ownResult' is false, the new CData's buffer refers to a slice of
//   another buffer kept alive by 'refObj'. 'source' data must be provided,
//   and the new CData's buffer will refer to 'source'.
JSObject*
CData::Create(JSContext* cx,
              HandleObject typeObj,
              HandleObject refObj,
              void* source,
              bool ownResult)
{
  JS_ASSERT(typeObj);
  JS_ASSERT(CType::IsCType(typeObj));
  JS_ASSERT(CType::IsSizeDefined(typeObj));
  JS_ASSERT(ownResult || source);
  JS_ASSERT_IF(refObj && CData::IsCData(refObj), !ownResult);

  // Get the 'prototype' property from the type.
  jsval slot = JS_GetReservedSlot(typeObj, SLOT_PROTO);
  JS_ASSERT(!JSVAL_IS_PRIMITIVE(slot));

  RootedObject proto(cx, JSVAL_TO_OBJECT(slot));
  RootedObject parent(cx, JS_GetParent(typeObj));
  JS_ASSERT(parent);

  RootedObject dataObj(cx, JS_NewObject(cx, &sCDataClass, proto, parent));
  if (!dataObj)
    return NULL;

  // set the CData's associated type
  JS_SetReservedSlot(dataObj, SLOT_CTYPE, OBJECT_TO_JSVAL(typeObj));

  // Stash the referent object, if any, for GC safety.
  if (refObj)
    JS_SetReservedSlot(dataObj, SLOT_REFERENT, OBJECT_TO_JSVAL(refObj));

  // Set our ownership flag.
  JS_SetReservedSlot(dataObj, SLOT_OWNS, BOOLEAN_TO_JSVAL(ownResult));

  // attach the buffer. since it might not be 2-byte aligned, we need to
  // allocate an aligned space for it and store it there. :(
  char** buffer = cx->new_<char*>();
  if (!buffer) {
    JS_ReportOutOfMemory(cx);
    return NULL;
  }

  char* data;
  if (!ownResult) {
    data = static_cast<char*>(source);
  } else {
    // Initialize our own buffer.
    size_t size = CType::GetSize(typeObj);
    data = cx->array_new<char>(size);
    if (!data) {
      // Report a catchable allocation error.
      JS_ReportAllocationOverflow(cx);
      Foreground::delete_(buffer);
      return NULL;
    }

    if (!source)
      memset(data, 0, size);
    else
      memcpy(data, source, size);
  }

  *buffer = data;
  JS_SetReservedSlot(dataObj, SLOT_DATA, PRIVATE_TO_JSVAL(buffer));

  return dataObj;
}

void
CData::Finalize(JSFreeOp *fop, JSObject* obj)
{
  // Delete our buffer, and the data it contains if we own it.
  jsval slot = JS_GetReservedSlot(obj, SLOT_OWNS);
  if (JSVAL_IS_VOID(slot))
    return;

  JSBool owns = JSVAL_TO_BOOLEAN(slot);

  slot = JS_GetReservedSlot(obj, SLOT_DATA);
  if (JSVAL_IS_VOID(slot))
    return;
  char** buffer = static_cast<char**>(JSVAL_TO_PRIVATE(slot));

  if (owns)
    FreeOp::get(fop)->array_delete(*buffer);
  FreeOp::get(fop)->delete_(buffer);
}

JSObject*
CData::GetCType(JSObject* dataObj)
{
  JS_ASSERT(CData::IsCData(dataObj));

  jsval slot = JS_GetReservedSlot(dataObj, SLOT_CTYPE);
  JSObject* typeObj = JSVAL_TO_OBJECT(slot);
  JS_ASSERT(CType::IsCType(typeObj));
  return typeObj;
}

void*
CData::GetData(JSObject* dataObj)
{
  JS_ASSERT(CData::IsCData(dataObj));

  jsval slot = JS_GetReservedSlot(dataObj, SLOT_DATA);

  void** buffer = static_cast<void**>(JSVAL_TO_PRIVATE(slot));
  JS_ASSERT(buffer);
  JS_ASSERT(*buffer);
  return *buffer;
}

bool
CData::IsCData(JSObject* obj)
{
  return JS_GetClass(obj) == &sCDataClass;
}

bool
CData::IsCDataProto(JSObject* obj)
{
  return JS_GetClass(obj) == &sCDataProtoClass;
}

JSBool
CData::ValueGetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, MutableHandleValue vp)
{
  if (!IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  // Convert the value to a primitive; do not create a new CData object.
  RootedObject ctype(cx, GetCType(obj));
  if (!ConvertToJS(cx, ctype, NullPtr(), GetData(obj), true, false, vp.address()))
    return JS_FALSE;

  return JS_TRUE;
}

JSBool
CData::ValueSetter(JSContext* cx, JSHandleObject obj, JSHandleId idval, JSBool strict, MutableHandleValue vp)
{
  if (!IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  return ImplicitConvert(cx, vp, GetCType(obj), GetData(obj), false, NULL);
}

JSBool
CData::Address(JSContext* cx, unsigned argc, jsval* vp)
{
  if (argc != 0) {
    JS_ReportError(cx, "address takes zero arguments");
    return JS_FALSE;
  }

  RootedObject obj(cx, JS_THIS_OBJECT(cx, vp));
  if (!obj)
    return JS_FALSE;
  if (!IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  RootedObject typeObj(cx, CData::GetCType(obj));
  RootedObject pointerType(cx, PointerType::CreateInternal(cx, typeObj));
  if (!pointerType)
    return JS_FALSE;

  // Create a PointerType CData object containing null.
  JSObject* result = CData::Create(cx, pointerType, NullPtr(), NULL, true);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));

  // Manually set the pointer inside the object, so we skip the conversion step.
  void** data = static_cast<void**>(GetData(result));
  *data = GetData(obj);
  return JS_TRUE;
}

JSBool
CData::Cast(JSContext* cx, unsigned argc, jsval* vp)
{
  if (argc != 2) {
    JS_ReportError(cx, "cast takes two arguments");
    return JS_FALSE;
  }

  jsval* argv = JS_ARGV(cx, vp);
  if (JSVAL_IS_PRIMITIVE(argv[0]) ||
      !CData::IsCData(JSVAL_TO_OBJECT(argv[0]))) {
    JS_ReportError(cx, "first argument must be a CData");
    return JS_FALSE;
  }
  RootedObject sourceData(cx, JSVAL_TO_OBJECT(argv[0]));
  JSObject* sourceType = CData::GetCType(sourceData);

  if (JSVAL_IS_PRIMITIVE(argv[1]) ||
      !CType::IsCType(JSVAL_TO_OBJECT(argv[1]))) {
    JS_ReportError(cx, "second argument must be a CType");
    return JS_FALSE;
  }

  RootedObject targetType(cx, JSVAL_TO_OBJECT(argv[1]));
  size_t targetSize;
  if (!CType::GetSafeSize(targetType, &targetSize) ||
      targetSize > CType::GetSize(sourceType)) {
    JS_ReportError(cx,
      "target CType has undefined or larger size than source CType");
    return JS_FALSE;
  }

  // Construct a new CData object with a type of 'targetType' and a referent
  // of 'sourceData'.
  void* data = CData::GetData(sourceData);
  JSObject* result = CData::Create(cx, targetType, sourceData, data, false);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));
  return JS_TRUE;
}

JSBool
CData::GetRuntime(JSContext* cx, unsigned argc, jsval* vp)
{
  if (argc != 1) {
    JS_ReportError(cx, "getRuntime takes one argument");
    return JS_FALSE;
  }

  jsval* argv = JS_ARGV(cx, vp);
  if (JSVAL_IS_PRIMITIVE(argv[0]) ||
      !CType::IsCType(JSVAL_TO_OBJECT(argv[0]))) {
    JS_ReportError(cx, "first argument must be a CType");
    return JS_FALSE;
  }

  RootedObject targetType(cx, JSVAL_TO_OBJECT(argv[0]));
  size_t targetSize;
  if (!CType::GetSafeSize(targetType, &targetSize) ||
      targetSize != sizeof(void*)) {
    JS_ReportError(cx, "target CType has non-pointer size");
    return JS_FALSE;
  }

  void* data = static_cast<void*>(cx->runtime);
  JSObject* result = CData::Create(cx, targetType, NullPtr(), &data, true);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));
  return JS_TRUE;
}

JSBool
CData::ReadString(JSContext* cx, unsigned argc, jsval* vp)
{
  if (argc != 0) {
    JS_ReportError(cx, "readString takes zero arguments");
    return JS_FALSE;
  }

  JSObject* obj = CDataFinalizer::GetCData(cx, JS_THIS_OBJECT(cx, vp));
  if (!obj || !IsCData(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  // Make sure we are a pointer to, or an array of, an 8-bit or 16-bit
  // character or integer type.
  JSObject* baseType;
  JSObject* typeObj = GetCType(obj);
  TypeCode typeCode = CType::GetTypeCode(typeObj);
  void* data;
  size_t maxLength = -1;
  switch (typeCode) {
  case TYPE_pointer:
    baseType = PointerType::GetBaseType(typeObj);
    data = *static_cast<void**>(GetData(obj));
    if (data == NULL) {
      JS_ReportError(cx, "cannot read contents of null pointer");
      return JS_FALSE;
    }
    break;
  case TYPE_array:
    baseType = ArrayType::GetBaseType(typeObj);
    data = GetData(obj);
    maxLength = ArrayType::GetLength(typeObj);
    break;
  default:
    JS_ReportError(cx, "not a PointerType or ArrayType");
    return JS_FALSE;
  }

  // Convert the string buffer, taking care to determine the correct string
  // length in the case of arrays (which may contain embedded nulls).
  JSString* result;
  switch (CType::GetTypeCode(baseType)) {
  case TYPE_int8_t:
  case TYPE_uint8_t:
  case TYPE_char:
  case TYPE_signed_char:
  case TYPE_unsigned_char: {
    char* bytes = static_cast<char*>(data);
    size_t length = strnlen(bytes, maxLength);

    // Determine the length.
    size_t dstlen;
    if (!InflateUTF8StringToBuffer(cx, bytes, length, NULL, &dstlen))
      return JS_FALSE;

    jschar* dst =
      static_cast<jschar*>(JS_malloc(cx, (dstlen + 1) * sizeof(jschar)));
    if (!dst)
      return JS_FALSE;

    ASSERT_OK(InflateUTF8StringToBuffer(cx, bytes, length, dst, &dstlen));
    dst[dstlen] = 0;

    result = JS_NewUCString(cx, dst, dstlen);
    break;
  }
  case TYPE_int16_t:
  case TYPE_uint16_t:
  case TYPE_short:
  case TYPE_unsigned_short:
  case TYPE_jschar: {
    jschar* chars = static_cast<jschar*>(data);
    size_t length = strnlen(chars, maxLength);
    result = JS_NewUCStringCopyN(cx, chars, length);
    break;
  }
  default:
    JS_ReportError(cx,
      "base type is not an 8-bit or 16-bit integer or character type");
    return JS_FALSE;
  }

  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, STRING_TO_JSVAL(result));
  return JS_TRUE;
}

JSString *
CData::GetSourceString(JSContext *cx, JSHandleObject typeObj, void *data)
{
  // Walk the types, building up the toSource() string.
  // First, we build up the type expression:
  // 't.ptr' for pointers;
  // 't.array([n])' for arrays;
  // 'n' for structs, where n = t.name, the struct's name. (We assume this is
  // bound to a variable in the current scope.)
  AutoString source;
  BuildTypeSource(cx, typeObj, true, source);
  AppendString(source, "(");
  if (!BuildDataSource(cx, typeObj, data, false, source))
    return NULL;

  AppendString(source, ")");

  return NewUCString(cx, source);
}

JSBool
CData::ToSource(JSContext* cx, unsigned argc, jsval* vp)
{
  if (argc != 0) {
    JS_ReportError(cx, "toSource takes zero arguments");
    return JS_FALSE;
  }

  JSObject* obj = JS_THIS_OBJECT(cx, vp);
  if (!obj)
    return JS_FALSE;
  if (!CData::IsCData(obj) && !CData::IsCDataProto(obj)) {
    JS_ReportError(cx, "not a CData");
    return JS_FALSE;
  }

  JSString* result;
  if (CData::IsCData(obj)) {
    RootedObject typeObj(cx, CData::GetCType(obj));
    void* data = CData::GetData(obj);

    result = CData::GetSourceString(cx, typeObj, data);
  } else {
    result = JS_NewStringCopyZ(cx, "[CData proto object]");
  }

  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, STRING_TO_JSVAL(result));
  return JS_TRUE;
}

JSBool
CData::ErrnoGetter(JSContext* cx, JSHandleObject obj, JSHandleId, MutableHandleValue vp)
{
  if (!IsCTypesGlobal(obj)) {
    JS_ReportError(cx, "this is not not global object ctypes");
    return JS_FALSE;
  }

  vp.set(JS_GetReservedSlot(obj, SLOT_ERRNO));
  return JS_TRUE;
}

#if defined(XP_WIN)
JSBool
CData::LastErrorGetter(JSContext* cx, JSHandleObject obj, JSHandleId, MutableHandleValue vp)
{
  if (!IsCTypesGlobal(obj)) {
    JS_ReportError(cx, "not global object ctypes");
    return JS_FALSE;
  }

  vp.set(JS_GetReservedSlot(obj, SLOT_LASTERROR));
  return JS_TRUE;
}
#endif // defined(XP_WIN)

JSBool
CDataFinalizer::Methods::ToSource(JSContext *cx, unsigned argc, jsval *vp)
{
  RootedObject objThis(cx, JS_THIS_OBJECT(cx, vp));
  if (!objThis)
    return JS_FALSE;
  if (!CDataFinalizer::IsCDataFinalizer(objThis)) {
    JS_ReportError(cx, "not a CDataFinalizer");
    return JS_FALSE;
  }

  CDataFinalizer::Private *p = (CDataFinalizer::Private *)
    JS_GetPrivate(objThis);

  JSString *strMessage;
  if (!p) {
    strMessage = JS_NewStringCopyZ(cx, "ctypes.CDataFinalizer()");
  } else {
    RootedObject objType(cx, CDataFinalizer::GetCType(cx, objThis));
    if (!objType) {
      JS_ReportError(cx, "CDataFinalizer has no type");
      return JS_FALSE;
    }

    AutoString source;
    AppendString(source, "ctypes.CDataFinalizer(");
    JSString *srcValue = CData::GetSourceString(cx, objType, p->cargs);
    if (!srcValue) {
      return JS_FALSE;
    }
    AppendString(source, srcValue);
    AppendString(source, ", ");
    jsval valCodePtrType = JS_GetReservedSlot(objThis,
                                              SLOT_DATAFINALIZER_CODETYPE);
    if (JSVAL_IS_PRIMITIVE(valCodePtrType)) {
      return JS_FALSE;
    }

    RootedObject typeObj(cx, JSVAL_TO_OBJECT(valCodePtrType));
    JSString *srcDispose = CData::GetSourceString(cx, typeObj, &(p->code));
    if (!srcDispose) {
      return JS_FALSE;
    }

    AppendString(source, srcDispose);
    AppendString(source, ")");
    strMessage = NewUCString(cx, source);
  }

  if (!strMessage) {
    // This is a memory issue, no error message
    return JS_FALSE;
  }

  JS_SET_RVAL(cx, vp, STRING_TO_JSVAL(strMessage));
  return JS_TRUE;
}

JSBool
CDataFinalizer::Methods::ToString(JSContext *cx, unsigned argc, jsval *vp)
{
  JSObject* objThis = JS_THIS_OBJECT(cx, vp);
  if (!objThis)
    return JS_FALSE;
  if (!CDataFinalizer::IsCDataFinalizer(objThis)) {
    JS_ReportError(cx, "not a CDataFinalizer");
    return JS_FALSE;
  }

  JSString *strMessage;
  jsval value;
  if (!JS_GetPrivate(objThis)) {
    // Pre-check whether CDataFinalizer::GetValue can fail
    // to avoid reporting an error when not appropriate.
    strMessage = JS_NewStringCopyZ(cx, "[CDataFinalizer - empty]");
    if (!strMessage) {
      return JS_FALSE;
    }
  } else if (!CDataFinalizer::GetValue(cx, objThis, &value)) {
    JS_NOT_REACHED("Could not convert an empty CDataFinalizer");
  } else {
    strMessage = JS_ValueToString(cx, value);
    if (!strMessage) {
      return JS_FALSE;
    }
  }
  JS_SET_RVAL(cx, vp, STRING_TO_JSVAL(strMessage));
  return JS_TRUE;
}

bool
CDataFinalizer::IsCDataFinalizer(JSObject *obj)
{
  return JS_GetClass(obj) == &sCDataFinalizerClass;
}


JSObject *
CDataFinalizer::GetCType(JSContext *cx, JSObject *obj)
{
  MOZ_ASSERT(IsCDataFinalizer(obj));

  jsval valData = JS_GetReservedSlot(obj,
                                     SLOT_DATAFINALIZER_VALTYPE);
  if (JSVAL_IS_VOID(valData)) {
    return NULL;
  }

  return JSVAL_TO_OBJECT(valData);
}

JSObject*
CDataFinalizer::GetCData(JSContext *cx, JSObject *obj)
{
  if (!obj) {
    JS_ReportError(cx, "No C data");
    return NULL;
  }
  if (CData::IsCData(obj)) {
    return obj;
  }
  if (!CDataFinalizer::IsCDataFinalizer(obj)) {
    JS_ReportError(cx, "Not C data");
    return NULL;
  }
  jsval val;
  if (!CDataFinalizer::GetValue(cx, obj, &val) || JSVAL_IS_PRIMITIVE(val)) {
    JS_ReportError(cx, "Empty CDataFinalizer");
    return NULL;
  }
  return JSVAL_TO_OBJECT(val);
}

bool
CDataFinalizer::GetValue(JSContext *cx, JSObject *obj, jsval *aResult)
{
  MOZ_ASSERT(IsCDataFinalizer(obj));

  CDataFinalizer::Private *p = (CDataFinalizer::Private *)
    JS_GetPrivate(obj);

  if (!p) {
    JS_ReportError(cx, "Attempting to get the value of an empty CDataFinalizer");
    return false;  // We have called |dispose| or |forget| already.
  }

  RootedObject ctype(cx, GetCType(cx, obj));
  return ConvertToJS(cx, ctype, /*parent*/NullPtr(), p -> cargs, false, true, aResult);
}

/*
 * Attach a C function as a finalizer to a JS object.
 *
 * Pseudo-JS signature:
 * function(CData<T>, CData<T -> U>): CDataFinalizer<T>
 *          value,    finalizer
 *
 * This function attaches strong references to the following values:
 * - the CType of |value|
 *
 * Note: This function takes advantage of the fact that non-variadic
 * CData functions are initialized during creation.
 */
JSBool
CDataFinalizer::Construct(JSContext* cx, unsigned argc, jsval *vp)
{
  RootedObject objSelf(cx, JSVAL_TO_OBJECT(JS_CALLEE(cx, vp)));
  RootedObject objProto(cx);
  if (!GetObjectProperty(cx, objSelf, "prototype", &objProto)) {
    JS_ReportError(cx, "CDataFinalizer.prototype does not exist");
    return JS_FALSE;
  }

  // Get arguments
  if (argc == 0) { // Special case: the empty (already finalized) object
    JSObject *objResult = JS_NewObject(cx, &sCDataFinalizerClass, objProto, NULL);
    JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(objResult));
    return JS_TRUE;
  }

  if (argc != 2) {
    JS_ReportError(cx, "CDataFinalizer takes 2 arguments");
    return JS_FALSE;
  }

  jsval* argv = JS_ARGV(cx, vp);
  JS::Value valCodePtr = argv[1];
  if (!valCodePtr.isObject()) {
    return TypeError(cx, "_a CData object_ of a function pointer type",
                     valCodePtr);
  }
  JSObject *objCodePtr = &valCodePtr.toObject();

  //Note: Using a custom argument formatter here would be awkward (requires
  //a destructor just to uninstall the formatter).

  // 2. Extract argument type of |objCodePtr|
  if (!CData::IsCData(objCodePtr)) {
    return TypeError(cx, "a _CData_ object of a function pointer type",
                     valCodePtr);
  }
  JSObject *objCodePtrType = CData::GetCType(objCodePtr);
  MOZ_ASSERT(objCodePtrType);

  TypeCode typCodePtr = CType::GetTypeCode(objCodePtrType);
  if (typCodePtr != TYPE_pointer) {
    return TypeError(cx, "a CData object of a function _pointer_ type",
                     OBJECT_TO_JSVAL(objCodePtrType));
  }

  JSObject *objCodeType = PointerType::GetBaseType(objCodePtrType);
  MOZ_ASSERT(objCodeType);

  TypeCode typCode = CType::GetTypeCode(objCodeType);
  if (typCode != TYPE_function) {
    return TypeError(cx, "a CData object of a _function_ pointer type",
                     OBJECT_TO_JSVAL(objCodePtrType));
  }
  uintptr_t code = *reinterpret_cast<uintptr_t*>(CData::GetData(objCodePtr));
  if (!code) {
    return TypeError(cx, "a CData object of a _non-NULL_ function pointer type",
                     OBJECT_TO_JSVAL(objCodePtrType));
  }

  FunctionInfo* funInfoFinalizer =
    FunctionType::GetFunctionInfo(objCodeType);
  MOZ_ASSERT(funInfoFinalizer);

  if ((funInfoFinalizer->mArgTypes.length() != 1)
      || (funInfoFinalizer->mIsVariadic)) {
    return TypeError(cx, "a function accepting exactly one argument",
                     OBJECT_TO_JSVAL(objCodeType));
  }
  RootedObject objArgType(cx, funInfoFinalizer->mArgTypes[0]);
  RootedObject returnType(cx, funInfoFinalizer->mReturnType);

  // Invariant: At this stage, we know that funInfoFinalizer->mIsVariadic
  // is |false|. Therefore, funInfoFinalizer->mCIF has already been initialized.

  bool freePointer = false;

  // 3. Perform dynamic cast of |argv[0]| into |objType|, store it in |cargs|

  size_t sizeArg;
  jsval valData = argv[0];
  if (!CType::GetSafeSize(objArgType, &sizeArg)) {
    return TypeError(cx, "(an object with known size)", valData);
  }

  ScopedFreePtr<void> cargs(malloc(sizeArg));

  if (!ImplicitConvert(cx, valData, objArgType, cargs.get(),
                       false, &freePointer)) {
    return TypeError(cx, "(an object that can be converted to the following type)",
                     OBJECT_TO_JSVAL(objArgType));
  }
  if (freePointer) {
    // Note: We could handle that case, if necessary.
    JS_ReportError(cx, "Internal Error during CDataFinalizer. Object cannot be represented");
    return JS_FALSE;
  }

  // 4. Prepare buffer for holding return value

  ScopedFreePtr<void> rvalue;
  if (CType::GetTypeCode(returnType) != TYPE_void_t) {
    rvalue = malloc(Align(CType::GetSize(returnType),
                          sizeof(ffi_arg)));
  } //Otherwise, simply do not allocate

  // 5. Create |objResult|

  JSObject *objResult = JS_NewObject(cx, &sCDataFinalizerClass, objProto, NULL);
  if (!objResult) {
    return JS_FALSE;
  }

  // If our argument is a CData, it holds a type.
  // This is the type that we should capture, not that
  // of the function, which may be less precise.
  JSObject *objBestArgType = objArgType;
  if (!JSVAL_IS_PRIMITIVE(valData)) {
    JSObject *objData = JSVAL_TO_OBJECT(valData);
    if (CData::IsCData(objData)) {
      objBestArgType = CData::GetCType(objData);
      size_t sizeBestArg;
      if (!CType::GetSafeSize(objBestArgType, &sizeBestArg)) {
        JS_NOT_REACHED("object with unknown size");
      }
      if (sizeBestArg != sizeArg) {
        return TypeError(cx, "(an object with the same size as that expected by the C finalization function)", valData);
      }
    }
  }

  // Used by GetCType
  JS_SetReservedSlot(objResult,
                     SLOT_DATAFINALIZER_VALTYPE,
                     OBJECT_TO_JSVAL(objBestArgType));

  // Used by ToSource
  JS_SetReservedSlot(objResult,
                     SLOT_DATAFINALIZER_CODETYPE,
                     OBJECT_TO_JSVAL(objCodePtrType));

  ffi_abi abi;
  if (!GetABI(cx, OBJECT_TO_JSVAL(funInfoFinalizer->mABI), &abi)) {
    JS_ReportError(cx, "Internal Error: "
                   "Invalid ABI specification in CDataFinalizer");
    return false;
  }

  ffi_type* rtype = CType::GetFFIType(cx, funInfoFinalizer->mReturnType);
  if (!rtype) {
    JS_ReportError(cx, "Internal Error: "
                   "Could not access ffi type of CDataFinalizer");
    return JS_FALSE;
  }

  // 7. Store C information as private
  ScopedFreePtr<CDataFinalizer::Private>
    p((CDataFinalizer::Private*)malloc(sizeof(CDataFinalizer::Private)));

  memmove(&p->CIF, &funInfoFinalizer->mCIF, sizeof(ffi_cif));

  p->cargs = cargs.forget();
  p->rvalue = rvalue.forget();
  p->cargs_size = sizeArg;
  p->code = code;


  JS_SetPrivate(objResult, p.forget());
  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(objResult));
  return JS_TRUE;
}


/*
 * Actually call the finalizer. Does not perform any cleanup on the object.
 *
 * Preconditions: |this| must be a |CDataFinalizer|, |p| must be non-null.
 * The function fails if |this| has gone through |Forget|/|Dispose|
 * or |Finalize|.
 *
 * This function does not alter the value of |errno|/|GetLastError|.
 *
 * If argument |errnoStatus| is non-NULL, it receives the value of |errno|
 * immediately after the call. Under Windows, if argument |lastErrorStatus|
 * is non-NULL, it receives the value of |GetLastError| immediately after the
 * call. On other platforms, |lastErrorStatus| is ignored.
 */
void
CDataFinalizer::CallFinalizer(CDataFinalizer::Private *p,
                              int* errnoStatus,
                              int32_t* lastErrorStatus)
{
  int savedErrno = errno;
  errno = 0;
#if defined(XP_WIN)
  int32_t savedLastError = GetLastError();
  SetLastError(0);
#endif // defined(XP_WIN)

  ffi_call(&p->CIF, FFI_FN(p->code), p->rvalue, &p->cargs);

  if (errnoStatus) {
    *errnoStatus = errno;
  }
  errno = savedErrno;
#if defined(XP_WIN)
  if (lastErrorStatus) {
    *lastErrorStatus = GetLastError();
  }
  SetLastError(savedLastError);
#endif // defined(XP_WIN)
}

/*
 * Forget the value.
 *
 * Preconditions: |this| must be a |CDataFinalizer|.
 * The function fails if |this| has gone through |Forget|/|Dispose|
 * or |Finalize|.
 *
 * Does not call the finalizer. Cleans up the Private memory and releases all
 * strong references.
 */
JSBool
CDataFinalizer::Methods::Forget(JSContext* cx, unsigned argc, jsval *vp)
{
  if (argc != 0) {
    JS_ReportError(cx, "CDataFinalizer.prototype.forget takes no arguments");
    return JS_FALSE;
  }

  JSObject *obj = JS_THIS_OBJECT(cx, vp);
  if (!obj)
    return JS_FALSE;
  if (!CDataFinalizer::IsCDataFinalizer(obj)) {
    return TypeError(cx, "a CDataFinalizer", OBJECT_TO_JSVAL(obj));
  }

  CDataFinalizer::Private *p = (CDataFinalizer::Private *)
    JS_GetPrivate(obj);

  if (!p) {
    JS_ReportError(cx, "forget called on an empty CDataFinalizer");
    return JS_FALSE;
  }

  jsval valJSData;
  RootedObject ctype(cx, GetCType(cx, obj));
  if (!ConvertToJS(cx, ctype, NullPtr(), p->cargs, false, true, &valJSData)) {
    JS_ReportError(cx, "CDataFinalizer value cannot be represented");
    return JS_FALSE;
  }

  CDataFinalizer::Cleanup(p, obj);

  JS_SET_RVAL(cx, vp, valJSData);
  return JS_TRUE;
}

/*
 * Clean up the value.
 *
 * Preconditions: |this| must be a |CDataFinalizer|.
 * The function fails if |this| has gone through |Forget|/|Dispose|
 * or |Finalize|.
 *
 * Calls the finalizer, cleans up the Private memory and releases all
 * strong references.
 */
JSBool
CDataFinalizer::Methods::Dispose(JSContext* cx, unsigned argc, jsval *vp)
{
  if (argc != 0) {
    JS_ReportError(cx, "CDataFinalizer.prototype.dispose takes no arguments");
    return JS_FALSE;
  }

  JSObject *obj = JS_THIS_OBJECT(cx, vp);
  if (!obj)
    return JS_FALSE;
  if (!CDataFinalizer::IsCDataFinalizer(obj)) {
    return TypeError(cx, "a CDataFinalizer", OBJECT_TO_JSVAL(obj));
  }

  CDataFinalizer::Private *p = (CDataFinalizer::Private *)
    JS_GetPrivate(obj);

  if (!p) {
    JS_ReportError(cx, "dispose called on an empty CDataFinalizer.");
    return JS_FALSE;
  }

  jsval valType = JS_GetReservedSlot(obj, SLOT_DATAFINALIZER_VALTYPE);
  JS_ASSERT(!JSVAL_IS_PRIMITIVE(valType));

  JSObject *objCTypes = CType::GetGlobalCTypes(cx, JSVAL_TO_OBJECT(valType));

  jsval valCodePtrType = JS_GetReservedSlot(obj, SLOT_DATAFINALIZER_CODETYPE);
  JS_ASSERT(!JSVAL_IS_PRIMITIVE(valCodePtrType));
  JSObject *objCodePtrType = JSVAL_TO_OBJECT(valCodePtrType);

  JSObject *objCodeType = PointerType::GetBaseType(objCodePtrType);
  JS_ASSERT(objCodeType);
  JS_ASSERT(CType::GetTypeCode(objCodeType) == TYPE_function);

  RootedObject resultType(cx, FunctionType::GetFunctionInfo(objCodeType)->mReturnType);
  jsval result = JSVAL_VOID;

  int errnoStatus;
#if defined(XP_WIN)
  int32_t lastErrorStatus;
  CDataFinalizer::CallFinalizer(p, &errnoStatus, &lastErrorStatus);
#else
  CDataFinalizer::CallFinalizer(p, &errnoStatus, NULL);
#endif // defined(XP_WIN)

  JS_SetReservedSlot(objCTypes, SLOT_ERRNO, INT_TO_JSVAL(errnoStatus));
#if defined(XP_WIN)
  JS_SetReservedSlot(objCTypes, SLOT_LASTERROR, INT_TO_JSVAL(lastErrorStatus));
#endif // defined(XP_WIN)

  if (ConvertToJS(cx, resultType, NullPtr(), p->rvalue, false, true, &result)) {
    CDataFinalizer::Cleanup(p, obj);
    JS_SET_RVAL(cx, vp, result);
    return true;
  }
  CDataFinalizer::Cleanup(p, obj);
  return false;
}

/*
 * Perform finalization.
 *
 * Preconditions: |this| must be the result of |CDataFinalizer|.
 * It may have gone through |Forget|/|Dispose|.
 *
 * If |this| has not gone through |Forget|/|Dispose|, calls the
 * finalizer, cleans up the Private memory and releases all
 * strong references.
 */
void
CDataFinalizer::Finalize(JSFreeOp* fop, JSObject* obj)
{
  CDataFinalizer::Private *p = (CDataFinalizer::Private *)
    JS_GetPrivate(obj);

  if (!p) {
    return;
  }

  CDataFinalizer::CallFinalizer(p, NULL, NULL);
  CDataFinalizer::Cleanup(p, NULL);
}

/*
 * Perform cleanup of a CDataFinalizer
 *
 * Release strong references, cleanup |Private|.
 *
 * Argument |p| contains the private information of the CDataFinalizer. If NULL,
 * this function does nothing.
 * Argument |obj| should contain |NULL| during finalization (or in any context
 * in which the object itself should not be cleaned up), or a CDataFinalizer
 * object otherwise.
 */
void
CDataFinalizer::Cleanup(CDataFinalizer::Private *p, JSObject *obj)
{
  if (!p) {
    return;  // We have already cleaned up
  }

  free(p->cargs);
  free(p->rvalue);
  free(p);

  if (!obj) {
    return;  // No slots to clean up
  }

  JS_ASSERT(CDataFinalizer::IsCDataFinalizer(obj));

  JS_SetPrivate(obj, NULL);
  for (int i = 0; i < CDATAFINALIZER_SLOTS; ++i) {
    JS_SetReservedSlot(obj, i, JSVAL_NULL);
  }
}


/*******************************************************************************
** Int64 and UInt64 implementation
*******************************************************************************/

JSObject*
Int64Base::Construct(JSContext* cx,
                     HandleObject proto,
                     uint64_t data,
                     bool isUnsigned)
{
  JSClass* clasp = isUnsigned ? &sUInt64Class : &sInt64Class;
  RootedObject parent(cx, JS_GetParent(proto));
  RootedObject result(cx, JS_NewObject(cx, clasp, proto, parent));
  if (!result)
    return NULL;

  // attach the Int64's data
  uint64_t* buffer = cx->new_<uint64_t>(data);
  if (!buffer) {
    JS_ReportOutOfMemory(cx);
    return NULL;
  }

  JS_SetReservedSlot(result, SLOT_INT64, PRIVATE_TO_JSVAL(buffer));

  if (!JS_FreezeObject(cx, result))
    return NULL;

  return result;
}

void
Int64Base::Finalize(JSFreeOp *fop, JSObject* obj)
{
  jsval slot = JS_GetReservedSlot(obj, SLOT_INT64);
  if (JSVAL_IS_VOID(slot))
    return;

  FreeOp::get(fop)->delete_(static_cast<uint64_t*>(JSVAL_TO_PRIVATE(slot)));
}

uint64_t
Int64Base::GetInt(JSObject* obj) {
  JS_ASSERT(Int64::IsInt64(obj) || UInt64::IsUInt64(obj));

  jsval slot = JS_GetReservedSlot(obj, SLOT_INT64);
  return *static_cast<uint64_t*>(JSVAL_TO_PRIVATE(slot));
}

JSBool
Int64Base::ToString(JSContext* cx,
                    JSObject* obj,
                    unsigned argc,
                    jsval* vp,
                    bool isUnsigned)
{
  if (argc > 1) {
    JS_ReportError(cx, "toString takes zero or one argument");
    return JS_FALSE;
  }

  int radix = 10;
  if (argc == 1) {
    jsval arg = JS_ARGV(cx, vp)[0];
    if (JSVAL_IS_INT(arg))
      radix = JSVAL_TO_INT(arg);
    if (!JSVAL_IS_INT(arg) || radix < 2 || radix > 36) {
      JS_ReportError(cx, "radix argument must be an integer between 2 and 36");
      return JS_FALSE;
    }
  }

  AutoString intString;
  if (isUnsigned) {
    IntegerToString(GetInt(obj), radix, intString);
  } else {
    IntegerToString(static_cast<int64_t>(GetInt(obj)), radix, intString);
  }

  JSString *result = NewUCString(cx, intString);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, STRING_TO_JSVAL(result));
  return JS_TRUE;
}

JSBool
Int64Base::ToSource(JSContext* cx,
                    JSObject* obj,
                    unsigned argc,
                    jsval* vp,
                    bool isUnsigned)
{
  if (argc != 0) {
    JS_ReportError(cx, "toSource takes zero arguments");
    return JS_FALSE;
  }

  // Return a decimal string suitable for constructing the number.
  AutoString source;
  if (isUnsigned) {
    AppendString(source, "ctypes.UInt64(\"");
    IntegerToString(GetInt(obj), 10, source);
  } else {
    AppendString(source, "ctypes.Int64(\"");
    IntegerToString(static_cast<int64_t>(GetInt(obj)), 10, source);
  }
  AppendString(source, "\")");

  JSString *result = NewUCString(cx, source);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, STRING_TO_JSVAL(result));
  return JS_TRUE;
}

JSBool
Int64::Construct(JSContext* cx,
                 unsigned argc,
                 jsval* vp)
{
  CallArgs args = CallArgsFromVp(argc, vp);

  // Construct and return a new Int64 object.
  if (argc != 1) {
    JS_ReportError(cx, "Int64 takes one argument");
    return JS_FALSE;
  }

  int64_t i = 0;
  if (!jsvalToBigInteger(cx, args[0], true, &i))
    return TypeError(cx, "int64", args[0]);

  // Get ctypes.Int64.prototype from the 'prototype' property of the ctor.
  jsval slot;
  RootedObject callee(cx, &args.callee());
  ASSERT_OK(JS_GetProperty(cx, callee, "prototype", &slot));
  RootedObject proto(cx, JSVAL_TO_OBJECT(slot));
  JS_ASSERT(JS_GetClass(proto) == &sInt64ProtoClass);

  JSObject* result = Int64Base::Construct(cx, proto, i, false);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));
  return JS_TRUE;
}

bool
Int64::IsInt64(JSObject* obj)
{
  return JS_GetClass(obj) == &sInt64Class;
}

JSBool
Int64::ToString(JSContext* cx, unsigned argc, jsval* vp)
{
  JSObject* obj = JS_THIS_OBJECT(cx, vp);
  if (!obj)
    return JS_FALSE;
  if (!Int64::IsInt64(obj)) {
    JS_ReportError(cx, "not an Int64");
    return JS_FALSE;
  }

  return Int64Base::ToString(cx, obj, argc, vp, false);
}

JSBool
Int64::ToSource(JSContext* cx, unsigned argc, jsval* vp)
{
  JSObject* obj = JS_THIS_OBJECT(cx, vp);
  if (!obj)
    return JS_FALSE;
  if (!Int64::IsInt64(obj)) {
    JS_ReportError(cx, "not an Int64");
    return JS_FALSE;
  }

  return Int64Base::ToSource(cx, obj, argc, vp, false);
}

JSBool
Int64::Compare(JSContext* cx, unsigned argc, jsval* vp)
{
  jsval* argv = JS_ARGV(cx, vp);
  if (argc != 2 ||
      JSVAL_IS_PRIMITIVE(argv[0]) ||
      JSVAL_IS_PRIMITIVE(argv[1]) ||
      !Int64::IsInt64(JSVAL_TO_OBJECT(argv[0])) ||
      !Int64::IsInt64(JSVAL_TO_OBJECT(argv[1]))) {
    JS_ReportError(cx, "compare takes two Int64 arguments");
    return JS_FALSE;
  }

  JSObject* obj1 = JSVAL_TO_OBJECT(argv[0]);
  JSObject* obj2 = JSVAL_TO_OBJECT(argv[1]);

  int64_t i1 = Int64Base::GetInt(obj1);
  int64_t i2 = Int64Base::GetInt(obj2);

  if (i1 == i2)
    JS_SET_RVAL(cx, vp, INT_TO_JSVAL(0));
  else if (i1 < i2)
    JS_SET_RVAL(cx, vp, INT_TO_JSVAL(-1));
  else
    JS_SET_RVAL(cx, vp, INT_TO_JSVAL(1));

  return JS_TRUE;
}

#define LO_MASK ((uint64_t(1) << 32) - 1)
#define INT64_LO(i) ((i) & LO_MASK)
#define INT64_HI(i) ((i) >> 32)

JSBool
Int64::Lo(JSContext* cx, unsigned argc, jsval* vp)
{
  jsval* argv = JS_ARGV(cx, vp);
  if (argc != 1 || JSVAL_IS_PRIMITIVE(argv[0]) ||
      !Int64::IsInt64(JSVAL_TO_OBJECT(argv[0]))) {
    JS_ReportError(cx, "lo takes one Int64 argument");
    return JS_FALSE;
  }

  JSObject* obj = JSVAL_TO_OBJECT(argv[0]);
  int64_t u = Int64Base::GetInt(obj);
  double d = uint32_t(INT64_LO(u));

  jsval result = JS_NumberValue(d);

  JS_SET_RVAL(cx, vp, result);
  return JS_TRUE;
}

JSBool
Int64::Hi(JSContext* cx, unsigned argc, jsval* vp)
{
  jsval* argv = JS_ARGV(cx, vp);
  if (argc != 1 || JSVAL_IS_PRIMITIVE(argv[0]) ||
      !Int64::IsInt64(JSVAL_TO_OBJECT(argv[0]))) {
    JS_ReportError(cx, "hi takes one Int64 argument");
    return JS_FALSE;
  }

  JSObject* obj = JSVAL_TO_OBJECT(argv[0]);
  int64_t u = Int64Base::GetInt(obj);
  double d = int32_t(INT64_HI(u));

  jsval result = JS_NumberValue(d);

  JS_SET_RVAL(cx, vp, result);
  return JS_TRUE;
}

JSBool
Int64::Join(JSContext* cx, unsigned argc, jsval* vp)
{
  if (argc != 2) {
    JS_ReportError(cx, "join takes two arguments");
    return JS_FALSE;
  }

  jsval* argv = JS_ARGV(cx, vp);
  int32_t hi;
  uint32_t lo;
  if (!jsvalToInteger(cx, argv[0], &hi))
    return TypeError(cx, "int32", argv[0]);
  if (!jsvalToInteger(cx, argv[1], &lo))
    return TypeError(cx, "uint32", argv[1]);

  int64_t i = (int64_t(hi) << 32) + int64_t(lo);

  // Get Int64.prototype from the function's reserved slot.
  JSObject* callee = JSVAL_TO_OBJECT(JS_CALLEE(cx, vp));

  jsval slot = js::GetFunctionNativeReserved(callee, SLOT_FN_INT64PROTO);
  RootedObject proto(cx, JSVAL_TO_OBJECT(slot));
  JS_ASSERT(JS_GetClass(proto) == &sInt64ProtoClass);

  JSObject* result = Int64Base::Construct(cx, proto, i, false);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));
  return JS_TRUE;
}

JSBool
UInt64::Construct(JSContext* cx,
                  unsigned argc,
                  jsval* vp)
{
  CallArgs args = CallArgsFromVp(argc, vp);

  // Construct and return a new UInt64 object.
  if (argc != 1) {
    JS_ReportError(cx, "UInt64 takes one argument");
    return JS_FALSE;
  }

  uint64_t u = 0;
  if (!jsvalToBigInteger(cx, args[0], true, &u))
    return TypeError(cx, "uint64", args[0]);

  // Get ctypes.UInt64.prototype from the 'prototype' property of the ctor.
  jsval slot;
  RootedObject callee(cx, &args.callee());
  ASSERT_OK(JS_GetProperty(cx, callee, "prototype", &slot));
  RootedObject proto(cx, JSVAL_TO_OBJECT(slot));
  JS_ASSERT(JS_GetClass(proto) == &sUInt64ProtoClass);

  JSObject* result = Int64Base::Construct(cx, proto, u, true);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));
  return JS_TRUE;
}

bool
UInt64::IsUInt64(JSObject* obj)
{
  return JS_GetClass(obj) == &sUInt64Class;
}

JSBool
UInt64::ToString(JSContext* cx, unsigned argc, jsval* vp)
{
  JSObject* obj = JS_THIS_OBJECT(cx, vp);
  if (!obj)
    return JS_FALSE;
  if (!UInt64::IsUInt64(obj)) {
    JS_ReportError(cx, "not a UInt64");
    return JS_FALSE;
  }

  return Int64Base::ToString(cx, obj, argc, vp, true);
}

JSBool
UInt64::ToSource(JSContext* cx, unsigned argc, jsval* vp)
{
  JSObject* obj = JS_THIS_OBJECT(cx, vp);
  if (!obj)
    return JS_FALSE;
  if (!UInt64::IsUInt64(obj)) {
    JS_ReportError(cx, "not a UInt64");
    return JS_FALSE;
  }

  return Int64Base::ToSource(cx, obj, argc, vp, true);
}

JSBool
UInt64::Compare(JSContext* cx, unsigned argc, jsval* vp)
{
  jsval* argv = JS_ARGV(cx, vp);
  if (argc != 2 ||
      JSVAL_IS_PRIMITIVE(argv[0]) ||
      JSVAL_IS_PRIMITIVE(argv[1]) ||
      !UInt64::IsUInt64(JSVAL_TO_OBJECT(argv[0])) ||
      !UInt64::IsUInt64(JSVAL_TO_OBJECT(argv[1]))) {
    JS_ReportError(cx, "compare takes two UInt64 arguments");
    return JS_FALSE;
  }

  JSObject* obj1 = JSVAL_TO_OBJECT(argv[0]);
  JSObject* obj2 = JSVAL_TO_OBJECT(argv[1]);

  uint64_t u1 = Int64Base::GetInt(obj1);
  uint64_t u2 = Int64Base::GetInt(obj2);

  if (u1 == u2)
    JS_SET_RVAL(cx, vp, INT_TO_JSVAL(0));
  else if (u1 < u2)
    JS_SET_RVAL(cx, vp, INT_TO_JSVAL(-1));
  else
    JS_SET_RVAL(cx, vp, INT_TO_JSVAL(1));

  return JS_TRUE;
}

JSBool
UInt64::Lo(JSContext* cx, unsigned argc, jsval* vp)
{
  jsval* argv = JS_ARGV(cx, vp);
  if (argc != 1 || JSVAL_IS_PRIMITIVE(argv[0]) ||
      !UInt64::IsUInt64(JSVAL_TO_OBJECT(argv[0]))) {
    JS_ReportError(cx, "lo takes one UInt64 argument");
    return JS_FALSE;
  }

  JSObject* obj = JSVAL_TO_OBJECT(argv[0]);
  uint64_t u = Int64Base::GetInt(obj);
  double d = uint32_t(INT64_LO(u));

  jsval result = JS_NumberValue(d);

  JS_SET_RVAL(cx, vp, result);
  return JS_TRUE;
}

JSBool
UInt64::Hi(JSContext* cx, unsigned argc, jsval* vp)
{
  jsval* argv = JS_ARGV(cx, vp);
  if (argc != 1 || JSVAL_IS_PRIMITIVE(argv[0]) ||
      !UInt64::IsUInt64(JSVAL_TO_OBJECT(argv[0]))) {
    JS_ReportError(cx, "hi takes one UInt64 argument");
    return JS_FALSE;
  }

  JSObject* obj = JSVAL_TO_OBJECT(argv[0]);
  uint64_t u = Int64Base::GetInt(obj);
  double d = uint32_t(INT64_HI(u));

  jsval result = JS_NumberValue(d);

  JS_SET_RVAL(cx, vp, result);
  return JS_TRUE;
}

JSBool
UInt64::Join(JSContext* cx, unsigned argc, jsval* vp)
{
  if (argc != 2) {
    JS_ReportError(cx, "join takes two arguments");
    return JS_FALSE;
  }

  jsval* argv = JS_ARGV(cx, vp);
  uint32_t hi;
  uint32_t lo;
  if (!jsvalToInteger(cx, argv[0], &hi))
    return TypeError(cx, "uint32_t", argv[0]);
  if (!jsvalToInteger(cx, argv[1], &lo))
    return TypeError(cx, "uint32_t", argv[1]);

  uint64_t u = (uint64_t(hi) << 32) + uint64_t(lo);

  // Get UInt64.prototype from the function's reserved slot.
  JSObject* callee = JSVAL_TO_OBJECT(JS_CALLEE(cx, vp));

  jsval slot = js::GetFunctionNativeReserved(callee, SLOT_FN_INT64PROTO);
  RootedObject proto(cx, JSVAL_TO_OBJECT(slot));
  JS_ASSERT(JS_GetClass(proto) == &sUInt64ProtoClass);

  JSObject* result = Int64Base::Construct(cx, proto, u, true);
  if (!result)
    return JS_FALSE;

  JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(result));
  return JS_TRUE;
}

}
}

