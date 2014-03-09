/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_idbrequest_h__
#define mozilla_dom_indexeddb_idbrequest_h__

#include "js/RootingAPI.h"
#include "mozilla/Attributes.h"
#include "mozilla/EventForwards.h"
#include "mozilla/dom/IDBRequestBinding.h"
#include "mozilla/dom/indexedDB/IDBWrapperCache.h"
#include "nsAutoPtr.h"
#include "nsCycleCollectionParticipant.h"

class nsPIDOMWindow;
struct PRThread;

namespace mozilla {

class ErrorResult;

namespace dom {

class DOMError;
class ErrorEventInit;
template <typename> class Nullable;
class OwningIDBObjectStoreOrIDBIndexOrIDBCursor;

namespace indexedDB {

class IDBCursor;
class IDBDatabase;
class IDBFactory;
class IDBIndex;
class IDBObjectStore;
class IDBTransaction;

class IDBRequest
  : public IDBWrapperCache
{
public:
  typedef nsresult
    (*GetResultCallback)(JSContext* aCx,
                         void* aUserData,
                         JS::MutableHandle<JS::Value> aResult);

protected:
  // At most one of these three fields can be non-null.
  nsRefPtr<IDBObjectStore> mSourceAsObjectStore;
  nsRefPtr<IDBIndex> mSourceAsIndex;
  nsRefPtr<IDBCursor> mSourceAsCursor;

  nsRefPtr<IDBTransaction> mTransaction;

#ifdef DEBUG
  PRThread* mOwningThread;
#endif

  JS::Heap<JS::Value> mResultVal;
  nsRefPtr<DOMError> mError;

  GetResultCallback mGetResultCallback;
  void* mGetResultCallbackUserData;

  nsString mFilename;
#ifdef MOZ_ENABLE_PROFILER_SPS
  uint64_t mSerialNumber;
#endif
  nsresult mErrorCode;
  uint32_t mLineNo;
  bool mHaveResultOrErrorCode;

public:
  typedef nsresult
    (*GetResultCallback)(JSContext* aCx,
                         void* aUserData,
                         JS::MutableHandle<JS::Value> aResult);

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS_INHERITED(IDBRequest,
                                                         IDBWrapperCache)

  static already_AddRefed<IDBRequest>
  Create(IDBDatabase* aDatabase, IDBTransaction* aTransaction);

  static already_AddRefed<IDBRequest>
  Create(IDBObjectStore* aSource,
         IDBDatabase* aDatabase,
         IDBTransaction* aTransaction);

  static already_AddRefed<IDBRequest>
  Create(IDBIndex* aSource,
         IDBDatabase* aDatabase,
         IDBTransaction* aTransaction);

  // nsIDOMEventTarget
  virtual nsresult
  PreHandleEvent(EventChainPreVisitor& aVisitor) MOZ_OVERRIDE;

  void
  GetSource(Nullable<OwningIDBObjectStoreOrIDBIndexOrIDBCursor>& aSource) const;

  void
  Reset();

  void
  DispatchNonTransactionError(nsresult aErrorCode);

  void
  NotifyHelperSentResultsToChildProcess(nsresult aRv);

  void
  SetError(nsresult aRv);

  void
  SetResultCallback(GetResultCallback aCallback, void* aUserData = nullptr);

  nsresult
  GetErrorCode() const
#ifdef DEBUG
  ;
#else
  {
    return mErrorCode;
  }
#endif

  DOMError*
  GetError(ErrorResult& aRv);

  JSContext*
  GetJSContext();

  void CaptureCaller();

  void
  FillScriptErrorEvent(ErrorEventInit& aEventInit) const;

  bool
  IsPending() const
  {
    return !mHaveResultOrErrorCode;
  }

#ifdef MOZ_ENABLE_PROFILER_SPS
  uint64_t
  GetSerialNumber() const
  {
    return mSerialNumber;
  }
#endif

  // nsWrapperCache
  virtual JSObject*
  WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope) MOZ_OVERRIDE;

  // WebIDL
  nsPIDOMWindow*
  GetParentObject() const
  {
    return GetOwner();
  }

  JS::Value
  GetResult(JSContext* aCx, ErrorResult& aRv);

  IDBTransaction*
  GetTransaction() const
  {
    AssertIsOnOwningThread();
    return mTransaction;
  }

  IDBRequestReadyState
  ReadyState() const;

  IMPL_EVENT_HANDLER(success);
  IMPL_EVENT_HANDLER(error);

  void
  AssertIsOnOwningThread() const
#ifdef DEBUG
  ;
#else
  { }
#endif

protected:
  IDBRequest(IDBDatabase* aDatabase);
  IDBRequest(nsPIDOMWindow* aOwner);
  ~IDBRequest();

  void
  InitMembers();

  void
  ConstructResult();

  void
  AssertSourceIsCorrect() const
#ifdef DEBUG
  ;
#else
  { }
#endif
};

class IDBOpenDBRequest MOZ_FINAL
  : public IDBRequest
{
  // Only touched on the owning thread.
  nsRefPtr<IDBFactory> mFactory;

public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(IDBOpenDBRequest, IDBRequest)

  static
  already_AddRefed<IDBOpenDBRequest>
  Create(IDBFactory* aFactory,
         nsPIDOMWindow* aOwner,
         JS::Handle<JSObject*> aScriptOwner);

  void
  SetTransaction(IDBTransaction* aTransaction);

  // nsIDOMEventTarget
  virtual nsresult
  PostHandleEvent(EventChainPostVisitor& aVisitor) MOZ_OVERRIDE;

  DOMError*
  GetError(ErrorResult& aRv)
  {
    return IDBRequest::GetError(aRv);
  }

  IDBFactory*
  Factory() const
  {
    return mFactory;
  }

  // nsWrapperCache
  virtual JSObject*
  WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope) MOZ_OVERRIDE;

  // WebIDL
  IMPL_EVENT_HANDLER(blocked);
  IMPL_EVENT_HANDLER(upgradeneeded);

private:
  IDBOpenDBRequest(nsPIDOMWindow* aOwner);
  ~IDBOpenDBRequest();
};

} // namespace indexedDB
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_indexeddb_idbrequest_h__
