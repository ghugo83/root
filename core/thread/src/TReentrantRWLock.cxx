// @(#)root/thread:$Id$
// Authors: Enric Tejedor CERN  12/09/2016
//          Philippe Canal FNAL 12/09/2016

/*************************************************************************
 * Copyright (C) 1995-2016, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

/** \class TReentrantRWLock
    \brief An implementation of a reentrant read-write lock with a
           configurable internal mutex/lock (default Spin Lock).

This class provides an implementation of a reentrant read-write lock
that uses an internal lock and a condition variable to synchronize
readers and writers when necessary.

The implementation allows a single reader to take the write lock without
releasing the reader lock.  It also allows the writer to take a read lock.
In other word, the lock is re-entrant for both reading and writing.

The implementation tries to make faster the scenario when readers come
and go but there is no writer. In that case, readers will not pay the
price of taking the internal spin lock.

Moreover, this RW lock tries to be fair with writers, giving them the
possibility to claim the lock and wait for only the remaining readers,
thus preventing starvation.
*/

#include "ROOT/TReentrantRWLock.hxx"
#include "ROOT/TSpinMutex.hxx"
#include "TMutex.h"
#include "TError.h"
#include <assert.h>

using namespace ROOT;

Internal::UniqueLockRecurseCount::UniqueLockRecurseCount()
{
   static bool singleton = false;
   if (singleton) {
      ::Fatal("UniqueLockRecurseCount Ctor", "Only one TReentrantRWLock using a UniqueLockRecurseCount is allowed.");
   }
   singleton = true;
}


////////////////////////////////////////////////////////////////////////////
/// Acquire the lock in read mode.
template <typename MutexT, typename RecurseCountsT>
TVirtualRWMutex::Hint_t *TReentrantRWLock<MutexT, RecurseCountsT>::ReadLock()
{
   ++fReaderReservation;

   // if (fReaders == std::numeric_limits<decltype(fReaders)>::max()) {
   //    ::Fatal("TRWSpinLock::WriteLock", "Too many recursions in TRWSpinLock!");
   // }

   auto local = fRecurseCounts.GetLocal();

   TVirtualRWMutex::Hint_t *hint = nullptr;

   if (!fWriter) {
      // There is no writer, go freely to the critical section
      ++fReaders;
      --fReaderReservation;

      hint = fRecurseCounts.IncrementReadCount(local, fMutex);

   } else if (! fRecurseCounts.IsNotCurrentWriter(local)) {

      --fReaderReservation;
      // This can run concurrently with another thread trying to get
      // the read lock and ending up in the next section ("Wait for writers, if any")
      // which need to also get the local readers count and thus can
      // modify the map.
      hint = fRecurseCounts.IncrementReadCount(local, fMutex);
      ++fReaders;

   } else {
      // A writer claimed the RW lock, we will need to wait on the
      // internal lock
      --fReaderReservation;

      std::unique_lock<MutexT> lock(fMutex);

      // Wait for writers, if any
      if (fWriter && fRecurseCounts.IsNotCurrentWriter(local)) {
         auto readerCount = fRecurseCounts.GetLocalReadersCount(local);
         if (readerCount == 0)
            fCond.wait(lock, [this] { return !fWriter; });
         // else
         //   There is a writer **but** we have outstanding readers
         //   locks, this must mean that the writer is actually
         //   waiting on this thread to release its read locks.
         //   This can be done in only two ways:
         //     * request the writer lock
         //     * release the reader lock
         //   Either way, this thread needs to proceed to
         //   be able to reach a point whether it does one
         //   of the two.
      }

      hint = fRecurseCounts.IncrementReadCount(local);

      // This RW lock now belongs to the readers
      ++fReaders;

      lock.unlock();
   }

   return hint;
}

//////////////////////////////////////////////////////////////////////////
/// Release the lock in read mode.
template <typename MutexT, typename RecurseCountsT>
void TReentrantRWLock<MutexT, RecurseCountsT>::ReadUnLock(TVirtualRWMutex::Hint_t *hint)
{
   size_t *localReaderCount;
   if (!hint) {
      // This should be very rare.
      auto local = fRecurseCounts.GetLocal();
      std::lock_guard<MutexT> lock(fMutex);
      localReaderCount = &(fRecurseCounts.GetLocalReadersCount(local));
   } else {
      localReaderCount = reinterpret_cast<size_t*>(hint);
   }

   --fReaders;
   if (fWriterReservation && fReaders == 0) {
      // We still need to lock here to prevent interleaving with a writer
      std::lock_guard<MutexT> lock(fMutex);

      --(*localReaderCount);

      // Make sure you wake up a writer, if any
      // Note: spurrious wakeups are okay, fReaders
      // will be checked again in WriteLock
      fCond.notify_all();
   } else {

      --(*localReaderCount);
   }
}

//////////////////////////////////////////////////////////////////////////
/// Acquire the lock in write mode.
template <typename MutexT, typename RecurseCountsT>
TVirtualRWMutex::Hint_t *TReentrantRWLock<MutexT, RecurseCountsT>::WriteLock()
{
   ++fWriterReservation;

   std::unique_lock<MutexT> lock(fMutex);

   auto local = fRecurseCounts.GetLocal();

   // Release this thread's reader lock(s)
   auto &readerCount = fRecurseCounts.GetLocalReadersCount(local);
   TVirtualRWMutex::Hint_t *hint = reinterpret_cast<TVirtualRWMutex::Hint_t *>(&readerCount);

   fReaders -= readerCount;

   // Wait for other writers, if any
   if (fWriter && fRecurseCounts.IsNotCurrentWriter(local)) {
      if (readerCount && fReaders == 0) {
         // we decrease fReaders to zero, let's wake up the
         // other writer.
         fCond.notify_all();
      }
      fCond.wait(lock, [this] { return !fWriter; });
   }

   // Claim the lock for this writer
   fWriter = true;
   fRecurseCounts.SetIsWriter(local);

   // Wait until all reader reservations finish
   while (fReaderReservation) {
   };

   // Wait for remaining readers
   fCond.wait(lock, [this] { return fReaders == 0; });

   // Restore this thread's reader lock(s)
   fReaders += readerCount;

   --fWriterReservation;

   lock.unlock();

   return hint;
}

