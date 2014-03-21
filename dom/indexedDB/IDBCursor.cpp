/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBCursor.h"

#include "ActorsChild.h"
#include "IDBDatabase.h"
#include "IDBIndex.h"
#include "IDBObjectStore.h"
#include "IDBRequest.h"
#include "IDBTransaction.h"
#include "IndexedDatabaseInlines.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/dom/UnionTypes.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBSharedTypes.h"
#include "nsString.h"
#include "ProfilerHelpers.h"
#include "ReportInternalError.h"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::indexedDB;

/*
class ContinueHelper : public CursorHelper
{
public:
  ContinueHelper(IDBCursor* aCursor,
                 int32_t aCount)
  : CursorHelper(aCursor), mCount(aCount)
  {
    MOZ_ASSERT(NS_IsMainThread());
    MOZ_ASSERT(aCursor);
    MOZ_ASSERT(aCount > 0);
  }

  virtual nsresult DoDatabaseWork(mozIStorageConnection* aConnection)
                                  MOZ_OVERRIDE;

  virtual nsresult GetSuccessResult(JSContext* aCx,
                                    JS::MutableHandle<JS::Value> aVal)
                                    MOZ_OVERRIDE;

  virtual void ReleaseMainThreadObjects() MOZ_OVERRIDE;

  virtual nsresult
  PackArgumentsForParentProcess(CursorRequestParams& aParams) MOZ_OVERRIDE;

  virtual ChildProcessSendResult
  SendResponseToChildProcess(nsresult aResultCode) MOZ_OVERRIDE;

  virtual nsresult
  UnpackResponseFromParentProcess(const ResponseValue& aResponseValue)
                                  MOZ_OVERRIDE;

protected:
  virtual ~ContinueHelper()
  {
    IDBObjectStore::ClearCloneReadInfo(mCloneReadInfo);
  }

  virtual nsresult
  BindArgumentsToStatement(mozIStorageStatement* aStatement) = 0;

  virtual nsresult
  GatherResultsFromStatement(mozIStorageStatement* aStatement) = 0;

  void UpdateCursorState()
  {
    mCursor->mCachedKey = JSVAL_VOID;
    mCursor->mCachedPrimaryKey = JSVAL_VOID;
    mCursor->mCachedValue = JSVAL_VOID;
    mCursor->mHaveCachedKey = false;
    mCursor->mHaveCachedPrimaryKey = false;
    mCursor->mHaveCachedValue = false;
    mCursor->mContinueCalled = false;

    if (mKey.IsUnset()) {
      mCursor->mHaveValue = false;
    } else {
      MOZ_ASSERT(mCursor->mType == IDBCursor::OBJECTSTORE ||
                 mCursor->mType == IDBCursor::OBJECTSTOREKEY ||
                 !mObjectKey.IsUnset());

      // Set new values.
      mCursor->mKey = mKey;
      mCursor->mObjectKey = mObjectKey;
      mCursor->mContinueToKey.Unset();

      mCursor->mCloneReadInfo = Move(mCloneReadInfo);
      mCloneReadInfo.mCloneBuffer.clear();
    }
  }

  int32_t mCount;
  Key mKey;
  Key mObjectKey;
  StructuredCloneReadInfo mCloneReadInfo;
};

class ContinueObjectStoreHelper : public ContinueHelper
{
public:
  ContinueObjectStoreHelper(IDBCursor* aCursor,
                            uint32_t aCount)
  : ContinueHelper(aCursor, aCount)
  { }

protected:
  virtual ~ContinueObjectStoreHelper()
  { }

private:
  nsresult BindArgumentsToStatement(mozIStorageStatement* aStatement);
  nsresult GatherResultsFromStatement(mozIStorageStatement* aStatement);
};

class ContinueObjectStoreKeyHelper : public ContinueObjectStoreHelper
{
public:
  ContinueObjectStoreKeyHelper(IDBCursor* aCursor,
                               uint32_t aCount)
  : ContinueObjectStoreHelper(aCursor, aCount)
  { }

private:
  virtual ~ContinueObjectStoreKeyHelper()
  { }

  virtual nsresult
  GatherResultsFromStatement(mozIStorageStatement* aStatement) MOZ_OVERRIDE;
};

class ContinueIndexHelper : public ContinueHelper
{
public:
  ContinueIndexHelper(IDBCursor* aCursor,
                      uint32_t aCount)
  : ContinueHelper(aCursor, aCount)
  { }

protected:
  virtual ~ContinueIndexHelper()
  { }

private:
  nsresult BindArgumentsToStatement(mozIStorageStatement* aStatement);
  nsresult GatherResultsFromStatement(mozIStorageStatement* aStatement);
};

class ContinueIndexObjectHelper : public ContinueIndexHelper
{
public:
  ContinueIndexObjectHelper(IDBCursor* aCursor,
                            uint32_t aCount)
  : ContinueIndexHelper(aCursor, aCount)
  { }

private:
  virtual ~ContinueIndexObjectHelper()
  { }

  nsresult GatherResultsFromStatement(mozIStorageStatement* aStatement);
};
*/

