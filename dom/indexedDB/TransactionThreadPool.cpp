/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TransactionThreadPool.h"

#include "IDBTransaction.h"
#include "mozilla/Monitor.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "nsComponentManagerUtils.h"
#include "nsIRunnable.h"
#include "nsISupportsPriority.h"
#include "nsIThreadPool.h"
#include "nsThreadUtils.h"
#include "nsServiceManagerUtils.h"
#include "nsXPCOMCIDInternal.h"
#include "ProfilerHelpers.h"

using namespace mozilla;
using namespace mozilla::dom::indexedDB;

using mozilla::ipc::AssertIsOnBackgroundThread;

namespace {

const uint32_t kThreadLimit = 20;
const uint32_t kIdleThreadLimit = 5;
const uint32_t kIdleThreadTimeoutMs = 30000;

// Only touched on the PBackground thread.
TransactionThreadPool* gThreadPool = nullptr;

// Only touched on the PBackground thread.
uint64_t gUniqueTransactionId = 0;

#if defined(DEBUG) || defined(MOZ_ENABLE_PROFILER_SPS)
#define BUILD_THREADPOOL_LISTENER
#endif

#ifdef DEBUG

const int32_t kDEBUGThreadPriority = nsISupportsPriority::PRIORITY_NORMAL;
const uint32_t kDEBUGThreadSleepMS = 0;

#endif // DEBUG

#ifdef BUILD_THREADPOOL_LISTENER

class TransactionThreadPoolListener MOZ_FINAL
  : public nsIThreadPoolListener
{
public:
  TransactionThreadPoolListener()
  { }

  NS_DECL_THREADSAFE_ISUPPORTS

private:
  virtual ~TransactionThreadPoolListener()
  { }

  NS_DECL_NSITHREADPOOLLISTENER
};

#endif // BUILD_THREADPOOL_LISTENER

} // anonymous namespace

class TransactionThreadPool::FinishTransactionRunnable MOZ_FINAL
  : public nsRunnable
{
  typedef TransactionThreadPool::FinishCallback FinishCallback;

  TransactionThreadPool* mThreadPool;
  nsRefPtr<FinishCallback> mFinishCallback;
  uint64_t mTransactionId;
  const nsCString mDatabaseId;
  const nsTArray<nsString> mObjectStoreNames;
  uint16_t mMode;

public:
  FinishTransactionRunnable(TransactionThreadPool* aThreadPool,
                            uint64_t aTransactionId,
                            const nsACString& aDatabaseId,
                            const nsTArray<nsString>& aObjectStoreNames,
                            uint16_t aMode,
                            already_AddRefed<FinishCallback> aFinishCallback);

  NS_DECL_ISUPPORTS_INHERITED

private:
  ~FinishTransactionRunnable()
  { }

  NS_DECL_NSIRUNNABLE
};

class TransactionThreadPool::CleanupRunnable MOZ_FINAL
  : public nsRunnable
{
  nsAutoPtr<TransactionThreadPool> mThreadPool;
  nsCOMPtr<nsIRunnable> mCallback;

public:
  CleanupRunnable(TransactionThreadPool* aThreadPool, nsIRunnable* aCallback)
    : mThreadPool(aThreadPool)
    , mCallback(aCallback)
  {
    MOZ_ASSERT(aThreadPool);
    aThreadPool->AssertIsOnOwningThread();
  }

  NS_IMETHOD
  Run()
  {
    mThreadPool->AssertIsOnOwningThread();

    if (NS_FAILED(mThreadPool->Cleanup())) {
      NS_WARNING("Failed to clean up thread pool!");
    }

    if (mCallback) {
      nsresult rv = mCallback->Run();
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
    }

    return NS_OK;
  }

private:
  ~CleanupRunnable()
  { }
};

