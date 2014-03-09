/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBRequest.h"

#include "AsyncConnectionHelper.h"
#include "BackgroundChildImpl.h"
#include "IDBCursor.h"
#include "IDBEvents.h"
#include "IDBFactory.h"
#include "IDBIndex.h"
#include "IDBObjectStore.h"
#include "IDBTransaction.h"
#include "mozilla/ContentEvents.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/EventDispatcher.h"
#include "mozilla/dom/ErrorEventBinding.h"
#include "mozilla/dom/IDBOpenDBRequestBinding.h"
#include "mozilla/dom/UnionTypes.h"
#include "nsComponentManagerUtils.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsCxPusher.h"
#include "nsDOMClassInfoID.h"
#include "nsDOMJSUtils.h"
#include "nsIScriptContext.h"
#include "nsJSUtils.h"
#include "nsPIDOMWindow.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "nsWrapperCacheInlines.h"
#include "ReportInternalError.h"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::indexedDB;
using namespace mozilla::ipc;

IDBRequest::IDBRequest(IDBDatabase* aDatabase)
  : IDBWrapperCache(aDatabase)
{
  MOZ_ASSERT(aDatabase);
  InitMembers();
}

IDBRequest::IDBRequest(nsPIDOMWindow* aOwner)
  : IDBWrapperCache(aOwner)
{
  InitMembers();
}

IDBRequest::~IDBRequest()
{
  AssertIsOnOwningThread();
}

#ifdef DEBUG

void
IDBRequest::AssertIsOnOwningThread() const
{
  MOZ_ASSERT(mOwningThread);
  MOZ_ASSERT(PR_GetCurrentThread() == mOwningThread);
}

#endif // DEBUG

void
IDBRequest::InitMembers()
{
#ifdef DEBUG
  mOwningThread = PR_GetCurrentThread();
#endif
  AssertIsOnOwningThread();

  mResultVal.setUndefined();
  mGetResultCallback = nullptr;
  mGetResultCallbackUserData = nullptr;
  mErrorCode = NS_OK;
  mLineNo = 0;
  mHaveResultOrErrorCode = false;

#ifdef MOZ_ENABLE_PROFILER_SPS
  // XXX This should be threadlocal!
  /*
  BackgroundChildImpl::ThreadLocal* threadLocal =
    BackgroundChildImpl::GetThreadLocalForCurrentThread();
  MOZ_ASSERT(threadLocal);

  mSerialNumber = threadLocal->mNextRequestSerialNumber++;
  */
  static uint64_t sNextSerial = 1;
  mSerialNumber = sNextSerial++;
#endif
}

// static
already_AddRefed<IDBRequest>
IDBRequest::Create(IDBDatabase* aDatabase,
                   IDBTransaction* aTransaction)
{
  MOZ_ASSERT(aDatabase);
  aDatabase->AssertIsOnOwningThread();

  nsRefPtr<IDBRequest> request = new IDBRequest(aDatabase);

  request->mTransaction = aTransaction;
  request->SetScriptOwner(aDatabase->GetScriptOwner());

  request->CaptureCaller();

  return request.forget();
}

// static
already_AddRefed<IDBRequest>
IDBRequest::Create(IDBObjectStore* aSourceAsObjectStore,
                   IDBDatabase* aDatabase,
                   IDBTransaction* aTransaction)
{
  nsRefPtr<IDBRequest> request = Create(aDatabase, aTransaction);

  request->mSourceAsObjectStore = aSourceAsObjectStore;

  return request.forget();
}

// static
already_AddRefed<IDBRequest>
IDBRequest::Create(IDBIndex* aSourceAsIndex,
                   IDBDatabase* aDatabase,
                   IDBTransaction* aTransaction)
{
  nsRefPtr<IDBRequest> request = Create(aDatabase, aTransaction);

  request->mSourceAsIndex = aSourceAsIndex;

  return request.forget();
}

#ifdef DEBUG
void
IDBRequest::AssertSourceIsCorrect() const
{
  // At most one of mSourceAs* is allowed to be non-null.  Check that by
  // summing the double negation of each one and asserting the sum is at most
  // 1.

  MOZ_ASSERT(!!mSourceAsObjectStore + !!mSourceAsIndex + !!mSourceAsCursor <= 1);
}
#endif

void
IDBRequest::GetSource(Nullable<OwningIDBObjectStoreOrIDBIndexOrIDBCursor>& aSource) const
{
  MOZ_ASSERT(NS_IsMainThread());

  AssertSourceIsCorrect();

  if (mSourceAsObjectStore) {
    aSource.SetValue().SetAsIDBObjectStore() = mSourceAsObjectStore;
  } else if (mSourceAsIndex) {
    aSource.SetValue().SetAsIDBIndex() = mSourceAsIndex;
  } else if (mSourceAsCursor) {
    aSource.SetValue().SetAsIDBCursor() = mSourceAsCursor;
  } else {
    aSource.SetNull();
  }
}