IDBCursor::IDBCursor(Type aType,
                     IDBRequest* aRequest,
                     IDBObjectStore* aSourceObjectStore,
                     IDBIndex* aSourceIndex,
                     IDBTransaction* aTransaction,
                     BackgroundCursorChild* aBackgroundActor,
                     Direction aDirection,
                     const Key& aKey)
  : mRequest(aRequest)
  , mSourceObjectStore(aSourceObjectStore)
  , mSourceIndex(aSourceIndex)
  , mTransaction(aTransaction)
  , mBackgroundActor(aBackgroundActor)
  , mScriptOwner(aTransaction->Database()->GetScriptOwner())
  , mCachedKey(JSVAL_VOID)
  , mCachedPrimaryKey(JSVAL_VOID)
  , mCachedValue(JSVAL_VOID)
  , mKey(aKey)
  , mType(aType)
  , mDirection(aDirection)
  , mHaveCachedKey(false)
  , mHaveCachedPrimaryKey(false)
  , mHaveCachedValue(false)
  , mRooted(false)
  , mContinueCalled(false)
  , mHaveValue(true)
{
  MOZ_ASSERT_IF(aType == Type_ObjectStore || aType == Type_ObjectStoreKey,
                aSourceObjectStore);
  MOZ_ASSERT_IF(aType == Type_Index || aType == Type_IndexKey, aSourceIndex);
  MOZ_ASSERT(aTransaction);
  aTransaction->AssertIsOnOwningThread();
  MOZ_ASSERT(aBackgroundActor);
  MOZ_ASSERT(!aKey.IsUnset());
  MOZ_ASSERT(mScriptOwner);

  SetIsDOMBinding();

  if (mScriptOwner) {
    mozilla::HoldJSObjects(this);
    mRooted = true;
  }
}

IDBCursor::~IDBCursor()
{
  AssertIsOnOwningThread();

  DropJSObjects();

  if (mBackgroundActor) {
    mBackgroundActor->SendDeleteMeInternal();
    MOZ_ASSERT(!mBackgroundActor, "SendDeleteMeInternal should have cleared!");
  }
}

// static
already_AddRefed<IDBCursor>
IDBCursor::Create(IDBRequest* aRequest,
                  IDBObjectStore* aObjectStore,
                  BackgroundCursorChild* aBackgroundActor,
                  Direction aDirection,
                  const Key& aKey,
                  StructuredCloneReadInfo&& aCloneInfo)
{
  MOZ_ASSERT(aRequest);
  aRequest->AssertIsOnOwningThread();
  MOZ_ASSERT(aObjectStore);
  MOZ_ASSERT(aBackgroundActor);
  MOZ_ASSERT(!aKey.IsUnset());

  nsRefPtr<IDBCursor> cursor =
    new IDBCursor(Type_ObjectStore, aRequest, aObjectStore, nullptr,
                  aObjectStore->Transaction(), aBackgroundActor, aDirection,
                  aKey);

  cursor->mCloneInfo = Move(aCloneInfo);

  return cursor.forget();
}