struct TransactionThreadPool::DatabaseTransactionInfo MOZ_FINAL
{
  typedef nsClassHashtable<nsUint64HashKey, TransactionInfo>
    TransactionHashtable;
  TransactionHashtable transactions;
  nsClassHashtable<nsStringHashKey, TransactionInfoPair> blockingTransactions;

  DatabaseTransactionInfo()
  {
    MOZ_COUNT_CTOR(DatabaseTransactionInfo);
  }

  ~DatabaseTransactionInfo()
  {
    MOZ_COUNT_DTOR(DatabaseTransactionInfo);
  }
};

struct TransactionThreadPool::DatabasesCompleteCallback MOZ_FINAL
{
  friend class nsAutoPtr<DatabasesCompleteCallback>;

  nsTArray<nsCString> mDatabaseIds;
  nsCOMPtr<nsIRunnable> mCallback;

  DatabasesCompleteCallback()
  {
    MOZ_COUNT_CTOR(DatabasesCompleteCallback);
  }

private:
  ~DatabasesCompleteCallback()
  {
    MOZ_COUNT_DTOR(DatabasesCompleteCallback);
  }
};

class TransactionThreadPool::TransactionQueue MOZ_FINAL
  : public nsRunnable
{
  mozilla::Monitor mMonitor;

  TransactionThreadPool* mOwningThreadPool;
  nsCOMPtr<nsIEventTarget> mOwningThread;
  uint64_t mTransactionId;
  const nsCString mDatabaseId;
  const nsTArray<nsString> mObjectStoreNames;
  uint16_t mMode;

  nsAutoTArray<nsCOMPtr<nsIRunnable>, 10> mQueue;
  nsRefPtr<FinishCallback> mFinishCallback;
  bool mShouldFinish;

public:
  TransactionQueue(TransactionThreadPool* aThreadPool,
                    uint64_t aTransactionId,
                    const nsACString& aDatabaseId,
                    const nsTArray<nsString>& aObjectStoreNames,
                    uint16_t aMode);

  NS_DECL_ISUPPORTS_INHERITED

  void Unblock();

  void Dispatch(nsIRunnable* aRunnable);

  void Finish(FinishCallback* aFinishCallback);

private:
  ~TransactionQueue()
  { }

  NS_DECL_NSIRUNNABLE
};

struct TransactionThreadPool::TransactionInfo MOZ_FINAL
{
  uint64_t transactionId;
  nsCString databaseId;
  nsRefPtr<TransactionQueue> queue;
  nsTHashtable<nsPtrHashKey<TransactionInfo>> blockedOn;
  nsTHashtable<nsPtrHashKey<TransactionInfo>> blocking;

  TransactionInfo(TransactionThreadPool* aThreadPool,
                  uint64_t aTransactionId,
                  const nsACString& aDatabaseId,
                  const nsTArray<nsString>& aObjectStoreNames,
                  uint16_t aMode)
  : transactionId(aTransactionId), databaseId(aDatabaseId)
  {
    MOZ_COUNT_CTOR(TransactionInfo);

    queue = new TransactionQueue(aThreadPool, aTransactionId, aDatabaseId,
                                  aObjectStoreNames, aMode);
  }

  ~TransactionInfo()
  {
    MOZ_COUNT_DTOR(TransactionInfo);
  }
};

struct TransactionThreadPool::TransactionInfoPair MOZ_FINAL
{
  // Multiple reading transactions can block future writes.
  nsTArray<TransactionInfo*> lastBlockingWrites;
  // But only a single writing transaction can block future reads.
  TransactionInfo* lastBlockingReads;

  TransactionInfoPair()
    : lastBlockingReads(nullptr)
  {
    MOZ_COUNT_CTOR(TransactionInfoPair);
  }

  ~TransactionInfoPair()
  {
    MOZ_COUNT_DTOR(TransactionInfoPair);
  }
};

TransactionThreadPool::TransactionThreadPool()
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(!gThreadPool);

#ifdef DEBUG
  mDEBUGOwningPRThread = PR_GetCurrentThread();