//////////////////////////////////////////////////////////////////////////
/// Release the lock in write mode.
template <typename MutexT, typename RecurseCountsT>
void TReentrantRWLock<MutexT, RecurseCountsT>::WriteUnLock(TVirtualRWMutex::Hint_t *)
{
   // We need to lock here to prevent interleaving with a reader
   std::lock_guard<MutexT> lock(fMutex);

   if (!fWriter || fRecurseCounts.fWriteRecurse == 0) {
      Error("TReentrantRWLock::WriteUnLock", "Write lock already released for %p", this);
      return;
   }

   fRecurseCounts.DecrementWriteCount();

   if (!fRecurseCounts.fWriteRecurse) {
      fWriter = false;

      auto local = fRecurseCounts.GetLocal();
      fRecurseCounts.ResetIsWriter(local);

      // Notify all potential readers/writers that are waiting
      fCond.notify_all();
   }
}

namespace {
template <typename MutexT, typename RecurseCountsT>
struct TReentrantRWLockState: public TVirtualMutex::State {
    int fReadersCount = 0;
    size_t *fReadersCountLoc = nullptr;
    size_t fWriteRecurse = 0;
    bool fIsWriter = false;
};
}

template <typename MutexT, typename RecurseCountsT>
std::unique_ptr<TVirtualMutex::State> TReentrantRWLock<MutexT, RecurseCountsT>::Reset()
{
   using State_t = TReentrantRWLockState<MutexT, RecurseCountsT>;

   std::unique_ptr<State_t> pState(new State_t);
   auto local = fRecurseCounts.GetLocal();

   {
      std::unique_lock<MutexT> lock(fMutex);
      pState->fReadersCountLoc = &(fRecurseCounts.GetLocalReadersCount(local));
   }
   size_t &readerCount(*(pState->fReadersCountLoc));

   pState->fReadersCount = readerCount;

   if (fWriter && !fRecurseCounts.IsNotCurrentWriter(local)) {

      // We are holding the write lock.
      pState->fIsWriter = true;
      pState->fWriteRecurse = fRecurseCounts.fWriteRecurse;

      // Now set the lock (and potential read locks) for immediate release.
      fReaders -= readerCount;
      fRecurseCounts.fWriteRecurse = 1;

      *(pState->fReadersCountLoc) = 0;

      // Release this thread's write lock
      WriteUnLock(reinterpret_cast<TVirtualRWMutex::Hint_t *>(pState->fReadersCountLoc));
   } else if (readerCount) {
      // Now set the lock for release.
      fReaders -= (readerCount-1);
      *(pState->fReadersCountLoc) = 1;

      // Release this thread's reader lock(s)
      ReadUnLock(reinterpret_cast<TVirtualRWMutex::Hint_t *>(pState->fReadersCountLoc));
   }

   // Do something.
   //::Fatal("Reset()", "Not implemented, contact pcanal@fnal.gov");
   return std::move(pState);
}

template <typename MutexT, typename RecurseCountsT>
void TReentrantRWLock<MutexT, RecurseCountsT>::Restore(std::unique_ptr<TVirtualMutex::State> &&state)
{
   TReentrantRWLockState<MutexT, RecurseCountsT> *pState = dynamic_cast<TReentrantRWLockState<MutexT, RecurseCountsT> *>(state.get());
   if (!pState) {
      if (state) {
         SysError("Restore", "LOGIC ERROR - invalid state object!");
         return;
      }
      // No state, do nothing.
      return;
   }

   // At a restore point, this thread should not be holding any part
   // of the lock (if it does the following code will forget about it)
   assert( *(pState->fReadersCountLoc) == 0);
   // assert( auto local = fRecurseCounts.GetLocal() && fRecurseCounts.IsNotCurrentWriter(local) )

   const auto readerCount = pState->fReadersCount;

   if (pState->fIsWriter) {
      WriteLock();
      // Now that we go the lock, fix up the recursion count.
      std::unique_lock<MutexT> lock(fMutex);
      fRecurseCounts.fWriteRecurse = pState->fWriteRecurse;
      *(pState->fReadersCountLoc) = readerCount;
      fReaders +=  readerCount;
   } else if (readerCount) {
      ReadLock();
      // Now that we got the read lock, fix up the local recursion count.

      *(pState->fReadersCountLoc) = readerCount;
      fReaders += readerCount - 1;
   }

   // Do something.
   //::Fatal("Restore()", "Not implemented, contact pcanal@fnal.gov");
}

namespace ROOT {
template class TReentrantRWLock<ROOT::TSpinMutex, ROOT::Internal::RecurseCounts>;
template class TReentrantRWLock<TMutex, ROOT::Internal::RecurseCounts>;
template class TReentrantRWLock<std::mutex, ROOT::Internal::RecurseCounts>;

template class TReentrantRWLock<ROOT::TSpinMutex, ROOT::Internal::UniqueLockRecurseCount>;
template class TReentrantRWLock<TMutex, ROOT::Internal::UniqueLockRecurseCount>;
}