// static
already_AddRefed<IDBCursor>
IDBCursor::Create(IDBRequest* aRequest,
                  IDBObjectStore* aObjectStore,
                  BackgroundCursorChild* aBackgroundActor,
                  Direction aDirection,
                  const Key& aKey)
{
  MOZ_ASSERT(aRequest);
  aRequest->AssertIsOnOwningThread();
  MOZ_ASSERT(aObjectStore);
  MOZ_ASSERT(aBackgroundActor);
  MOZ_ASSERT(!aKey.IsUnset());

  nsRefPtr<IDBCursor> cursor =
    new IDBCursor(Type_ObjectStoreKey, aRequest, aObjectStore, nullptr,
                  aObjectStore->Transaction(), aBackgroundActor, aDirection,
                  aKey);

  return cursor.forget();
}

// static
already_AddRefed<IDBCursor>
IDBCursor::Create(IDBRequest* aRequest,
                  IDBIndex* aIndex,
                  BackgroundCursorChild* aBackgroundActor,
                  Direction aDirection,
                  const Key& aKey,
                  const Key& aPrimaryKey,
                  StructuredCloneReadInfo&& aCloneInfo)
{
  MOZ_ASSERT(aRequest);
  aRequest->AssertIsOnOwningThread();
  MOZ_ASSERT(aIndex);
  MOZ_ASSERT(aBackgroundActor);
  MOZ_ASSERT(!aKey.IsUnset());
  MOZ_ASSERT(!aPrimaryKey.IsUnset());

  nsRefPtr<IDBCursor> cursor =
    new IDBCursor(Type_Index, aRequest, nullptr, aIndex,
                  aIndex->ObjectStore()->Transaction(), aBackgroundActor,
                  aDirection, aKey);

  cursor->mCloneInfo = Move(aCloneInfo);

  return cursor.forget();
}

// static
already_AddRefed<IDBCursor>
IDBCursor::Create(IDBRequest* aRequest,
                  IDBIndex* aIndex,
                  BackgroundCursorChild* aBackgroundActor,
                  Direction aDirection,
                  const Key& aKey,
                  const Key& aPrimaryKey)
{
  MOZ_ASSERT(aRequest);
  aRequest->AssertIsOnOwningThread();
  MOZ_ASSERT(aIndex);
  MOZ_ASSERT(aBackgroundActor);
  MOZ_ASSERT(!aKey.IsUnset());
  MOZ_ASSERT(!aPrimaryKey.IsUnset());

  nsRefPtr<IDBCursor> cursor =
    new IDBCursor(Type_IndexKey, aRequest, nullptr, aIndex,
                  aIndex->ObjectStore()->Transaction(), aBackgroundActor,
                  aDirection, aKey);

  return cursor.forget();
}

// static
auto
IDBCursor::ConvertDirection(IDBCursorDirection aDirection) -> Direction
{
  switch (aDirection) {
    case mozilla::dom::IDBCursorDirection::Next:
      return NEXT;

    case mozilla::dom::IDBCursorDirection::Nextunique:
      return NEXT_UNIQUE;

    case mozilla::dom::IDBCursorDirection::Prev:
      return PREV;

    case mozilla::dom::IDBCursorDirection::Prevunique:
      return PREV_UNIQUE;

    default:
      MOZ_ASSUME_UNREACHABLE("Should never get here!");
  }
}

#ifdef DEBUG

void
IDBCursor::AssertIsOnOwningThread() const
{
  MOZ_ASSERT(mTransaction);
  mTransaction->AssertIsOnOwningThread();
}

#endif // DEBUG

void
IDBCursor::DropJSObjects()
{
  AssertIsOnOwningThread();

  Reset();

  if (!mRooted) {
    return;
  }

  mScriptOwner = nullptr;
  mRooted = false;

  mozilla::DropJSObjects(this);
}

void
IDBCursor::Reset()
{
  AssertIsOnOwningThread();

  mCachedKey.setUndefined();
  mCachedPrimaryKey.setUndefined();
  mCachedValue.setUndefined();
  IDBObjectStore::ClearCloneReadInfo(mCloneInfo);

  mHaveCachedKey = false;
  mHaveCachedPrimaryKey = false;
  mHaveCachedValue = false;
  mHaveValue = false;
}

nsPIDOMWindow*
IDBCursor::GetParentObject() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mTransaction);

  return mTransaction->GetParentObject();
}