#endif
  AssertIsOnOwningThread();
}

TransactionThreadPool::~TransactionThreadPool()
{
  AssertIsOnOwningThread();
}

#ifdef DEBUG

void
TransactionThreadPool::AssertIsOnOwningThread() const
{
  MOZ_ASSERT(mDEBUGOwningPRThread);
  MOZ_ASSERT(PR_GetCurrentThread() == mDEBUGOwningPRThread);
}

#endif // DEBUG

// static
TransactionThreadPool*
TransactionThreadPool::GetOrCreate()
{
  AssertIsOnBackgroundThread();

  if (!gThreadPool) {
    nsAutoPtr<TransactionThreadPool> pool(new TransactionThreadPool());
    pool->AssertIsOnOwningThread();

    if (NS_WARN_IF(NS_FAILED(pool->Init()))) {
      return nullptr;
    }

    gThreadPool = pool.forget();
  }

  return gThreadPool;
}

// static
TransactionThreadPool*
TransactionThreadPool::Get()
{
  AssertIsOnBackgroundThread();

  if (gThreadPool) {
    gThreadPool->AssertIsOnOwningThread();
    return gThreadPool;
  }

  return nullptr;
}

// static
void
TransactionThreadPool::Shutdown(nsIRunnable* aCallback)
{
  AssertIsOnBackgroundThread();

  if (gThreadPool) {
    gThreadPool->AssertIsOnOwningThread();

    nsRefPtr<CleanupRunnable> runnable =
      new CleanupRunnable(gThreadPool, aCallback);
    gThreadPool = nullptr;

    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToCurrentThread(runnable)));
  } else if (aCallback) {
    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToCurrentThread(aCallback)));
  }

  gUniqueTransactionId = 0;
}

uint64_t
TransactionThreadPool::NextTransactionId()
{
  AssertIsOnBackgroundThread();

  return ++gUniqueTransactionId;
}

nsresult
TransactionThreadPool::Init()
{
  AssertIsOnOwningThread();

  nsresult rv;
  mThreadPool = do_CreateInstance(NS_THREADPOOL_CONTRACTID, &rv);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = mThreadPool->SetName(NS_LITERAL_CSTRING("IndexedDB Trans"));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = mThreadPool->SetThreadLimit(kThreadLimit);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = mThreadPool->SetIdleThreadLimit(kIdleThreadLimit);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = mThreadPool->SetIdleThreadTimeout(kIdleThreadTimeoutMs);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

#ifdef BUILD_THREADPOOL_LISTENER
  nsCOMPtr<nsIThreadPoolListener> listener =
    new TransactionThreadPoolListener();

  rv = mThreadPool->SetListener(listener);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }
#endif // BUILD_THREADPOOL_LISTENER

  NS_WARN_IF_FALSE(!kDEBUGThreadSleepMS,
                   "TransactionThreadPool thread debugging enabled, sleeping "
                   "after every event!");

  return NS_OK;
}

