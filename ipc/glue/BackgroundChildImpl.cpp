/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BackgroundChildImpl.h"

#include "mozilla/dom/indexedDB/ActorsChild.h"
#include "mozilla/ipc/PBackgroundTestChild.h"
#include "nsTraceRefcnt.h"

namespace indexedDB = mozilla::dom::indexedDB;

namespace {

class TestChild MOZ_FINAL : public mozilla::ipc::PBackgroundTestChild
{
  friend class mozilla::ipc::BackgroundChildImpl;

  nsCString mTestArg;

  TestChild(const nsCString& aTestArg)
  : mTestArg(aTestArg)
  {
    MOZ_COUNT_CTOR(mozilla::ipc::BackgroundTestChild);
  }

  ~TestChild()
  {
    MOZ_COUNT_DTOR(mozilla::ipc::BackgroundTestChild);
  }

  virtual bool
  Recv__delete__(const nsCString& aTestArg) MOZ_OVERRIDE;
};

} // anonymous namespace

namespace mozilla {
namespace ipc {

// -----------------------------------------------------------------------------
// BackgroundChildImpl::ThreadLocal
// -----------------------------------------------------------------------------

BackgroundChildImpl::
ThreadLocal::ThreadLocal()
: mCurrentTransaction(nullptr)
#ifdef MOZ_ENABLE_PROFILER_SPS
, mNextTransactionSerialNumber(1)
, mNextRequestSerialNumber(1)
#endif
{
  // May happen on any thread!
  MOZ_COUNT_CTOR(mozilla::ipc::BackgroundChildImpl::ThreadLocal);
}

BackgroundChildImpl::
ThreadLocal::~ThreadLocal()
{
  // May happen on any thread!
  MOZ_COUNT_DTOR(mozilla::ipc::BackgroundChildImpl::ThreadLocal);
}

// -----------------------------------------------------------------------------
// BackgroundChildImpl
// -----------------------------------------------------------------------------

BackgroundChildImpl::BackgroundChildImpl()
{
  // May happen on any thread!
  MOZ_COUNT_CTOR(mozilla::ipc::BackgroundChildImpl);
}

BackgroundChildImpl::~BackgroundChildImpl()
{
  // May happen on any thread!
  MOZ_COUNT_DTOR(mozilla::ipc::BackgroundChildImpl);
}

void
BackgroundChildImpl::ActorDestroy(ActorDestroyReason aWhy)
{
  // May happen on any thread!
}

PBackgroundTestChild*
BackgroundChildImpl::AllocPBackgroundTestChild(const nsCString& aTestArg)
{
  return new TestChild(aTestArg);
}

bool
BackgroundChildImpl::DeallocPBackgroundTestChild(PBackgroundTestChild* aActor)
{
  MOZ_ASSERT(aActor);

  delete static_cast<TestChild*>(aActor);
  return true;
}

BackgroundChildImpl::PBackgroundIDBFactoryChild*
BackgroundChildImpl::AllocPBackgroundIDBFactoryChild(
                                             const nsCString& aGroup,
                                             const nsCString& aASCIIOrigin,
                                             const StoragePrivilege& aPrivilege)
{
  MOZ_CRASH("PBackgroundIDBFactoryChild actors should be manually "
            "constructed!");
}

bool
BackgroundChildImpl::DeallocPBackgroundIDBFactoryChild(
                                             PBackgroundIDBFactoryChild* aActor)
{
  MOZ_ASSERT(aActor);

  delete static_cast<indexedDB::BackgroundFactoryChild*>(aActor);
  return true;
}

} // namespace ipc
} // namespace mozilla

bool
TestChild::Recv__delete__(const nsCString& aTestArg)
{
  MOZ_RELEASE_ASSERT(aTestArg == mTestArg,
                     "BackgroundTest message was corrupted!");

  return true;
}