IDBCursorDirection
IDBCursor::GetDirection() const
{
  AssertIsOnOwningThread();

  switch (mDirection) {
    case NEXT:
      return IDBCursorDirection::Next;

    case NEXT_UNIQUE:
      return IDBCursorDirection::Nextunique;

    case PREV:
      return IDBCursorDirection::Prev;

    case PREV_UNIQUE:
      return IDBCursorDirection::Prevunique;

    default:
      MOZ_ASSUME_UNREACHABLE("Should never get here!");
  }
}

void
IDBCursor::GetSource(OwningIDBObjectStoreOrIDBIndex& aSource) const
{
  AssertIsOnOwningThread();

  switch (mType) {
    case Type_ObjectStore:
    case Type_ObjectStoreKey:
      MOZ_ASSERT(mSourceObjectStore);
      aSource.SetAsIDBObjectStore() = mSourceObjectStore;
      return;

    case Type_Index:
    case Type_IndexKey:
      MOZ_ASSERT(mSourceIndex);
      aSource.SetAsIDBIndex() = mSourceIndex;
      return;

    default:
      MOZ_ASSUME_UNREACHABLE("Should never get here!");
  }
}

JS::Value
IDBCursor::GetKey(JSContext* aCx, ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  MOZ_ASSERT(!mKey.IsUnset() || !mHaveValue);

  if (!mHaveValue) {
    return JSVAL_VOID;
  }

  if (!mHaveCachedKey) {
    if (!mRooted) {
      mozilla::HoldJSObjects(this);
      mRooted = true;
    }

    aRv = mKey.ToJSVal(aCx, mCachedKey);
    if (NS_WARN_IF(aRv.Failed())) {
      return JSVAL_VOID;
    }

    mHaveCachedKey = true;
  }

  return mCachedKey;
}

JS::Value
IDBCursor::GetPrimaryKey(JSContext* aCx, ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (!mHaveValue) {
    return JSVAL_VOID;
  }

  if (!mHaveCachedPrimaryKey) {
    if (!mRooted) {
      mozilla::HoldJSObjects(this);
      mRooted = true;
    }

    const Key& key =
      (mType == Type_ObjectStore || mType == Type_ObjectStoreKey) ?
      mKey :
      mPrimaryKey;

    MOZ_ASSERT(!key.IsUnset());

    aRv = key.ToJSVal(aCx, mCachedPrimaryKey);
    if (NS_WARN_IF(aRv.Failed())) {
      return JSVAL_VOID;
    }

    mHaveCachedPrimaryKey = true;
  }

  return mCachedPrimaryKey;
}

JS::Value
IDBCursor::GetValue(JSContext* aCx, ErrorResult& aRv)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mType == Type_ObjectStore || mType == Type_Index);

  if (!mHaveValue) {
    return JSVAL_VOID;
  }

  if (!mHaveCachedValue) {
    if (!mRooted) {
      mozilla::HoldJSObjects(this);
      mRooted = true;
    }

    JS::Rooted<JS::Value> val(aCx);
    if (NS_WARN_IF(!IDBObjectStore::DeserializeValue(aCx, mCloneInfo, &val))) {
      aRv.Throw(NS_ERROR_DOM_DATA_CLONE_ERR);
      return JSVAL_VOID;
    }

    IDBObjectStore::ClearCloneReadInfo(mCloneInfo);

    mCachedValue = val;
    mHaveCachedValue = true;
  }

  return mCachedValue;
}

void
IDBCursor::Continue(JSContext* aCx,
                    JS::Handle<JS::Value> aKey,
                    ErrorResult &aRv)
{
  AssertIsOnOwningThread();

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return;
  }

  if (!mHaveValue || mContinueCalled) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return;
  }

  Key key;
  aRv = key.SetFromJSVal(aCx, aKey);
  if (aRv.Failed()) {
    return;
  }

  if (!key.IsUnset()) {
    switch (mDirection) {
      case NEXT:
      case NEXT_UNIQUE:
        if (key <= mKey) {
          aRv.Throw(NS_ERROR_DOM_INDEXEDDB_DATA_ERR);
          return;
        }
        break;

      case PREV:
      case PREV_UNIQUE:
        if (key >= mKey) {
          aRv.Throw(NS_ERROR_DOM_INDEXEDDB_DATA_ERR);
          return;
        }
        break;

      default:
        MOZ_ASSUME_UNREACHABLE("Should never get here!");
    }
  }

  mBackgroundActor->SendContinueInternal(ContinueParams(key));

  mContinueCalled = true;