nsresult
TransactionThreadPool::Cleanup()
{
  AssertIsOnOwningThread();

  nsresult rv = mThreadPool->Shutdown();
  NS_ENSURE_SUCCESS(rv, rv);

  nsIThread* currentThread = NS_GetCurrentThread();
  MOZ_ASSERT(currentThread);

  // Make sure the pool is still accessible while any callbacks generated from
  // the other threads are processed.
  rv = NS_ProcessPendingEvents(currentThread);
  NS_ENSURE_SUCCESS(rv, rv);

  if (!mCompleteCallbacks.IsEmpty()) {
    // Run all callbacks manually now.
    for (uint32_t index = 0; index < mCompleteCallbacks.Length(); index++) {
      MOZ_ASSERT(mCompleteCallbacks[index]);
      mCompleteCallbacks[index]->mCallback->Run();
    }
    mCompleteCallbacks.Clear();

    // And make sure they get processed.
    rv = NS_ProcessPendingEvents(currentThread);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

// static
PLDHashOperator
TransactionThreadPool::MaybeUnblockTransaction(nsPtrHashKey<TransactionInfo>* aKey,
                                               void* aUserArg)
{
  AssertIsOnBackgroundThread();

  TransactionInfo* maybeUnblockedInfo = aKey->GetKey();
  TransactionInfo* finishedInfo = static_cast<TransactionInfo*>(aUserArg);

  NS_ASSERTION(maybeUnblockedInfo->blockedOn.Contains(finishedInfo),
               "Huh?");
  maybeUnblockedInfo->blockedOn.RemoveEntry(finishedInfo);
  if (!maybeUnblockedInfo->blockedOn.Count()) {
    // Let this transaction run.
    maybeUnblockedInfo->queue->Unblock();
  }

  return PL_DHASH_NEXT;
}

void
TransactionThreadPool::FinishTransaction(
                                    uint64_t aTransactionId,
                                    const nsACString& aDatabaseId,
                                    const nsTArray<nsString>& aObjectStoreNames,
                                    uint16_t aMode)
{
  AssertIsOnOwningThread();

  PROFILER_LABEL("IndexedDB",
                 "TransactionThreadPool::FinishTransaction",
                 js::ProfileEntry::Category::STORAGE);

  DatabaseTransactionInfo* dbTransactionInfo;
  if (!mTransactionsInProgress.Get(aDatabaseId, &dbTransactionInfo)) {
    NS_ERROR("We don't know anyting about this database?!");
    return;
  }

  DatabaseTransactionInfo::TransactionHashtable& transactionsInProgress =
    dbTransactionInfo->transactions;

  uint32_t transactionCount = transactionsInProgress.Count();

#ifdef DEBUG
  if (aMode == IDBTransaction::VERSION_CHANGE) {
    NS_ASSERTION(transactionCount == 1,
                 "More transactions running than should be!");
  }
#endif

  if (transactionCount == 1) {
#ifdef DEBUG
    {
      const TransactionInfo* info = transactionsInProgress.Get(aTransactionId);
      NS_ASSERTION(info->transactionId == aTransactionId, "Transaction mismatch!");
    }
#endif
    mTransactionsInProgress.Remove(aDatabaseId);

    // See if we need to fire any complete callbacks.
    uint32_t index = 0;
    while (index < mCompleteCallbacks.Length()) {
      if (MaybeFireCallback(mCompleteCallbacks[index])) {
        mCompleteCallbacks.RemoveElementAt(index);
      } else {
        index++;
      }
    }

    return;
  }
  TransactionInfo* info = transactionsInProgress.Get(aTransactionId);
  NS_ASSERTION(info, "We've never heard of this transaction?!?");

  const nsTArray<nsString>& objectStoreNames = aObjectStoreNames;
  for (size_t index = 0, count = objectStoreNames.Length(); index < count;
       index++) {
    TransactionInfoPair* blockInfo =
      dbTransactionInfo->blockingTransactions.Get(objectStoreNames[index]);
    NS_ASSERTION(blockInfo, "Huh?");

    if (aMode == IDBTransaction::READ_WRITE &&
        blockInfo->lastBlockingReads == info) {
      blockInfo->lastBlockingReads = nullptr;
    }

    size_t i = blockInfo->lastBlockingWrites.IndexOf(info);
    if (i != blockInfo->lastBlockingWrites.NoIndex) {
      blockInfo->lastBlockingWrites.RemoveElementAt(i);
    }
  }

  info->blocking.EnumerateEntries(MaybeUnblockTransaction, info);

  transactionsInProgress.Remove(aTransactionId);
}

TransactionThreadPool::TransactionQueue*
TransactionThreadPool::GetQueueForTransaction(uint64_t aTransactionId,
                                              const nsACString& aDatabaseId)
{
  AssertIsOnOwningThread();

  DatabaseTransactionInfo* dbTransactionInfo;
  if (mTransactionsInProgress.Get(aDatabaseId, &dbTransactionInfo)) {
    DatabaseTransactionInfo::TransactionHashtable& transactionsInProgress =
      dbTransactionInfo->transactions;
    TransactionInfo* info = transactionsInProgress.Get(aTransactionId);
    if (info) {
      // We recognize this one.
      return info->queue;
    }
  }

  return nullptr;
}

TransactionThreadPool::TransactionQueue&
TransactionThreadPool::GetQueueForTransaction(
                                    uint64_t aTransactionId,
                                    const nsACString& aDatabaseId,
                                    const nsTArray<nsString>& aObjectStoreNames,
                                    uint16_t aMode)
{
  AssertIsOnOwningThread();

  TransactionQueue* existingQueue =
    GetQueueForTransaction(aTransactionId, aDatabaseId);
  if (existingQueue) {
    return *existingQueue;
  }

  // See if we can run this transaction now.
  DatabaseTransactionInfo* dbTransactionInfo;
  if (!mTransactionsInProgress.Get(aDatabaseId, &dbTransactionInfo)) {
    // First transaction for this database.
    dbTransactionInfo = new DatabaseTransactionInfo();
    mTransactionsInProgress.Put(aDatabaseId, dbTransactionInfo);
  }

  DatabaseTransactionInfo::TransactionHashtable& transactionsInProgress =
    dbTransactionInfo->transactions;
  TransactionInfo* info = transactionsInProgress.Get(aTransactionId);
  if (info) {
    // We recognize this one.
    return *info->queue;
  }

  TransactionInfo* transactionInfo = new TransactionInfo(this,
                                                         aTransactionId,
                                                         aDatabaseId,
                                                         aObjectStoreNames,
                                                         aMode);

  dbTransactionInfo->transactions.Put(aTransactionId, transactionInfo);;

  for (uint32_t index = 0, count = aObjectStoreNames.Length(); index < count;
       index++) {
    TransactionInfoPair* blockInfo =
      dbTransactionInfo->blockingTransactions.Get(aObjectStoreNames[index]);
    if (!blockInfo) {
      blockInfo = new TransactionInfoPair();
      blockInfo->lastBlockingReads = nullptr;
      dbTransactionInfo->blockingTransactions.Put(aObjectStoreNames[index],
                                                  blockInfo);
    }

    // Mark what we are blocking on.
    if (blockInfo->lastBlockingReads) {
      TransactionInfo* blockingInfo = blockInfo->lastBlockingReads;
      transactionInfo->blockedOn.PutEntry(blockingInfo);
      blockingInfo->blocking.PutEntry(transactionInfo);
    }

    if (aMode == IDBTransaction::READ_WRITE &&
        blockInfo->lastBlockingWrites.Length()) {
      for (uint32_t index = 0,
           count = blockInfo->lastBlockingWrites.Length(); index < count;
           index++) {
        TransactionInfo* blockingInfo = blockInfo->lastBlockingWrites[index];
        transactionInfo->blockedOn.PutEntry(blockingInfo);
        blockingInfo->blocking.PutEntry(transactionInfo);
      }
    }

    if (aMode == IDBTransaction::READ_WRITE) {
      blockInfo->lastBlockingReads = transactionInfo;
      blockInfo->lastBlockingWrites.Clear();
    }
    else {
      blockInfo->lastBlockingWrites.AppendElement(transactionInfo);
    }
  }

  if (!transactionInfo->blockedOn.Count()) {
    transactionInfo->queue->Unblock();
  }

  return *transactionInfo->queue;
}

nsresult
TransactionThreadPool::Dispatch(uint64_t aTransactionId,
                                const nsACString& aDatabaseId,
                                const nsTArray<nsString>& aObjectStoreNames,
                                uint16_t aMode,
                                nsIRunnable* aRunnable,
                                bool aFinish,
                                FinishCallback* aFinishCallback)
{
  TransactionQueue& queue = GetQueueForTransaction(aTransactionId,
                                                   aDatabaseId,
                                                   aObjectStoreNames,
                                                   aMode);

  queue.Dispatch(aRunnable);
  if (aFinish) {
    queue.Finish(aFinishCallback);
  }
  return NS_OK;
}

void
TransactionThreadPool::Dispatch(uint64_t aTransactionId,
                                const nsACString& aDatabaseId,
                                nsIRunnable* aRunnable,
                                bool aFinish,
                                FinishCallback* aFinishCallback)
{
  TransactionQueue* queue = GetQueueForTransaction(aTransactionId, aDatabaseId);
  if (!queue) {
    MOZ_CRASH("This transaction id was useless!");
  }

  queue->Dispatch(aRunnable);
  if (aFinish) {
    queue->Finish(aFinishCallback);
  }
}

void
TransactionThreadPool::WaitForDatabasesToComplete(
                                             nsTArray<nsCString>& aDatabaseIds,
                                             nsIRunnable* aCallback)
{
  AssertIsOnOwningThread();
  NS_ASSERTION(!aDatabaseIds.IsEmpty(), "No databases to wait on!");
  NS_ASSERTION(aCallback, "Null pointer!");

  nsAutoPtr<DatabasesCompleteCallback> callback =
    new DatabasesCompleteCallback();
  callback->mCallback = aCallback;
  callback->mDatabaseIds.SwapElements(aDatabaseIds);
  
  if (!MaybeFireCallback(callback)) {
    mCompleteCallbacks.AppendElement(callback.forget());
  }
}

// static
PLDHashOperator
TransactionThreadPool::CollectTransactions(const uint64_t& aTransactionId,
                                           TransactionInfo* aValue,
                                           void* aUserArg)
{
  nsAutoTArray<TransactionInfo*, 50>* transactionArray =
    static_cast<nsAutoTArray<TransactionInfo*, 50>*>(aUserArg);
  transactionArray->AppendElement(aValue);

  return PL_DHASH_NEXT;
}

struct MOZ_STACK_CLASS TransactionSearchInfo
{
  TransactionSearchInfo(const nsACString& aDatabaseId)
    : databaseId(aDatabaseId), found(false)
  {
  }

  nsCString databaseId;
  bool found;
};

// static
PLDHashOperator
TransactionThreadPool::FindTransaction(const uint64_t& aTransactionId,
                                       TransactionInfo* aValue,
                                       void* aUserArg)
{
  TransactionSearchInfo* info = static_cast<TransactionSearchInfo*>(aUserArg);

  if (aValue->databaseId == info->databaseId) {
    info->found = true;
    return PL_DHASH_STOP;
  }

  return PL_DHASH_NEXT;
}

bool
TransactionThreadPool::HasTransactionsForDatabase(const nsACString& aDatabaseId)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(!aDatabaseId.IsEmpty(), "An empty DatabaseId!");

  DatabaseTransactionInfo* dbTransactionInfo = nullptr;
  dbTransactionInfo = mTransactionsInProgress.Get(aDatabaseId);
  if (!dbTransactionInfo) {
    return false;
  }

  TransactionSearchInfo info(aDatabaseId);
  dbTransactionInfo->transactions.EnumerateRead(FindTransaction, &info);

  return info.found;
}

bool
TransactionThreadPool::MaybeFireCallback(DatabasesCompleteCallback* aCallback)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aCallback);
  MOZ_ASSERT(!aCallback->mDatabaseIds.IsEmpty());
  MOZ_ASSERT(aCallback->mCallback);

  PROFILER_LABEL("IndexedDB",
                 "TransactionThreadPool::MaybeFireCallback",
                 js::ProfileEntry::Category::STORAGE);

  for (uint32_t count = aCallback->mDatabaseIds.Length(), index = 0;
       index < count;
       index++) {
    const nsCString& databaseId = aCallback->mDatabaseIds[index];
    MOZ_ASSERT(!databaseId.IsEmpty());

    if (mTransactionsInProgress.Get(databaseId, nullptr)) {
      return false;
    }
  }

  aCallback->mCallback->Run();
  return true;
}