void
IDBRequest::Reset()
{
  AssertIsOnOwningThread();

  mResultVal.setUndefined();
  mGetResultCallback = nullptr;
  mGetResultCallbackUserData = nullptr;
  mHaveResultOrErrorCode = false;
  mError = nullptr;
}

void
IDBRequest::DispatchNonTransactionError(nsresult aErrorCode)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(NS_FAILED(aErrorCode));

  SetError(aErrorCode);

  // Make an error event and fire it at the target.
  nsCOMPtr<nsIDOMEvent> event =
    CreateGenericEvent(this, NS_LITERAL_STRING(ERROR_EVT_STR), eDoesBubble,
                       eCancelable);
  MOZ_ASSERT(event);

  bool ignored;
  NS_WARN_IF(NS_FAILED(DispatchEvent(event, &ignored)));
}

void
IDBRequest::NotifyHelperSentResultsToChildProcess(nsresult aRv)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(!mHaveResultOrErrorCode, "Already called!");
  NS_ASSERTION(JSVAL_IS_VOID(mResultVal), "Should be undefined!");

  // See if our window is still valid. If not then we're going to pretend that
  // we never completed.
  if (NS_FAILED(CheckInnerWindowCorrectness())) {
    return;
  }

  mHaveResultOrErrorCode = true;

  if (NS_FAILED(aRv)) {
    SetError(aRv);
  }
}

void
IDBRequest::SetError(nsresult aRv)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(NS_FAILED(aRv));
  MOZ_ASSERT(!mError);

  mHaveResultOrErrorCode = true;
  mError = new DOMError(GetOwner(), aRv);
  mErrorCode = aRv;

  mResultVal.setUndefined();
}

#ifdef DEBUG
nsresult
IDBRequest::GetErrorCode() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mHaveResultOrErrorCode);

  return mErrorCode;
}
#endif

JSContext*
IDBRequest::GetJSContext()
{
  AssertIsOnOwningThread();

  JSContext* cx;

  // XXX Fix me!
  MOZ_ASSERT(NS_IsMainThread(), "This can't work off the main thread!");
  if (GetScriptOwner()) {
    return nsContentUtils::GetSafeJSContext();
  }

  nsresult rv;
  nsIScriptContext* sc = GetContextForEventHandlers(&rv);
  NS_ENSURE_SUCCESS(rv, nullptr);
  NS_ENSURE_TRUE(sc, nullptr);

  cx = sc->GetNativeContext();
  NS_ASSERTION(cx, "Failed to get a context!");

  return cx;
}

void
IDBRequest::CaptureCaller()
{
  AutoJSContext cx;

  const char* filename = nullptr;
  uint32_t lineNo = 0;
  if (!nsJSUtils::GetCallingLocation(cx, &filename, &lineNo)) {
    NS_WARNING("Failed to get caller.");
    return;
  }

  mFilename.Assign(NS_ConvertUTF8toUTF16(filename));
  mLineNo = lineNo;
}

void
IDBRequest::FillScriptErrorEvent(ErrorEventInit& aEventInit) const
{
  aEventInit.mLineno = mLineNo;
  aEventInit.mFilename = mFilename;
}

mozilla::dom::IDBRequestReadyState
IDBRequest::ReadyState() const
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  if (IsPending()) {
    return IDBRequestReadyState::Pending;
  }

  return IDBRequestReadyState::Done;
}

JSObject*
IDBRequest::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope)
{
  return IDBRequestBinding::Wrap(aCx, aScope, this);
}

JS::Value
IDBRequest::GetResult(JSContext* aCx, mozilla::ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (!mHaveResultOrErrorCode) {
    MOZ_ASSERT(mGetResultCallback);

    ConstructResult();

    MOZ_ASSERT(mHaveResultOrErrorCode);
  }

  return mResultVal;
}

void
IDBRequest::SetResultCallback(GetResultCallback aCallback,
                              void* aUserData)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aCallback);
  MOZ_ASSERT(!mHaveResultOrErrorCode);
  MOZ_ASSERT(!mGetResultCallback);
  MOZ_ASSERT(!mGetResultCallbackUserData);
  MOZ_ASSERT(mResultVal.isUndefined());
  MOZ_ASSERT(!mError);

  mGetResultCallback = aCallback;
  mGetResultCallbackUserData = aUserData;
}