#ifdef IDB_PROFILER_USE_MARKS
  if (mType == Type_ObjectStore || mType == Type_ObjectStoreKey) {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s).cursor(%s)."
                      "continue(%s)",
                      "IDBRequest[%llu] MT IDBCursor.continue()",
                      Request()->GetSerialNumber(),
                      IDB_PROFILER_STRING(Transaction()->Database()),
                      IDB_PROFILER_STRING(Transaction()),
                      IDB_PROFILER_STRING(mSourceObjectStore),
                      IDB_PROFILER_STRING(mDirection),
                      key.IsUnset() ? "" : IDB_PROFILER_STRING(key));
  } else {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s).index(%s)."
                      "cursor(%s).continue(%s)",
                      "IDBRequest[%llu] MT IDBCursor.continue()",
                      Request()->GetSerialNumber(),
                      IDB_PROFILER_STRING(Transaction()->Database()),
                      IDB_PROFILER_STRING(Transaction()),
                      IDB_PROFILER_STRING(mSourceIndex->ObjectStore()),
                      IDB_PROFILER_STRING(mSourceIndex),
                      IDB_PROFILER_STRING(mDirection),
                      key.IsUnset() ? "" : IDB_PROFILER_STRING(key));
  }
#endif
}

void
IDBCursor::Advance(uint32_t aCount, ErrorResult &aRv)
{
  AssertIsOnOwningThread();

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return;
  }

  if (!mHaveValue || mContinueCalled) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return;
  }

  if (!aCount) {
    aRv.ThrowTypeError(MSG_INVALID_ADVANCE_COUNT);
    return;
  }

  mBackgroundActor->SendContinueInternal(AdvanceParams(aCount));

  mContinueCalled = true;

#ifdef IDB_PROFILER_USE_MARKS
  {
    if (mType == Type_ObjectStore || mType == Type_ObjectStoreKey) {
      IDB_PROFILER_MARK("IndexedDB Request %llu: "
                        "database(%s).transaction(%s).objectStore(%s)."
                        "cursor(%s).advance(%ld)",
                        "IDBRequest[%llu] MT IDBCursor.advance()",
                        Request()->GetSerialNumber(),
                        IDB_PROFILER_STRING(Transaction()->Database()),
                        IDB_PROFILER_STRING(Transaction()),
                        IDB_PROFILER_STRING(mSourceObjectStore),
                        IDB_PROFILER_STRING(mDirection), aCount);
    } else {
      IDB_PROFILER_MARK("IndexedDB Request %llu: "
                        "database(%s).transaction(%s).objectStore(%s)."
                        "index(%s).cursor(%s).advance(%ld)",
                        "IDBRequest[%llu] MT IDBCursor.advance()",
                        Request()->GetSerialNumber(),
                        IDB_PROFILER_STRING(Transaction()->Database()),
                        IDB_PROFILER_STRING(Transaction()),
                        IDB_PROFILER_STRING(mSourceIndex->ObjectStore()),
                        IDB_PROFILER_STRING(mSourceIndex),
                        IDB_PROFILER_STRING(mDirection), aCount);
    }
  }
#endif
}