TransactionThreadPool::
TransactionQueue::TransactionQueue(TransactionThreadPool* aThreadPool,
                                   uint64_t aTransactionId,
                                   const nsACString& aDatabaseId,
                                   const nsTArray<nsString>& aObjectStoreNames,
                                   uint16_t aMode)
: mMonitor("TransactionQueue::mMonitor"),
  mOwningThreadPool(aThreadPool),
  mOwningThread(NS_GetCurrentThread()),
  mTransactionId(aTransactionId),
  mDatabaseId(aDatabaseId),
  mObjectStoreNames(aObjectStoreNames),
  mMode(aMode),
  mShouldFinish(false)
{
  MOZ_ASSERT(aThreadPool);
  aThreadPool->AssertIsOnOwningThread();
  MOZ_ASSERT(mOwningThread);
}

void
TransactionThreadPool::TransactionQueue::Unblock()
{
  MonitorAutoLock lock(mMonitor);

  // NB: Finish may be called before Unblock.

  TransactionThreadPool::Get()->mThreadPool-> Dispatch(this,
                                                       NS_DISPATCH_NORMAL);
}

void
TransactionThreadPool::TransactionQueue::Dispatch(nsIRunnable* aRunnable)
{
  MonitorAutoLock lock(mMonitor);

  NS_ASSERTION(!mShouldFinish, "Dispatch called after Finish!");

  mQueue.AppendElement(aRunnable);

  mMonitor.Notify();
}

