/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef threading_ProtectedData_h
#define threading_ProtectedData_h

#include "threading/Thread.h"

namespace js {

// This file provides classes for encapsulating pieces of data with a check
// that ensures the data is only accessed if certain conditions are met.
// Checking is only done in debug builds; in release builds these classes
// have no space or time overhead. These classes are mainly used for ensuring
// that data is used in threadsafe ways.
//
// ProtectedData does not by itself ensure that data is threadsafe: it only
// documents and checks synchronization constraints that need to be established
// by the code using the data. If a mutex can be created and directly
// associated with the data, consider using the ExclusiveData class instead.
// Otherwise, ProtectedData should be used to document whatever synchronization
// method is used.
//
// Note that several of the checks below are currently disabled
// (e.g. ThreadLocalData, ZoneGroupData). These will be fixed soon (bug 1323066),
// but for now this class is largely documenting how we would like data to be
// protected, rather than how it actually is protected.

#define DECLARE_ONE_BOOL_OPERATOR(OP, T)        \
    template <typename U>                       \
    bool operator OP(const U& other) const { return ref() OP static_cast<T>(other); }

#define DECLARE_BOOL_OPERATORS(T)               \
    DECLARE_ONE_BOOL_OPERATOR(==, T)            \
    DECLARE_ONE_BOOL_OPERATOR(!=, T)            \
    DECLARE_ONE_BOOL_OPERATOR(<=, T)            \
    DECLARE_ONE_BOOL_OPERATOR(>=, T)            \
    DECLARE_ONE_BOOL_OPERATOR(<, T)             \
    DECLARE_ONE_BOOL_OPERATOR(>, T)

// Mark a region of code that should be treated as single threaded and suppress
// any ProtectedData checks.
//
// Note that in practice there may be multiple threads running when this class
// is used, due to the presence of multiple runtimes in the process. When each
// process has only a single runtime this will no longer be a concern.
class MOZ_RAII AutoNoteSingleThreadedRegion
{
  public:
#ifdef DEBUG
    static mozilla::Atomic<size_t> count;
    AutoNoteSingleThreadedRegion() { count++; }
    ~AutoNoteSingleThreadedRegion() { count--; }
#else
    AutoNoteSingleThreadedRegion() {}
#endif
};

// Class for protected data that may be written to any number of times. Checks
// occur when the data is both read from and written to.
template <typename Check, typename T>
class ProtectedData
{
    typedef ProtectedData<Check, T> ThisType;

  public:
    template <typename... Args>
    explicit ProtectedData(const Check& check, Args&&... args)
      : value(mozilla::Forward<Args>(args)...)
#ifdef DEBUG
      , check(check)
#endif
    {}

    DECLARE_BOOL_OPERATORS(T)

    operator const T&() const { return ref(); }
    const T& operator->() const { return ref(); }

    template <typename U>
    ThisType& operator=(const U& p) { this->ref() = p; return *this; }

    template <typename U> T& operator +=(const U& rhs) { return ref() += rhs; }
    template <typename U> T& operator -=(const U& rhs) { return ref() -= rhs; }
    template <typename U> T& operator *=(const U& rhs) { return ref() *= rhs; }
    template <typename U> T& operator /=(const U& rhs) { return ref() /= rhs; }
    template <typename U> T& operator &=(const U& rhs) { return ref() &= rhs; }
    template <typename U> T& operator |=(const U& rhs) { return ref() |= rhs; }
    T& operator ++() { return ++ref(); }
    T& operator --() { return --ref(); }
    T operator ++(int) { return ref()++; }
    T operator --(int) { return ref()--; }

    T& ref() {
#ifdef DEBUG
        if (!AutoNoteSingleThreadedRegion::count)
            check.check();
#endif
        return value;
    }

    const T& ref() const {
#ifdef DEBUG
        if (!AutoNoteSingleThreadedRegion::count)
            check.check();
#endif
        return value;
    }