already_AddRefed<IDBRequest>
IDBCursor::Update(JSContext* aCx, JS::Handle<JS::Value> aValue,
                  ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  if (!mHaveValue || mType == Type_ObjectStoreKey || mType == Type_IndexKey) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  if (!mTransaction->IsWriteAllowed()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_READ_ONLY_ERR);
    return nullptr;
  }

  MOZ_ASSERT(mType == Type_ObjectStore || mType == Type_Index);
  MOZ_ASSERT(!mKey.IsUnset());
  MOZ_ASSERT_IF(mType == Type_Index, !mPrimaryKey.IsUnset());

  IDBObjectStore* objectStore =
    (mType == Type_ObjectStore) ?
    mSourceObjectStore :
    mSourceIndex->ObjectStore();
  MOZ_ASSERT(objectStore);

  const Key& primaryKey = (mType == Type_ObjectStore) ? mKey : mPrimaryKey;

  nsRefPtr<IDBRequest> request;

  if (objectStore->HasValidKeyPath()) {
    // Make sure the object given has the correct keyPath value set on it.
    const KeyPath& keyPath = objectStore->GetKeyPath();
    Key key;

    aRv = keyPath.ExtractKey(aCx, aValue, key);
    if (aRv.Failed()) {
      return nullptr;
    }

    if (key != primaryKey) {
      aRv.Throw(NS_ERROR_DOM_INDEXEDDB_DATA_ERR);
      return nullptr;
    }

    request = objectStore->Put(aCx, aValue, JS::UndefinedHandleValue, aRv);
    if (aRv.Failed()) {
      return nullptr;
    }
  }
  else {
    JS::Rooted<JS::Value> keyVal(aCx);
    aRv = primaryKey.ToJSVal(aCx, &keyVal);
    if (aRv.Failed()) {
      return nullptr;
    }

    request = objectStore->Put(aCx, aValue, keyVal, aRv);
    if (aRv.Failed()) {
      return nullptr;
    }
  }

  request->SetSource(this);

#ifdef IDB_PROFILER_USE_MARKS
  {
    uint64_t requestSerial = request->GetSerialNumber();
    if (mType == Type_ObjectStore) {
      IDB_PROFILER_MARK("IndexedDB Request %llu: "
                        "database(%s).transaction(%s).objectStore(%s)."
                        "cursor(%s).update(%s)",
                        "IDBRequest[%llu] MT IDBCursor.update()",
                        requestSerial,
                        IDB_PROFILER_STRING(mTransaction->Database()),
                        IDB_PROFILER_STRING(mTransaction),
                        IDB_PROFILER_STRING(objectStore),
                        IDB_PROFILER_STRING(mDirection),
                        mObjectStore->HasValidKeyPath() ? "" :
                          IDB_PROFILER_STRING(primaryKey));
    } else {
      IDB_PROFILER_MARK("IndexedDB Request %llu: "
                        "database(%s).transaction(%s).objectStore(%s)."
                        "index(%s).cursor(%s).update(%s)",
                        "IDBRequest[%llu] MT IDBCursor.update()",
                        requestSerial,
                        IDB_PROFILER_STRING(mTransaction->Database()),
                        IDB_PROFILER_STRING(mTransaction),
                        IDB_PROFILER_STRING(objectStore),
                        IDB_PROFILER_STRING(mSourceIndex),
                        IDB_PROFILER_STRING(mDirection),
                        mObjectStore->HasValidKeyPath() ? "" :
                          IDB_PROFILER_STRING(primaryKey));
    }
  }
#endif

  return request.forget();
}