void
TransactionThreadPool::TransactionQueue::Finish(FinishCallback* aFinishCallback)
{
  MonitorAutoLock lock(mMonitor);

  NS_ASSERTION(!mShouldFinish, "Finish called more than once!");

  mShouldFinish = true;
  mFinishCallback = aFinishCallback;

  mMonitor.Notify();
}

NS_IMPL_ISUPPORTS_INHERITED0(TransactionThreadPool::TransactionQueue,
                             nsRunnable)

NS_IMETHODIMP
TransactionThreadPool::TransactionQueue::Run()
{
  PROFILER_LABEL("IndexedDB",
                 "TransactionThreadPool::TransactionQueue""Run",
                 js::ProfileEntry::Category::STORAGE);

  IDB_PROFILER_MARK("IndexedDB Transaction %llu: Beginning database work",
                    "IDBTransaction[%llu] DT Start",
                    mTransaction->GetSerialNumber());

  nsAutoTArray<nsCOMPtr<nsIRunnable>, 10> queue;
  nsRefPtr<FinishCallback> FinishCallback;
  bool shouldFinish = false;

  do {
    NS_ASSERTION(queue.IsEmpty(), "Should have cleared this!");

    {
      MonitorAutoLock lock(mMonitor);
      while (!mShouldFinish && mQueue.IsEmpty()) {
        if (NS_FAILED(mMonitor.Wait())) {
          NS_ERROR("Failed to wait!");
        }
      }

      mQueue.SwapElements(queue);
      if (mShouldFinish) {
        mFinishCallback.swap(FinishCallback);
        shouldFinish = true;
      }
    }

    uint32_t count = queue.Length();
    for (uint32_t index = 0; index < count; index++) {
#ifdef DEBUG
      if (kDEBUGThreadSleepMS) {
        MOZ_ALWAYS_TRUE(
          PR_Sleep(PR_MillisecondsToInterval(kDEBUGThreadSleepMS)) ==
            PR_SUCCESS);
      }
#endif // DEBUG

      nsCOMPtr<nsIRunnable>& runnable = queue[index];
      runnable->Run();
      runnable = nullptr;
    }

    if (count) {
      queue.Clear();
    }
  } while (!shouldFinish);

  IDB_PROFILER_MARK("IndexedDB Transaction %llu: Finished database work",
                    "IDBTransaction[%llu] DT Done",
                    mTransaction->GetSerialNumber());

  nsCOMPtr<nsIRunnable> finishTransactionRunnable =
    new FinishTransactionRunnable(mOwningThreadPool, mTransactionId,
                                  mDatabaseId, mObjectStoreNames, mMode,
                                  FinishCallback.forget());
  if (NS_FAILED(mOwningThread->Dispatch(finishTransactionRunnable,
                                        NS_DISPATCH_NORMAL))) {
    NS_WARNING("Failed to dispatch finishTransactionRunnable!");
  }

  return NS_OK;
}