    T& refNoCheck() { return value; }
    const T& refNoCheck() const { return value; }

  private:
    T value;
#ifdef DEBUG
    Check check;
#endif
};

// Intermediate class for protected data whose checks take no constructor arguments.
template <typename Check, typename T>
class ProtectedDataNoCheckArgs : public ProtectedData<Check, T>
{
    typedef ProtectedDataNoCheckArgs<Check, T> ThisType;

  public:
    template <typename... Args>
    explicit ProtectedDataNoCheckArgs(Args&&... args)
      : ProtectedData<Check, T>(Check(), mozilla::Forward<Args>(args)...)
    {}

    template <typename U>
    ThisType& operator=(const U& p) { this->ref() = p; return *this; }
};

class ZoneGroup;

// Intermediate class for protected data whose checks take a ZoneGroup constructor argument.
template <typename Check, typename T>
class ProtectedDataZoneGroupArg : public ProtectedData<Check, T>
{
    typedef ProtectedDataZoneGroupArg<Check, T> ThisType;

  public:
    template <typename... Args>
    explicit ProtectedDataZoneGroupArg(ZoneGroup* group, Args&&... args)
      : ProtectedData<Check, T>(Check(group), mozilla::Forward<Args>(args)...)
    {}

    template <typename U>
    ThisType& operator=(const U& p) { this->ref() = p; return *this; }
};

class CheckUnprotected
{
#ifdef DEBUG
  public:
    inline void check() const {}
#endif
};

// Data with a no-op check that permits all accesses. This is tantamount to not
// using ProtectedData at all, but is in place to document points which need
// to be fixed in order for runtimes to be multithreaded (see bug 1323066).
template <typename T>
using UnprotectedData = ProtectedDataNoCheckArgs<CheckUnprotected, T>;

class CheckThreadLocal
{
#ifdef DEBUG
    Thread::Id id;

  public:
    CheckThreadLocal()
      : id(ThisThread::GetId())
    {}

    inline void check() const {
        // This check is currently disabled because JSContexts used for off
        // thread parsing are created on different threads than where they run.
        // This will be fixed soon (bug 1323066).
        //MOZ_ASSERT(id == ThisThread::GetId());
    }
#endif
};

// Data which may only be accessed by the thread on which it is created.
template <typename T>
using ThreadLocalData = ProtectedDataNoCheckArgs<CheckThreadLocal, T>;

// Enum describing which background threads (GC tasks or Ion compilations) may
// access data even though they do not have exclusive access to any zone group.
enum class AllowedBackgroundThread
{
    None,
    GCTask,
    IonCompile,
    GCTaskOrIonCompile
};

template <AllowedBackgroundThread Background>
class CheckZoneGroup
{
#ifdef DEBUG
    ZoneGroup* group;