already_AddRefed<IDBRequest>
IDBCursor::Delete(JSContext* aCx, ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  if (!mHaveValue || mType == Type_ObjectStoreKey || mType == Type_IndexKey) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  if (!mTransaction->IsWriteAllowed()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_READ_ONLY_ERR);
    return nullptr;
  }

  MOZ_ASSERT(mType == Type_ObjectStore || mType == Type_Index);
  MOZ_ASSERT(!mKey.IsUnset());

  IDBObjectStore* objectStore =
    (mType == Type_ObjectStore) ?
    mSourceObjectStore :
    mSourceIndex->ObjectStore();
  MOZ_ASSERT(objectStore);

  const Key& primaryKey = (mType == Type_ObjectStore) ? mKey : mPrimaryKey;

  JS::Rooted<JS::Value> key(aCx);
  aRv = primaryKey.ToJSVal(aCx, &key);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  nsRefPtr<IDBRequest> request = objectStore->Delete(aCx, key, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  request->SetSource(this);

#ifdef IDB_PROFILER_USE_MARKS
  {
    uint64_t requestSerial = request->GetSerialNumber();
    if (mType == Type_ObjectStore) {
      IDB_PROFILER_MARK("IndexedDB Request %llu: "
                        "database(%s).transaction(%s).objectStore(%s)."
                        "cursor(%s).delete(%s)",
                        "IDBRequest[%llu] MT IDBCursor.delete()",
                        requestSerial,
                        IDB_PROFILER_STRING(mTransaction->Database()),
                        IDB_PROFILER_STRING(mTransaction),
                        IDB_PROFILER_STRING(objectStore),
                        IDB_PROFILER_STRING(mDirection),
                        mObjectStore->HasValidKeyPath() ? "" :
                          IDB_PROFILER_STRING(primaryKey));
    } else {
      IDB_PROFILER_MARK("IndexedDB Request %llu: "
                        "database(%s).transaction(%s).objectStore(%s)."
                        "index(%s).cursor(%s).delete(%s)",
                        "IDBRequest[%llu] MT IDBCursor.delete()",
                        requestSerial,
                        IDB_PROFILER_STRING(mTransaction->Database()),
                        IDB_PROFILER_STRING(mTransaction),
                        IDB_PROFILER_STRING(objectStore),
                        IDB_PROFILER_STRING(mSourceIndex),
                        IDB_PROFILER_STRING(mDirection),
                        mObjectStore->HasValidKeyPath() ? "" :
                          IDB_PROFILER_STRING(primaryKey));
    }
  }
#endif

  return request.forget();
}

void
IDBCursor::Reset(Key&& aKey, StructuredCloneReadInfo&& aValue)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mType == Type_ObjectStore);

  Reset();

  mKey = Move(aKey);
  mCloneInfo = Move(aValue);
}

void
IDBCursor::Reset(Key&& aKey)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mType == Type_ObjectStoreKey);

  Reset();

  mKey = Move(aKey);
}

void
IDBCursor::Reset(Key&& aKey,
                 Key&& aPrimaryKey,
                 StructuredCloneReadInfo&& aValue)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mType == Type_Index);

  Reset();

  mKey = Move(aKey);
  mPrimaryKey = Move(aPrimaryKey);
  mCloneInfo = Move(aValue);
}

void
IDBCursor::Reset(Key&& aKey, Key&& aPrimaryKey)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mType == Type_IndexKey);

  Reset();

  mKey = Move(aKey);
  mPrimaryKey = Move(aPrimaryKey);
}

NS_IMPL_CYCLE_COLLECTING_ADDREF(IDBCursor)
NS_IMPL_CYCLE_COLLECTING_RELEASE(IDBCursor)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(IDBCursor)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTION_CLASS(IDBCursor)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(IDBCursor)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE_SCRIPT_OBJECTS
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mRequest)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN(IDBCursor)
  MOZ_ASSERT_IF(!tmp->mHaveCachedKey, tmp->mCachedKey.isUndefined());
  MOZ_ASSERT_IF(!tmp->mHaveCachedPrimaryKey,
                tmp->mCachedPrimaryKey.isUndefined());
  MOZ_ASSERT_IF(!tmp->mHaveCachedValue, tmp->mCachedValue.isUndefined());

  NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER
  NS_IMPL_CYCLE_COLLECTION_TRACE_JS_MEMBER_CALLBACK(mScriptOwner)
  NS_IMPL_CYCLE_COLLECTION_TRACE_JSVAL_MEMBER_CALLBACK(mCachedKey)
  NS_IMPL_CYCLE_COLLECTION_TRACE_JSVAL_MEMBER_CALLBACK(mCachedPrimaryKey)
  NS_IMPL_CYCLE_COLLECTION_TRACE_JSVAL_MEMBER_CALLBACK(mCachedValue)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(IDBCursor)
  // Don't unlink mRequest!
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER
  tmp->DropJSObjects();
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

JSObject*
IDBCursor::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope)
{
  AssertIsOnOwningThread();

  switch (mType) {
    case Type_ObjectStore:
    case Type_Index:
      return IDBCursorWithValueBinding::Wrap(aCx, aScope, this);

    case Type_ObjectStoreKey:
    case Type_IndexKey:
      return IDBCursorBinding::Wrap(aCx, aScope, this);

    default:
      MOZ_ASSUME_UNREACHABLE("Should never get here!");
  }
}