TransactionThreadPool::
FinishTransactionRunnable::FinishTransactionRunnable(
                               TransactionThreadPool* aThreadPool,
                               uint64_t aTransactionId,
                               const nsACString& aDatabaseId,
                               const nsTArray<nsString>& aObjectStoreNames,
                               uint16_t aMode,
                               already_AddRefed<FinishCallback> aFinishCallback)
: mThreadPool(aThreadPool),
  mFinishCallback(aFinishCallback),
  mTransactionId(aTransactionId),
  mDatabaseId(aDatabaseId),
  mObjectStoreNames(aObjectStoreNames),
  mMode(aMode)
{
  NS_ASSERTION(!NS_IsMainThread(), "Wrong thread!");
}

NS_IMPL_ISUPPORTS_INHERITED0(TransactionThreadPool::FinishTransactionRunnable,
                             nsRunnable)

NS_IMETHODIMP
TransactionThreadPool::
FinishTransactionRunnable::Run()
{
  MOZ_ASSERT(mThreadPool);
  mThreadPool->AssertIsOnOwningThread();

  PROFILER_LABEL("IndexedDB",
                 "TransactionThreadPool::FinishTransactionRunnable::Run",
                 js::ProfileEntry::Category::STORAGE);

  nsRefPtr<FinishCallback> callback;
  mFinishCallback.swap(callback);

  if (callback) {
    callback->TransactionFinishedBeforeUnblock();
  }

  mThreadPool->FinishTransaction(mTransactionId, mDatabaseId, mObjectStoreNames,
                                 mMode);

  if (callback) {
    callback->TransactionFinishedAfterUnblock();
  }

  return NS_OK;
}