  public:
    explicit CheckZoneGroup(ZoneGroup* group) : group(group) {}
    void check() const;
#else
  public:
    explicit CheckZoneGroup(ZoneGroup* group) {}
#endif
};

// Data which may only be accessed by threads with exclusive access to the
// associated zone group.
template <typename T>
using ZoneGroupData =
    ProtectedDataZoneGroupArg<CheckZoneGroup<AllowedBackgroundThread::None>, T>;

// Data which may only be accessed by threads with exclusive access to the
// associated zone group, or by GC helper thread tasks.
template <typename T>
using ZoneGroupOrGCTaskData =
    ProtectedDataZoneGroupArg<CheckZoneGroup<AllowedBackgroundThread::GCTask>, T>;

// Data which may only be accessed by threads with exclusive access to the
// associated zone group, or by Ion compilation helper thread tasks.
template <typename T>
using ZoneGroupOrIonCompileData =
    ProtectedDataZoneGroupArg<CheckZoneGroup<AllowedBackgroundThread::IonCompile>, T>;

// Data which may only be accessed by threads with exclusive access to the
// associated zone group, or by either GC helper or Ion compilation tasks.
template <typename T>
using ZoneGroupOrGCTaskOrIonCompileData =
    ProtectedDataZoneGroupArg<CheckZoneGroup<AllowedBackgroundThread::GCTaskOrIonCompile>, T>;

// Runtime wide locks which might protect some data.
enum class GlobalLock
{
    GCLock,
    ExclusiveAccessLock,
    HelperThreadLock
};

template <GlobalLock Lock, AllowedBackgroundThread Background>
class CheckGlobalLock
{
#ifdef DEBUG
  public:
    void check() const;
#endif
};

// Data which may only be accessed while holding the GC lock.
template <typename T>
using GCLockData =
    ProtectedDataNoCheckArgs<CheckGlobalLock<GlobalLock::GCLock, AllowedBackgroundThread::None>, T>;

// Data which may only be accessed while holding the exclusive access lock.
template <typename T>
using ExclusiveAccessLockData =
    ProtectedDataNoCheckArgs<CheckGlobalLock<GlobalLock::ExclusiveAccessLock, AllowedBackgroundThread::None>, T>;

// Data which may only be accessed while holding the exclusive access lock or
// by GC helper thread tasks (at which point a foreground thread should be
// holding the exclusive access lock, though we do not check this).
template <typename T>
using ExclusiveAccessLockOrGCTaskData =
    ProtectedDataNoCheckArgs<CheckGlobalLock<GlobalLock::ExclusiveAccessLock, AllowedBackgroundThread::GCTask>, T>;

// Data which may only be accessed while holding the helper thread lock.
template <typename T>
using HelperThreadLockData =
    ProtectedDataNoCheckArgs<CheckGlobalLock<GlobalLock::HelperThreadLock, AllowedBackgroundThread::None>, T>;

// Class for protected data that is only written to once. 'const' may sometimes
// be usable instead of this class, but in cases where the data cannot be set
// to its final value in its constructor this class is helpful. Protected data
// checking only occurs when writes are performed, not reads. Steps may need to
// be taken to ensure that reads do not occur until the written value is fully
// initialized, as such guarantees are not provided by this class.
template <typename Check, typename T>
class ProtectedDataWriteOnce
{
    typedef ProtectedDataWriteOnce<Check, T> ThisType;

  public:
    template <typename... Args>
    explicit ProtectedDataWriteOnce(Args&&... args)
      : value(mozilla::Forward<Args>(args)...)
#ifdef DEBUG
      , nwrites(0)
#endif
    {}

    DECLARE_BOOL_OPERATORS(T)

    operator const T&() const { return ref(); }
    const T& operator->() const { return ref(); }

    template <typename U>
    ThisType& operator=(const U& p) { this->writeRef() = p; return *this; }

    const T& ref() const { return value; }

    T& writeRef() {
#ifdef DEBUG
        if (!AutoNoteSingleThreadedRegion::count)
            check.check();
        // Despite the WriteOnce name, actually allow two writes to accommodate
        // data that is cleared during teardown.
        MOZ_ASSERT(++nwrites <= 2);
#endif
        return value;
    }

  private:
    T value;
#ifdef DEBUG
    Check check;
    size_t nwrites;
#endif
};

// Data that is written once with no requirements for exclusive access when
// that write occurs.
template <typename T>
using WriteOnceData = ProtectedDataWriteOnce<CheckUnprotected, T>;

// Data that is written once, and only while holding the exclusive access lock.
template <typename T>
using ExclusiveAccessLockWriteOnceData =
    ProtectedDataWriteOnce<CheckGlobalLock<GlobalLock::ExclusiveAccessLock, AllowedBackgroundThread::None>, T>;

#undef DECLARE_ASSIGNMENT_OPERATOR
#undef DECLARE_ONE_BOOL_OPERATOR
#undef DECLARE_BOOL_OPERATORS

} // namespace js

#endif // threading_ProtectedData_h