void
IDBRequest::ConstructResult()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(!mHaveResultOrErrorCode);
  MOZ_ASSERT(mGetResultCallback);
  MOZ_ASSERT(mResultVal.isUndefined());
  MOZ_ASSERT(!mError);

  // See if our window is still valid.
  if (NS_WARN_IF(NS_FAILED(CheckInnerWindowCorrectness()))) {
    IDB_REPORT_INTERNAL_ERR();
    SetError(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return;
  }

  JSContext* cx = GetJSContext();
  if (NS_WARN_IF(!cx)) {
    IDB_REPORT_INTERNAL_ERR();
    SetError(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return;
  }

  Maybe<AutoPushJSContext> maybePush;
  if (NS_IsMainThread()) {
    maybePush.construct(cx);
  }

  JS::Rooted<JSObject*> global(cx, IDBWrapperCache::GetParentObject());
  MOZ_ASSERT(global);

  JSAutoCompartment ac(cx, global);
  AssertIsRooted();

  JS::Rooted<JS::Value> result(cx);
  nsresult rv = mGetResultCallback(cx, mGetResultCallbackUserData, &result);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    SetError(rv);
    mResultVal.setUndefined();
  } else {
    mError = nullptr;
    mResultVal = result;
  }

  mGetResultCallback = nullptr;
  mGetResultCallbackUserData = nullptr;

  mHaveResultOrErrorCode = true;
}

mozilla::dom::DOMError*
IDBRequest::GetError(mozilla::ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (!mHaveResultOrErrorCode) {
    if (!mGetResultCallback) {
      aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
      return nullptr;
    }

    ConstructResult();

    MOZ_ASSERT(mHaveResultOrErrorCode);
  }

  return mError;
}

NS_IMPL_CYCLE_COLLECTION_CLASS(IDBRequest)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(IDBRequest, IDBWrapperCache)
  // Don't need NS_IMPL_CYCLE_COLLECTION_TRAVERSE_SCRIPT_OBJECTS because
  // nsDOMEventTargetHelper does it for us.
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSourceAsObjectStore)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSourceAsIndex)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSourceAsCursor)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mTransaction)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mError)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(IDBRequest, IDBWrapperCache)
  tmp->mResultVal.setUndefined();
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSourceAsObjectStore)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSourceAsIndex)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSourceAsCursor)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mTransaction)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mError)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN_INHERITED(IDBRequest, IDBWrapperCache)
  // Don't need NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER because
  // nsDOMEventTargetHelper does it for us.
  NS_IMPL_CYCLE_COLLECTION_TRACE_JSVAL_MEMBER_CALLBACK(mResultVal)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(IDBRequest)
NS_INTERFACE_MAP_END_INHERITING(IDBWrapperCache)

NS_IMPL_ADDREF_INHERITED(IDBRequest, IDBWrapperCache)
NS_IMPL_RELEASE_INHERITED(IDBRequest, IDBWrapperCache)

nsresult
IDBRequest::PreHandleEvent(EventChainPreVisitor& aVisitor)
{
  AssertIsOnOwningThread();

  aVisitor.mCanHandle = true;
  aVisitor.mParentTarget = mTransaction;
  return NS_OK;
}

IDBOpenDBRequest::IDBOpenDBRequest(nsPIDOMWindow* aOwner)
  : IDBRequest(aOwner)
{
  AssertIsOnOwningThread();
}

IDBOpenDBRequest::~IDBOpenDBRequest()
{
  AssertIsOnOwningThread();
}

// static
already_AddRefed<IDBOpenDBRequest>
IDBOpenDBRequest::Create(IDBFactory* aFactory,
                         nsPIDOMWindow* aOwner,
                         JS::Handle<JSObject*> aScriptOwner)
{
  MOZ_ASSERT(aFactory);
  aFactory->AssertIsOnOwningThread();

  nsRefPtr<IDBOpenDBRequest> request = new IDBOpenDBRequest(aOwner);

  request->SetScriptOwner(aScriptOwner);
  request->mFactory = aFactory;

  request->CaptureCaller();

  return request.forget();
}

void
IDBOpenDBRequest::SetTransaction(IDBTransaction* aTransaction)
{
  AssertIsOnOwningThread();

  MOZ_ASSERT(!aTransaction || !mTransaction);

  mTransaction = aTransaction;
}

NS_IMPL_CYCLE_COLLECTION_CLASS(IDBOpenDBRequest)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(IDBOpenDBRequest,
                                                  IDBRequest)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mFactory)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(IDBOpenDBRequest,
                                                IDBRequest)
  // Don't unlink mFactory!
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(IDBOpenDBRequest)
NS_INTERFACE_MAP_END_INHERITING(IDBRequest)

NS_IMPL_ADDREF_INHERITED(IDBOpenDBRequest, IDBRequest)
NS_IMPL_RELEASE_INHERITED(IDBOpenDBRequest, IDBRequest)

nsresult
IDBOpenDBRequest::PostHandleEvent(EventChainPostVisitor& aVisitor)
{
  // XXX Fix me!
  MOZ_ASSERT(NS_IsMainThread());

  return IndexedDatabaseManager::FireWindowOnError(GetOwner(), aVisitor);
}

JSObject*
IDBOpenDBRequest::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope)
{
  AssertIsOnOwningThread();

  return IDBOpenDBRequestBinding::Wrap(aCx, aScope, this);
}