#ifdef BUILD_THREADPOOL_LISTENER

NS_IMPL_ISUPPORTS(TransactionThreadPoolListener, nsIThreadPoolListener)

NS_IMETHODIMP
TransactionThreadPoolListener::OnThreadCreated()
{
  MOZ_ASSERT(!NS_IsMainThread());

#ifdef MOZ_ENABLE_PROFILER_SPS
  char aLocal;
  profiler_register_thread("IndexedDB Transaction", &aLocal);
#endif // MOZ_ENABLE_PROFILER_SPS

#ifdef DEBUG
  if (kDEBUGThreadPriority != nsISupportsPriority::PRIORITY_NORMAL) {
      NS_WARNING("TransactionThreadPool thread debugging enabled, priority has "
                 "been modified!");
      nsCOMPtr<nsISupportsPriority> thread =
        do_QueryInterface(NS_GetCurrentThread());
      MOZ_ASSERT(thread);

      MOZ_ALWAYS_TRUE(NS_SUCCEEDED(thread->SetPriority(kDEBUGThreadPriority)));
  }
#endif // DEBUG

  return NS_OK;
}

NS_IMETHODIMP
TransactionThreadPoolListener::OnThreadShuttingDown()
{
  MOZ_ASSERT(!NS_IsMainThread());

#ifdef MOZ_ENABLE_PROFILER_SPS
  profiler_unregister_thread();
#endif // MOZ_ENABLE_PROFILER_SPS

  return NS_OK;
}

#endif // BUILD_THREADPOOL_LISTENER
