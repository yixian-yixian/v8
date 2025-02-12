// Copyright 2013 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/platform/mutex.h"

#include "src/base/platform/condition-variable.h"
#include "src/base/platform/time.h"

#if V8_OS_WIN
#include <windows.h>
#endif

#if DEBUG
#include <unordered_set>
#endif  // DEBUG

namespace v8 {
namespace base {

#if V8_OS_DARWIN

// TODO(verwaest): We should use the constants from the header, but they aren't
// exposed until macOS 15.
#define OS_UNFAIR_LOCK_DATA_SYNCHRONIZATION 0x00010000
#define OS_UNFAIR_LOCK_ADAPTIVE_SPIN 0x00040000

typedef uint32_t os_unfair_lock_options_t;

extern "C" {
void __attribute__((weak))
os_unfair_lock_lock_with_options(os_unfair_lock* lock,
                                 os_unfair_lock_options_t);
}

void SpinningMutex::Lock() {
  if (os_unfair_lock_lock_with_options) {
    const os_unfair_lock_options_t options =
        static_cast<os_unfair_lock_options_t>(
            OS_UNFAIR_LOCK_DATA_SYNCHRONIZATION | OS_UNFAIR_LOCK_ADAPTIVE_SPIN);
    os_unfair_lock_lock_with_options(&lock_, options);
  } else {
    os_unfair_lock_lock(&lock_);
  }
}

#else

void SpinningMutex::Lock() ABSL_NO_THREAD_SAFETY_ANALYSIS { lock_.Lock(); }

#endif

#if DEBUG
namespace {
// Used for asserts to guarantee we are not re-locking a mutex on the same
// thread. If this thread has only one held shared mutex (common case), we use
// {single_held_shared_mutex}. If it has more than one we allocate a set for it.
// Said set has to manually be constructed and destroyed.
thread_local base::SharedMutex* single_held_shared_mutex = nullptr;
using TSet = std::unordered_set<base::SharedMutex*>;
thread_local TSet* held_shared_mutexes = nullptr;

// Returns true iff {shared_mutex} is not a held mutex.
bool SharedMutexNotHeld(SharedMutex* shared_mutex) {
  DCHECK_NOT_NULL(shared_mutex);
  return single_held_shared_mutex != shared_mutex &&
         (!held_shared_mutexes ||
          held_shared_mutexes->count(shared_mutex) == 0);
}

// Tries to hold {shared_mutex}. Returns true iff it hadn't been held prior to
// this function call.
bool TryHoldSharedMutex(SharedMutex* shared_mutex) {
  DCHECK_NOT_NULL(shared_mutex);
  if (single_held_shared_mutex) {
    if (shared_mutex == single_held_shared_mutex) {
      return false;
    }
    DCHECK_NULL(held_shared_mutexes);
    held_shared_mutexes = new TSet({single_held_shared_mutex, shared_mutex});
    single_held_shared_mutex = nullptr;
    return true;
  } else if (held_shared_mutexes) {
    return held_shared_mutexes->insert(shared_mutex).second;
  } else {
    DCHECK_NULL(single_held_shared_mutex);
    single_held_shared_mutex = shared_mutex;
    return true;
  }
}

// Tries to release {shared_mutex}. Returns true iff it had been held prior to
// this function call.
bool TryReleaseSharedMutex(SharedMutex* shared_mutex) {
  DCHECK_NOT_NULL(shared_mutex);
  if (single_held_shared_mutex == shared_mutex) {
    single_held_shared_mutex = nullptr;
    return true;
  }
  if (held_shared_mutexes && held_shared_mutexes->erase(shared_mutex)) {
    if (held_shared_mutexes->empty()) {
      delete held_shared_mutexes;
      held_shared_mutexes = nullptr;
    }
    return true;
  }
  return false;
}
}  // namespace
#endif  // DEBUG

#if V8_OS_POSIX

static V8_INLINE void InitializeRecursiveNativeHandle(pthread_mutex_t* mutex) {
  pthread_mutexattr_t attr;
  int result = pthread_mutexattr_init(&attr);
  DCHECK_EQ(0, result);
  result = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  DCHECK_EQ(0, result);
  result = pthread_mutex_init(mutex, &attr);
  DCHECK_EQ(0, result);
  result = pthread_mutexattr_destroy(&attr);
  DCHECK_EQ(0, result);
  USE(result);
}


static V8_INLINE void DestroyNativeHandle(pthread_mutex_t* mutex) {
  int result = pthread_mutex_destroy(mutex);
  DCHECK_EQ(0, result);
  USE(result);
}


static V8_INLINE void LockNativeHandle(pthread_mutex_t* mutex) {
  int result = pthread_mutex_lock(mutex);
  DCHECK_EQ(0, result);
  USE(result);
}


static V8_INLINE void UnlockNativeHandle(pthread_mutex_t* mutex) {
  int result = pthread_mutex_unlock(mutex);
  DCHECK_EQ(0, result);
  USE(result);
}


static V8_INLINE bool TryLockNativeHandle(pthread_mutex_t* mutex) {
  int result = pthread_mutex_trylock(mutex);
  if (result == EBUSY) {
    return false;
  }
  DCHECK_EQ(0, result);
  return true;
}

RecursiveMutex::RecursiveMutex() {
  InitializeRecursiveNativeHandle(&native_handle_);
#ifdef DEBUG
  level_ = 0;
#endif
}


RecursiveMutex::~RecursiveMutex() {
  DestroyNativeHandle(&native_handle_);
  DCHECK_EQ(0, level_);
}


void RecursiveMutex::Lock() {
  LockNativeHandle(&native_handle_);
#ifdef DEBUG
  DCHECK_LE(0, level_);
  level_++;
#endif
}


void RecursiveMutex::Unlock() {
#ifdef DEBUG
  DCHECK_LT(0, level_);
  level_--;
#endif
  UnlockNativeHandle(&native_handle_);
}


bool RecursiveMutex::TryLock() {
  if (!TryLockNativeHandle(&native_handle_)) {
    return false;
  }
#ifdef DEBUG
  DCHECK_LE(0, level_);
  level_++;
#endif
  return true;
}

#elif V8_OS_WIN

RecursiveMutex::RecursiveMutex() {
  InitializeCriticalSection(V8ToWindowsType(&native_handle_));
#ifdef DEBUG
  level_ = 0;
#endif
}


RecursiveMutex::~RecursiveMutex() {
  DeleteCriticalSection(V8ToWindowsType(&native_handle_));
  DCHECK_EQ(0, level_);
}


void RecursiveMutex::Lock() {
  EnterCriticalSection(V8ToWindowsType(&native_handle_));
#ifdef DEBUG
  DCHECK_LE(0, level_);
  level_++;
#endif
}


void RecursiveMutex::Unlock() {
#ifdef DEBUG
  DCHECK_LT(0, level_);
  level_--;
#endif
  LeaveCriticalSection(V8ToWindowsType(&native_handle_));
}


bool RecursiveMutex::TryLock() {
  if (!TryEnterCriticalSection(V8ToWindowsType(&native_handle_))) {
    return false;
  }
#ifdef DEBUG
  DCHECK_LE(0, level_);
  level_++;
#endif
  return true;
}

#elif V8_OS_STARBOARD

RecursiveMutex::RecursiveMutex() {}

RecursiveMutex::~RecursiveMutex() {}

void RecursiveMutex::Lock() { native_handle_.Acquire(); }

void RecursiveMutex::Unlock() { native_handle_.Release(); }

bool RecursiveMutex::TryLock() { return native_handle_.AcquireTry(); }

#endif  // V8_OS_STARBOARD

SharedMutex::SharedMutex() = default;
SharedMutex::~SharedMutex() = default;

void SharedMutex::LockShared() ABSL_SHARED_LOCK_FUNCTION(native_handle_) {
  DCHECK(TryHoldSharedMutex(this));
  native_handle_.ReaderLock();
}

void SharedMutex::LockExclusive() ABSL_EXCLUSIVE_LOCK_FUNCTION(native_handle_) {
  DCHECK(TryHoldSharedMutex(this));
  native_handle_.Lock();
}

void SharedMutex::UnlockShared() ABSL_UNLOCK_FUNCTION(native_handle_) {
  DCHECK(TryReleaseSharedMutex(this));
  native_handle_.ReaderUnlock();
}

void SharedMutex::UnlockExclusive() ABSL_UNLOCK_FUNCTION(native_handle_) {
  DCHECK(TryReleaseSharedMutex(this));
  native_handle_.Unlock();
}

bool SharedMutex::TryLockShared()
    ABSL_SHARED_TRYLOCK_FUNCTION(true, native_handle_) {
  DCHECK(SharedMutexNotHeld(this));
  bool result = native_handle_.ReaderTryLock();
  if (result) DCHECK(TryHoldSharedMutex(this));
  return result;
}

bool SharedMutex::TryLockExclusive()
    ABSL_EXCLUSIVE_TRYLOCK_FUNCTION(true, native_handle_) {
  DCHECK(SharedMutexNotHeld(this));
  bool result = native_handle_.TryLock();
  if (result) DCHECK(TryHoldSharedMutex(this));
  return result;
}

Mutex::Mutex() {
#ifdef DEBUG
  level_ = 0;
#endif
}

Mutex::~Mutex() { DCHECK_EQ(0, level_); }

void Mutex::Lock() ABSL_EXCLUSIVE_LOCK_FUNCTION(native_handle_) {
  native_handle_.Lock();
  AssertUnheldAndMark();
}

void Mutex::Unlock() ABSL_UNLOCK_FUNCTION(native_handle_) {
  AssertHeldAndUnmark();
  native_handle_.Unlock();
}

bool Mutex::TryLock() ABSL_EXCLUSIVE_TRYLOCK_FUNCTION(true, native_handle_) {
  if (!native_handle_.TryLock()) return false;
  AssertUnheldAndMark();
  return true;
}

#ifdef V8_OS_DARWIN
SpinningMutex::SpinningMutex() : lock_(OS_UNFAIR_LOCK_INIT) {}
#else
SpinningMutex::SpinningMutex() = default;
#endif

}  // namespace base
}  // namespace v8
