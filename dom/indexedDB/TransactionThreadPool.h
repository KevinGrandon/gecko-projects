/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_transactionthreadpool_h__
#define mozilla_dom_indexeddb_transactionthreadpool_h__

// Only meant to be included in IndexedDB source files, not exported.
#include "IndexedDatabase.h"

#include "nsIRunnable.h"

#include "mozilla/Monitor.h"
#include "nsClassHashtable.h"
#include "nsHashKeys.h"

class nsIEventTarget;
class nsIThreadPool;

BEGIN_INDEXEDDB_NAMESPACE

class FinishTransactionRunnable;
class QueuedDispatchInfo;

class TransactionThreadPool
{
  friend class nsAutoPtr<TransactionThreadPool>;
  friend class FinishTransactionRunnable;

  class CleanupRunnable;

public:
  // returns a non-owning ref!
  static TransactionThreadPool* GetOrCreate();

  // returns a non-owning ref!
  static TransactionThreadPool* Get();

  static void Shutdown();

  static uint64_t NextTransactionId();

  nsresult Dispatch(uint64_t aTransactionId,
                    const nsACString& aDatabaseId,
                    const nsTArray<nsString>& aObjectStoreNames,
                    uint16_t aMode,
                    nsIRunnable* aRunnable,
                    bool aFinish,
                    nsIRunnable* aFinishRunnable);

  void Dispatch(uint64_t aTransactionId,
                const nsACString& aDatabaseId,
                nsIRunnable* aRunnable,
                bool aFinish,
                nsIRunnable* aFinishRunnable);

  void WaitForDatabasesToComplete(nsTArray<nsCString>& aDatabaseIds,
                                  nsIRunnable* aCallback);

  // Returns true if there are running or pending transactions for aDatabase.
  bool HasTransactionsForDatabase(const nsACString& aDatabaseId);

protected:
  class TransactionQueue MOZ_FINAL : public nsIRunnable
  {
  public:
    NS_DECL_THREADSAFE_ISUPPORTS
    NS_DECL_NSIRUNNABLE

    TransactionQueue(TransactionThreadPool* aThreadPool,
                     uint64_t aTransactionId,
                     const nsACString& aDatabaseId,
                     const nsTArray<nsString>& aObjectStoreNames,
                     uint16_t aMode);

    void Unblock();

    void Dispatch(nsIRunnable* aRunnable);

    void Finish(nsIRunnable* aFinishRunnable);

  private:
    mozilla::Monitor mMonitor;

    TransactionThreadPool* mOwningThreadPool;
    nsCOMPtr<nsIEventTarget> mOwningThread;
    uint64_t mTransactionId;
    const nsCString mDatabaseId;
    const nsTArray<nsString> mObjectStoreNames;
    uint16_t mMode;

    nsAutoTArray<nsCOMPtr<nsIRunnable>, 10> mQueue;
    nsCOMPtr<nsIRunnable> mFinishRunnable;
    bool mShouldFinish;
  };

  friend class TransactionQueue;

  struct TransactionInfo
  {
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

    uint64_t transactionId;
    nsCString databaseId;
    nsRefPtr<TransactionQueue> queue;
    nsTHashtable<nsPtrHashKey<TransactionInfo>> blockedOn;
    nsTHashtable<nsPtrHashKey<TransactionInfo>> blocking;
  };

  struct TransactionInfoPair
  {
    TransactionInfoPair()
      : lastBlockingReads(nullptr)
    {
      MOZ_COUNT_CTOR(TransactionInfoPair);
    }

    ~TransactionInfoPair()
    {
      MOZ_COUNT_DTOR(TransactionInfoPair);
    }
    // Multiple reading transactions can block future writes.
    nsTArray<TransactionInfo*> lastBlockingWrites;
    // But only a single writing transaction can block future reads.
    TransactionInfo* lastBlockingReads;
  };

  struct DatabaseTransactionInfo
  {
    DatabaseTransactionInfo()
    {
      MOZ_COUNT_CTOR(DatabaseTransactionInfo);
    }

    ~DatabaseTransactionInfo()
    {
      MOZ_COUNT_DTOR(DatabaseTransactionInfo);
    }

    typedef nsClassHashtable<nsUint64HashKey, TransactionInfo>
      TransactionHashtable;
    TransactionHashtable transactions;
    nsClassHashtable<nsStringHashKey, TransactionInfoPair> blockingTransactions;
  };

  static PLDHashOperator
  CollectTransactions(const uint64_t& aTransactionId,
                      TransactionInfo* aValue,
                      void* aUserArg);

  static PLDHashOperator
  FindTransaction(const uint64_t& aTransactionId,
                  TransactionInfo* aValue,
                  void* aUserArg);

  static PLDHashOperator
  MaybeUnblockTransaction(nsPtrHashKey<TransactionInfo>* aKey,
                          void* aUserArg);

  struct DatabasesCompleteCallback
  {
    nsTArray<nsCString> mDatabaseIds;
    nsCOMPtr<nsIRunnable> mCallback;
  };

  TransactionThreadPool();
  ~TransactionThreadPool();

  nsresult Init();
  nsresult Cleanup();

  void FinishTransaction(uint64_t aTransactionId,
                         const nsACString& aDatabaseId,
                         const nsTArray<nsString>& aObjectStoreNames,
                         uint16_t aMode);

  TransactionQueue* GetQueueForTransaction(uint64_t aTransactionId,
                                           const nsACString& aDatabaseId);

  TransactionQueue& GetQueueForTransaction(
                                    uint64_t aTransactionId,
                                    const nsACString& aDatabaseId,
                                    const nsTArray<nsString>& aObjectStoreNames,
                                    uint16_t aMode);

  bool MaybeFireCallback(DatabasesCompleteCallback aCallback);

  nsCOMPtr<nsIThreadPool> mThreadPool;

  nsClassHashtable<nsCStringHashKey, DatabaseTransactionInfo>
    mTransactionsInProgress;

  nsTArray<DatabasesCompleteCallback> mCompleteCallbacks;
};

END_INDEXEDDB_NAMESPACE

#endif // mozilla_dom_indexeddb_transactionthreadpool_h__
