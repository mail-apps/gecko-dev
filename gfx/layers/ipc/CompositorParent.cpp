/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=2 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/CompositorParent.h"
#include <stdio.h>                      // for fprintf, stdout
#include <stdint.h>                     // for uint64_t
#include <map>                          // for _Rb_tree_iterator, etc
#include <utility>                      // for pair
#include "LayerTransactionParent.h"     // for LayerTransactionParent
#include "RenderTrace.h"                // for RenderTraceLayers
#include "base/message_loop.h"          // for MessageLoop
#include "base/process.h"               // for ProcessId
#include "base/task.h"                  // for CancelableTask, etc
#include "base/thread.h"                // for Thread
#include "base/tracked.h"               // for FROM_HERE
#include "gfxContext.h"                 // for gfxContext
#include "gfxPlatform.h"                // for gfxPlatform
#ifdef MOZ_WIDGET_GTK
#include "gfxPlatformGtk.h"             // for gfxPlatform
#endif
#include "gfxPrefs.h"                   // for gfxPrefs
#include "mozilla/AutoRestore.h"        // for AutoRestore
#include "mozilla/ClearOnShutdown.h"    // for ClearOnShutdown
#include "mozilla/DebugOnly.h"          // for DebugOnly
#include "mozilla/gfx/2D.h"          // for DrawTarget
#include "mozilla/gfx/Point.h"          // for IntSize
#include "mozilla/gfx/Rect.h"          // for IntSize
#include "VRManager.h"                  // for VRManager
#include "mozilla/ipc/Transport.h"      // for Transport
#include "mozilla/layers/APZCTreeManager.h"  // for APZCTreeManager
#include "mozilla/layers/APZThreadUtils.h"  // for APZCTreeManager
#include "mozilla/layers/AsyncCompositionManager.h"
#include "mozilla/layers/BasicCompositor.h"  // for BasicCompositor
#include "mozilla/layers/Compositor.h"  // for Compositor
#include "mozilla/layers/CompositorLRU.h"  // for CompositorLRU
#include "mozilla/layers/CompositorOGL.h"  // for CompositorOGL
#include "mozilla/layers/CompositorTypes.h"
#include "mozilla/layers/FrameUniformityData.h"
#include "mozilla/layers/ImageBridgeParent.h"
#include "mozilla/layers/LayerManagerComposite.h"
#include "mozilla/layers/LayersTypes.h"
#include "mozilla/layers/PLayerTransactionParent.h"
#include "mozilla/layers/ShadowLayersManager.h" // for ShadowLayersManager
#include "mozilla/media/MediaSystemResourceService.h" // for MediaSystemResourceService
#include "mozilla/mozalloc.h"           // for operator new, etc
#include "mozilla/Telemetry.h"
#ifdef MOZ_WIDGET_GTK
#include "basic/X11BasicCompositor.h" // for X11BasicCompositor
#endif
#include "nsCOMPtr.h"                   // for already_AddRefed
#include "nsDebug.h"                    // for NS_ASSERTION, etc
#include "nsISupportsImpl.h"            // for MOZ_COUNT_CTOR, etc
#include "nsIWidget.h"                  // for nsIWidget
#include "nsTArray.h"                   // for nsTArray
#include "nsThreadUtils.h"              // for NS_IsMainThread
#include "nsXULAppAPI.h"                // for XRE_GetIOMessageLoop
#include "nsIXULRuntime.h"              // for BrowserTabsRemoteAutostart
#ifdef XP_WIN
#include "mozilla/layers/CompositorD3D11.h"
#include "mozilla/layers/CompositorD3D9.h"
#endif
#include "GeckoProfiler.h"
#include "mozilla/ipc/ProtocolTypes.h"
#include "mozilla/unused.h"
#include "mozilla/Hal.h"
#include "mozilla/HalTypes.h"
#include "mozilla/StaticPtr.h"
#ifdef MOZ_ENABLE_PROFILER_SPS
#include "ProfilerMarkers.h"
#endif
#include "mozilla/VsyncDispatcher.h"

#ifdef MOZ_WIDGET_GONK
#include "GeckoTouchDispatcher.h"
#include "nsScreenManagerGonk.h"
#endif

#include "LayerScope.h"

namespace mozilla {

namespace gfx {
// See VRManagerChild.cpp
void ReleaseVRManagerParentSingleton();
} // namespace gfx

namespace layers {

using namespace mozilla::ipc;
using namespace mozilla::gfx;
using namespace std;

using base::ProcessId;
using base::Thread;

CompositorParent::LayerTreeState::LayerTreeState()
  : mParent(nullptr)
  , mLayerManager(nullptr)
  , mCrossProcessParent(nullptr)
  , mLayerTree(nullptr)
  , mUpdatedPluginDataAvailable(false)
{
}

CompositorParent::LayerTreeState::~LayerTreeState()
{
  if (mController) {
    mController->Destroy();
  }
}

typedef map<uint64_t, CompositorParent::LayerTreeState> LayerTreeMap;
static LayerTreeMap sIndirectLayerTrees;
static StaticAutoPtr<mozilla::Monitor> sIndirectLayerTreesLock;

static void EnsureLayerTreeMapReady()
{
  MOZ_ASSERT(NS_IsMainThread());
  if (!sIndirectLayerTreesLock) {
    sIndirectLayerTreesLock = new Monitor("IndirectLayerTree");
    mozilla::ClearOnShutdown(&sIndirectLayerTreesLock);
  }
}

/**
  * A global map referencing each compositor by ID.
  *
  * This map is used by the ImageBridge protocol to trigger
  * compositions without having to keep references to the
  * compositor
  */
typedef map<uint64_t,CompositorParent*> CompositorMap;
static CompositorMap* sCompositorMap;

static void CreateCompositorMap()
{
  MOZ_ASSERT(!sCompositorMap);
  sCompositorMap = new CompositorMap;
}

static void DestroyCompositorMap()
{
  MOZ_ASSERT(sCompositorMap);
  MOZ_ASSERT(sCompositorMap->empty());
  delete sCompositorMap;
  sCompositorMap = nullptr;
}

// See ImageBridgeChild.cpp
void ReleaseImageBridgeParentSingleton();

CompositorThreadHolder::CompositorThreadHolder()
  : mCompositorThread(CreateCompositorThread())
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_COUNT_CTOR(CompositorThreadHolder);
}

CompositorThreadHolder::~CompositorThreadHolder()
{
  MOZ_ASSERT(NS_IsMainThread());

  MOZ_COUNT_DTOR(CompositorThreadHolder);

  DestroyCompositorThread(mCompositorThread);
}

static StaticRefPtr<CompositorThreadHolder> sCompositorThreadHolder;
static bool sFinishedCompositorShutDown = false;

CompositorThreadHolder* GetCompositorThreadHolder()
{
  return sCompositorThreadHolder;
}

/* static */ Thread*
CompositorThreadHolder::CreateCompositorThread()
{
  MOZ_ASSERT(NS_IsMainThread());

  MOZ_ASSERT(!sCompositorThreadHolder, "The compositor thread has already been started!");

  Thread* compositorThread = new Thread("Compositor");

  Thread::Options options;
  /* Timeout values are powers-of-two to enable us get better data.
     128ms is chosen for transient hangs because 8Hz should be the minimally
     acceptable goal for Compositor responsiveness (normal goal is 60Hz). */
  options.transient_hang_timeout = 128; // milliseconds
  /* 2048ms is chosen for permanent hangs because it's longer than most
   * Compositor hangs seen in the wild, but is short enough to not miss getting
   * native hang stacks. */
  options.permanent_hang_timeout = 2048; // milliseconds
#if defined(_WIN32)
  /* With d3d9 the compositor thread creates native ui, see DeviceManagerD3D9. As
   * such the thread is a gui thread, and must process a windows message queue or
   * risk deadlocks. Chromium message loop TYPE_UI does exactly what we need. */
  options.message_loop_type = MessageLoop::TYPE_UI;
#endif

  if (!compositorThread->StartWithOptions(options)) {
    delete compositorThread;
    return nullptr;
  }

  EnsureLayerTreeMapReady();
  CreateCompositorMap();

  return compositorThread;
}

/* static */ void
CompositorThreadHolder::DestroyCompositorThread(Thread* aCompositorThread)
{
  MOZ_ASSERT(NS_IsMainThread());

  MOZ_ASSERT(!sCompositorThreadHolder, "We shouldn't be destroying the compositor thread yet.");

  DestroyCompositorMap();
  delete aCompositorThread;
  sFinishedCompositorShutDown = true;
}

static Thread* CompositorThread() {
  return sCompositorThreadHolder ? sCompositorThreadHolder->GetCompositorThread() : nullptr;
}

static void SetThreadPriority()
{
  hal::SetCurrentThreadPriority(hal::THREAD_PRIORITY_COMPOSITOR);
}

#ifdef COMPOSITOR_PERFORMANCE_WARNING
static int32_t
CalculateCompositionFrameRate()
{
  // Used when layout.frame_rate is -1. Needs to be kept in sync with
  // DEFAULT_FRAME_RATE in nsRefreshDriver.cpp.
  // TODO: This should actually return the vsync rate.
  const int32_t defaultFrameRate = 60;
  int32_t compositionFrameRatePref = gfxPrefs::LayersCompositionFrameRate();
  if (compositionFrameRatePref < 0) {
    // Use the same frame rate for composition as for layout.
    int32_t layoutFrameRatePref = gfxPrefs::LayoutFrameRate();
    if (layoutFrameRatePref < 0) {
      // TODO: The main thread frame scheduling code consults the actual
      // monitor refresh rate in this case. We should do the same.
      return defaultFrameRate;
    }
    return layoutFrameRatePref;
  }
  return compositionFrameRatePref;
}
#endif

CompositorVsyncScheduler::Observer::Observer(CompositorVsyncScheduler* aOwner)
  : mMutex("CompositorVsyncScheduler.Observer.Mutex")
  , mOwner(aOwner)
{
}

CompositorVsyncScheduler::Observer::~Observer()
{
  MOZ_ASSERT(!mOwner);
}

bool
CompositorVsyncScheduler::Observer::NotifyVsync(TimeStamp aVsyncTimestamp)
{
  MutexAutoLock lock(mMutex);
  if (!mOwner) {
    return false;
  }
  return mOwner->NotifyVsync(aVsyncTimestamp);
}

void
CompositorVsyncScheduler::Observer::Destroy()
{
  MutexAutoLock lock(mMutex);
  mOwner = nullptr;
}

CompositorVsyncScheduler::CompositorVsyncScheduler(CompositorParent* aCompositorParent, nsIWidget* aWidget)
  : mCompositorParent(aCompositorParent)
  , mLastCompose(TimeStamp::Now())
  , mIsObservingVsync(false)
  , mNeedsComposite(0)
  , mVsyncNotificationsSkipped(0)
  , mCurrentCompositeTaskMonitor("CurrentCompositeTaskMonitor")
  , mCurrentCompositeTask(nullptr)
  , mSetNeedsCompositeMonitor("SetNeedsCompositeMonitor")
  , mSetNeedsCompositeTask(nullptr)
#ifdef MOZ_WIDGET_GONK
#if ANDROID_VERSION >= 19
  , mDisplayEnabled(false)
  , mSetDisplayMonitor("SetDisplayMonitor")
  , mSetDisplayTask(nullptr)
#endif
#endif
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aWidget != nullptr);
  mVsyncObserver = new Observer(this);
  mCompositorVsyncDispatcher = aWidget->GetCompositorVsyncDispatcher();
#ifdef MOZ_WIDGET_GONK
  GeckoTouchDispatcher::GetInstance()->SetCompositorVsyncScheduler(this);

#if ANDROID_VERSION >= 19
  RefPtr<nsScreenManagerGonk> screenManager = nsScreenManagerGonk::GetInstance();
  screenManager->SetCompositorVsyncScheduler(this);
#endif
#endif

  // mAsapScheduling is set on the main thread during init,
  // but is only accessed after on the compositor thread.
  mAsapScheduling = gfxPrefs::LayersCompositionFrameRate() == 0 ||
                    gfxPlatform::IsInLayoutAsapMode();
}

CompositorVsyncScheduler::~CompositorVsyncScheduler()
{
  MOZ_ASSERT(!mIsObservingVsync);
  MOZ_ASSERT(!mVsyncObserver);
  // The CompositorVsyncDispatcher is cleaned up before this in the nsBaseWidget, which stops vsync listeners
  mCompositorParent = nullptr;
  mCompositorVsyncDispatcher = nullptr;
}

#ifdef MOZ_WIDGET_GONK
#if ANDROID_VERSION >= 19
void
CompositorVsyncScheduler::SetDisplay(bool aDisplayEnable)
{
  // SetDisplay() is usually called from nsScreenManager at main thread. Post
  // to compositor thread if needs.
  if (!CompositorParent::IsInCompositorThread()) {
    MOZ_ASSERT(NS_IsMainThread());
    MonitorAutoLock lock(mSetDisplayMonitor);
    mSetDisplayTask = NewRunnableMethod(this,
                                        &CompositorVsyncScheduler::SetDisplay,
                                        aDisplayEnable);
    ScheduleTask(mSetDisplayTask, 0);
    return;
  } else {
    MonitorAutoLock lock(mSetDisplayMonitor);
    mSetDisplayTask = nullptr;
  }

  if (mDisplayEnabled == aDisplayEnable) {
    return;
  }

  mDisplayEnabled = aDisplayEnable;
  if (!mDisplayEnabled) {
    CancelCurrentSetNeedsCompositeTask();
    CancelCurrentCompositeTask();
  }
}

void
CompositorVsyncScheduler::CancelSetDisplayTask()
{
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  MonitorAutoLock lock(mSetDisplayMonitor);
  if (mSetDisplayTask) {
    mSetDisplayTask->Cancel();
    mSetDisplayTask = nullptr;
  }

  // CancelSetDisplayTask is only be called in clean-up process, so
  // mDisplayEnabled could be false there.
  mDisplayEnabled = false;
}
#endif //ANDROID_VERSION >= 19
#endif //MOZ_WIDGET_GONK

void
CompositorVsyncScheduler::Destroy()
{
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  UnobserveVsync();
  mVsyncObserver->Destroy();
  mVsyncObserver = nullptr;

#ifdef MOZ_WIDGET_GONK
#if ANDROID_VERSION >= 19
  CancelSetDisplayTask();
#endif
#endif
  CancelCurrentSetNeedsCompositeTask();
  CancelCurrentCompositeTask();
}

void
CompositorVsyncScheduler::PostCompositeTask(TimeStamp aCompositeTimestamp)
{
  // can be called from the compositor or vsync thread
  MonitorAutoLock lock(mCurrentCompositeTaskMonitor);
  if (mCurrentCompositeTask == nullptr) {
    mCurrentCompositeTask = NewRunnableMethod(this,
                                              &CompositorVsyncScheduler::Composite,
                                              aCompositeTimestamp, nullptr, nullptr);
    ScheduleTask(mCurrentCompositeTask, 0);
  }
}

void
CompositorVsyncScheduler::ScheduleComposition()
{
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  if (mAsapScheduling) {
    // Used only for performance testing purposes
    PostCompositeTask(TimeStamp::Now());
#ifdef MOZ_WIDGET_ANDROID
  } else if (mNeedsComposite >= 2 && mIsObservingVsync) {
    // uh-oh, we already requested a composite at least twice so far, and a
    // composite hasn't happened yet. It is possible that the vsync observation
    // is blocked on the main thread, so let's just composite ASAP and not
    // wait for the vsync. Note that this should only ever happen on Fennec
    // because there content runs in the same process as the compositor, and so
    // content can actually block the main thread in this process.
    PostCompositeTask(TimeStamp::Now());
#endif
  } else {
    SetNeedsComposite();
  }
}

void
CompositorVsyncScheduler::CancelCurrentSetNeedsCompositeTask()
{
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  MonitorAutoLock lock(mSetNeedsCompositeMonitor);
  if (mSetNeedsCompositeTask) {
    mSetNeedsCompositeTask->Cancel();
    mSetNeedsCompositeTask = nullptr;
  }
  mNeedsComposite = 0;
}

/**
 * TODO Potential performance heuristics:
 * If a composite takes 17 ms, do we composite ASAP or wait until next vsync?
 * If a layer transaction comes after vsync, do we composite ASAP or wait until
 * next vsync?
 * How many skipped vsync events until we stop listening to vsync events?
 */
void
CompositorVsyncScheduler::SetNeedsComposite()
{
  if (!CompositorParent::IsInCompositorThread()) {
    MonitorAutoLock lock(mSetNeedsCompositeMonitor);
    mSetNeedsCompositeTask = NewRunnableMethod(this,
                                              &CompositorVsyncScheduler::SetNeedsComposite);
    ScheduleTask(mSetNeedsCompositeTask, 0);
    return;
  } else {
    MonitorAutoLock lock(mSetNeedsCompositeMonitor);
    mSetNeedsCompositeTask = nullptr;
  }

#ifdef MOZ_WIDGET_GONK
#if ANDROID_VERSION >= 19
  // Skip composition when display off.
  if (!mDisplayEnabled) {
    return;
  }
#endif
#endif

  mNeedsComposite++;
  if (!mIsObservingVsync && mNeedsComposite) {
    ObserveVsync();
  }
}

bool
CompositorVsyncScheduler::NotifyVsync(TimeStamp aVsyncTimestamp)
{
  // Called from the vsync dispatch thread
  MOZ_ASSERT(!CompositorParent::IsInCompositorThread());
  MOZ_ASSERT(!NS_IsMainThread());
  PostCompositeTask(aVsyncTimestamp);
  return true;
}

void
CompositorVsyncScheduler::CancelCurrentCompositeTask()
{
  MOZ_ASSERT(CompositorParent::IsInCompositorThread() || NS_IsMainThread());
  MonitorAutoLock lock(mCurrentCompositeTaskMonitor);
  if (mCurrentCompositeTask) {
    mCurrentCompositeTask->Cancel();
    mCurrentCompositeTask = nullptr;
  }
}

void
CompositorVsyncScheduler::Composite(TimeStamp aVsyncTimestamp, gfx::DrawTarget* aTarget, const IntRect* aRect)
{
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  {
    MonitorAutoLock lock(mCurrentCompositeTaskMonitor);
    mCurrentCompositeTask = nullptr;
  }

  if (aVsyncTimestamp < mLastCompose) {
    // We can sometimes get vsync timestamps that are in the past
    // compared to the last compose with force composites.
    // In those cases, normalize to now.
    aVsyncTimestamp = TimeStamp::Now();
  }

  DispatchTouchEvents(aVsyncTimestamp);
  DispatchVREvents(aVsyncTimestamp);

  if (mNeedsComposite || mAsapScheduling) {
    mNeedsComposite = 0;
    mLastCompose = aVsyncTimestamp;
    ComposeToTarget(aTarget, aRect);
    mVsyncNotificationsSkipped = 0;

    TimeDuration compositeFrameTotal = TimeStamp::Now() - aVsyncTimestamp;
    mozilla::Telemetry::Accumulate(mozilla::Telemetry::COMPOSITE_FRAME_ROUNDTRIP_TIME,
                                   compositeFrameTotal.ToMilliseconds());
  } else if (mVsyncNotificationsSkipped++ > gfxPrefs::CompositorUnobserveCount()) {
    UnobserveVsync();
  }
}

void
CompositorVsyncScheduler::OnForceComposeToTarget()
{
  /**
   * bug 1138502 - There are cases such as during long-running window resizing events
   * where we receive many sync RecvFlushComposites. We also get vsync notifications which
   * will increment mVsyncNotificationsSkipped because a composite just occurred. After
   * enough vsyncs and RecvFlushComposites occurred, we will disable vsync. Then at the next
   * ScheduleComposite, we will enable vsync, then get a RecvFlushComposite, which will
   * force us to unobserve vsync again. On some platforms, enabling/disabling vsync is not
   * free and this oscillating behavior causes a performance hit. In order to avoid this problem,
   * we reset the mVsyncNotificationsSkipped counter to keep vsync enabled.
   */
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  mVsyncNotificationsSkipped = 0;
}

void
CompositorVsyncScheduler::ForceComposeToTarget(gfx::DrawTarget* aTarget, const IntRect* aRect)
{
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  OnForceComposeToTarget();
  SetNeedsComposite();
  Composite(TimeStamp::Now(), aTarget, aRect);
}

bool
CompositorVsyncScheduler::NeedsComposite()
{
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  return mNeedsComposite;
}

void
CompositorVsyncScheduler::ObserveVsync()
{
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  mCompositorVsyncDispatcher->SetCompositorVsyncObserver(mVsyncObserver);
  mIsObservingVsync = true;
}

void
CompositorVsyncScheduler::UnobserveVsync()
{
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  mCompositorVsyncDispatcher->SetCompositorVsyncObserver(nullptr);
  mIsObservingVsync = false;
}

void
CompositorVsyncScheduler::DispatchTouchEvents(TimeStamp aVsyncTimestamp)
{
#ifdef MOZ_WIDGET_GONK
  GeckoTouchDispatcher::GetInstance()->NotifyVsync(aVsyncTimestamp);
#endif
}

void
CompositorVsyncScheduler::DispatchVREvents(TimeStamp aVsyncTimestamp)
{
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());

  VRManager* vm = VRManager::Get();
  vm->NotifyVsync(aVsyncTimestamp);
}

void CompositorParent::StartUp()
{
  MOZ_ASSERT(NS_IsMainThread(), "Should be on the main Thread!");
  MOZ_ASSERT(!sCompositorThreadHolder, "The compositor thread has already been started!");

  sCompositorThreadHolder = new CompositorThreadHolder();
}

void CompositorParent::ShutDown()
{
  MOZ_ASSERT(NS_IsMainThread(), "Should be on the main Thread!");
  MOZ_ASSERT(sCompositorThreadHolder, "The compositor thread has already been shut down!");

  ReleaseImageBridgeParentSingleton();
  ReleaseVRManagerParentSingleton();
  MediaSystemResourceService::Shutdown();

  sCompositorThreadHolder = nullptr;

  // No locking is needed around sFinishedCompositorShutDown because it is only
  // ever accessed on the main thread.
  while (!sFinishedCompositorShutDown) {
    NS_ProcessNextEvent(nullptr, true);
  }
}

MessageLoop* CompositorParent::CompositorLoop()
{
  return CompositorThread() ? CompositorThread()->message_loop() : nullptr;
}

void
CompositorVsyncScheduler::ScheduleTask(CancelableTask* aTask, int aTime)
{
  MOZ_ASSERT(CompositorParent::CompositorLoop());
  MOZ_ASSERT(aTime >= 0);
  CompositorParent::CompositorLoop()->PostDelayedTask(FROM_HERE, aTask, aTime);
}

void
CompositorVsyncScheduler::ResumeComposition()
{
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  SetNeedsComposite();
  Composite(TimeStamp::Now());
}

void
CompositorVsyncScheduler::ComposeToTarget(gfx::DrawTarget* aTarget, const IntRect* aRect)
{
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  MOZ_ASSERT(mCompositorParent);
  mCompositorParent->CompositeToTarget(aTarget, aRect);
}

CompositorParent::CompositorParent(nsIWidget* aWidget,
                                   bool aUseExternalSurfaceSize,
                                   int aSurfaceWidth, int aSurfaceHeight)
  : mWidget(aWidget)
  , mIsTesting(false)
  , mPendingTransaction(0)
  , mPaused(false)
  , mUseExternalSurfaceSize(aUseExternalSurfaceSize)
  , mEGLSurfaceSize(aSurfaceWidth, aSurfaceHeight)
  , mPauseCompositionMonitor("PauseCompositionMonitor")
  , mResumeCompositionMonitor("ResumeCompositionMonitor")
  , mRootLayerTreeID(AllocateLayerTreeId())
  , mOverrideComposeReadiness(false)
  , mForceCompositionTask(nullptr)
  , mCompositorThreadHolder(sCompositorThreadHolder)
  , mCompositorScheduler(nullptr)
#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
  , mLastPluginUpdateLayerTreeId(0)
  , mPluginUpdateResponsePending(false)
  , mDeferPluginWindows(false)
#endif
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(CompositorThread(),
             "The compositor thread must be Initialized before instanciating a CompositorParent.");
  MOZ_COUNT_CTOR(CompositorParent);
  mCompositorID = 0;
  // FIXME: This holds on the the fact that right now the only thing that
  // can destroy this instance is initialized on the compositor thread after
  // this task has been processed.
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(FROM_HERE, NewRunnableFunction(&AddCompositor,
                                                          this, &mCompositorID));

  CompositorLoop()->PostTask(FROM_HERE, NewRunnableFunction(SetThreadPriority));


  { // scope lock
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    sIndirectLayerTrees[mRootLayerTreeID].mParent = this;
  }

  // The Compositor uses the APZ pref directly since it needs to know whether
  // to attempt to create the APZ machinery at all.
  if (gfxPlatform::AsyncPanZoomEnabled() &&
      (aWidget->WindowType() == eWindowType_toplevel || aWidget->WindowType() == eWindowType_child)) {
    mApzcTreeManager = new APZCTreeManager();
  }

  mCompositorScheduler = new CompositorVsyncScheduler(this, aWidget);
  LayerScope::SetPixelScale(mWidget->GetDefaultScale().scale);
}

bool
CompositorParent::IsInCompositorThread()
{
  return CompositorThread() && CompositorThread()->thread_id() == PlatformThread::CurrentId();
}

uint64_t
CompositorParent::RootLayerTreeId()
{
  return mRootLayerTreeID;
}

CompositorParent::~CompositorParent()
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_COUNT_DTOR(CompositorParent);
}

void
CompositorParent::Destroy()
{
  MOZ_ASSERT(ManagedPLayerTransactionParent().Count() == 0,
             "CompositorParent destroyed before managed PLayerTransactionParent");

  MOZ_ASSERT(mPaused); // Ensure RecvWillStop was called
  // Ensure that the layer manager is destructed on the compositor thread.
  mLayerManager = nullptr;
  if (mCompositor) {
    mCompositor->Destroy();
  }
  mCompositor = nullptr;

  mCompositionManager = nullptr;
  if (mApzcTreeManager) {
    mApzcTreeManager->ClearTree();
    mApzcTreeManager = nullptr;
  }
  { // scope lock
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    sIndirectLayerTrees.erase(mRootLayerTreeID);
  }

  mCompositorScheduler->Destroy();
}

void
CompositorParent::ForceIsFirstPaint()
{
  mCompositionManager->ForceIsFirstPaint();
}

bool
CompositorParent::RecvWillStop()
{
  mPaused = true;
  RemoveCompositor(mCompositorID);

  // Ensure that the layer manager is destroyed before CompositorChild.
  if (mLayerManager) {
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    for (LayerTreeMap::iterator it = sIndirectLayerTrees.begin();
         it != sIndirectLayerTrees.end(); it++)
    {
      LayerTreeState* lts = &it->second;
      if (lts->mParent == this) {
        mLayerManager->ClearCachedResources(lts->mRoot);
        lts->mLayerManager = nullptr;
        lts->mParent = nullptr;
      }
    }
    mLayerManager->Destroy();
    mLayerManager = nullptr;
    mCompositionManager = nullptr;
  }

  return true;
}

void CompositorParent::DeferredDestroy()
{
  MOZ_ASSERT(!NS_IsMainThread());
  MOZ_ASSERT(mCompositorThreadHolder);
  mCompositorThreadHolder = nullptr;
  Release();
}

bool
CompositorParent::RecvStop()
{
  Destroy();
  // There are chances that the ref count reaches zero on the main thread shortly
  // after this function returns while some ipdl code still needs to run on
  // this thread.
  // We must keep the compositor parent alive untill the code handling message
  // reception is finished on this thread.
  this->AddRef(); // Corresponds to DeferredDestroy's Release
  MessageLoop::current()->PostTask(FROM_HERE,
                                   NewRunnableMethod(this,&CompositorParent::DeferredDestroy));
  return true;
}

bool
CompositorParent::RecvPause()
{
  PauseComposition();
  return true;
}

bool
CompositorParent::RecvResume()
{
  ResumeComposition();
  return true;
}

bool
CompositorParent::RecvMakeSnapshot(const SurfaceDescriptor& aInSnapshot,
                                   const gfx::IntRect& aRect)
{
  RefPtr<DrawTarget> target = GetDrawTargetForDescriptor(aInSnapshot, gfx::BackendType::CAIRO);
  MOZ_ASSERT(target);
  if (!target) {
    // We kill the content process rather than have it continue with an invalid
    // snapshot, that may be too harsh and we could decide to return some sort
    // of error to the child process and let it deal with it...
    return false;
  }
  ForceComposeToTarget(target, &aRect);
  return true;
}

bool
CompositorParent::RecvMakeWidgetSnapshot(const SurfaceDescriptor& aInSnapshot)
{
  if (!mCompositor || !mCompositor->GetWidget()) {
    return false;
  }

  RefPtr<DrawTarget> target = GetDrawTargetForDescriptor(aInSnapshot, gfx::BackendType::CAIRO);
  MOZ_ASSERT(target);
  if (!target) {
    return false;
  }
  mCompositor->GetWidget()->CaptureWidgetOnScreen(target);
  return true;
}

bool
CompositorParent::RecvFlushRendering()
{
  if (mCompositorScheduler->NeedsComposite())
  {
    CancelCurrentCompositeTask();
    ForceComposeToTarget(nullptr);
  }
  return true;
}

bool
CompositorParent::RecvGetTileSize(int32_t* aWidth, int32_t* aHeight)
{
  *aWidth = gfxPlatform::GetPlatform()->GetTileWidth();
  *aHeight = gfxPlatform::GetPlatform()->GetTileHeight();
  return true;
}

bool
CompositorParent::RecvNotifyRegionInvalidated(const nsIntRegion& aRegion)
{
  if (mLayerManager) {
    mLayerManager->AddInvalidRegion(aRegion);
  }
  return true;
}

void
CompositorParent::Invalidate()
{
  if (mLayerManager && mLayerManager->GetRoot()) {
    mLayerManager->AddInvalidRegion(
                                    mLayerManager->GetRoot()->GetVisibleRegion().ToUnknownRegion().GetBounds());
  }
}

bool
CompositorParent::RecvStartFrameTimeRecording(const int32_t& aBufferSize, uint32_t* aOutStartIndex)
{
  if (mLayerManager) {
    *aOutStartIndex = mLayerManager->StartFrameTimeRecording(aBufferSize);
  } else {
    *aOutStartIndex = 0;
  }
  return true;
}

bool
CompositorParent::RecvStopFrameTimeRecording(const uint32_t& aStartIndex,
                                             InfallibleTArray<float>* intervals)
{
  if (mLayerManager) {
    mLayerManager->StopFrameTimeRecording(aStartIndex, *intervals);
  }
  return true;
}

void
CompositorParent::ActorDestroy(ActorDestroyReason why)
{
  CancelCurrentCompositeTask();
  if (mForceCompositionTask) {
    mForceCompositionTask->Cancel();
    mForceCompositionTask = nullptr;
  }
  mPaused = true;
  RemoveCompositor(mCompositorID);

  if (mLayerManager) {
    mLayerManager->Destroy();
    mLayerManager = nullptr;
    { // scope lock
      MonitorAutoLock lock(*sIndirectLayerTreesLock);
      sIndirectLayerTrees[mRootLayerTreeID].mLayerManager = nullptr;
    }
    mCompositionManager = nullptr;
    mCompositor = nullptr;
  }
}


void
CompositorParent::ScheduleRenderOnCompositorThread()
{
  CancelableTask *renderTask = NewRunnableMethod(this, &CompositorParent::ScheduleComposition);
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(FROM_HERE, renderTask);
}

void
CompositorParent::InvalidateOnCompositorThread()
{
  CancelableTask *renderTask = NewRunnableMethod(this, &CompositorParent::Invalidate);
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(FROM_HERE, renderTask);
}

void
CompositorParent::PauseComposition()
{
  MOZ_ASSERT(IsInCompositorThread(),
             "PauseComposition() can only be called on the compositor thread");

  MonitorAutoLock lock(mPauseCompositionMonitor);

  if (!mPaused) {
    mPaused = true;

    mCompositor->Pause();

    TimeStamp now = TimeStamp::Now();
    DidComposite(now, now);
  }

  // if anyone's waiting to make sure that composition really got paused, tell them
  lock.NotifyAll();
}

void
CompositorParent::ResumeComposition()
{
  MOZ_ASSERT(IsInCompositorThread(),
             "ResumeComposition() can only be called on the compositor thread");

  MonitorAutoLock lock(mResumeCompositionMonitor);

  if (!mCompositor->Resume()) {
#ifdef MOZ_WIDGET_ANDROID
    // We can't get a surface. This could be because the activity changed between
    // the time resume was scheduled and now.
    __android_log_print(ANDROID_LOG_INFO, "CompositorParent", "Unable to renew compositor surface; remaining in paused state");
#endif
    lock.NotifyAll();
    return;
  }

  mPaused = false;

  mCompositorScheduler->ResumeComposition();

  // if anyone's waiting to make sure that composition really got resumed, tell them
  lock.NotifyAll();
}

void
CompositorParent::ForceComposition()
{
  // Cancel the orientation changed state to force composition
  mForceCompositionTask = nullptr;
  ScheduleRenderOnCompositorThread();
}

void
CompositorParent::CancelCurrentCompositeTask()
{
  mCompositorScheduler->CancelCurrentCompositeTask();
}

void
CompositorParent::SetEGLSurfaceSize(int width, int height)
{
  NS_ASSERTION(mUseExternalSurfaceSize, "Compositor created without UseExternalSurfaceSize provided");
  mEGLSurfaceSize.SizeTo(width, height);
  if (mCompositor) {
    mCompositor->SetDestinationSurfaceSize(gfx::IntSize(mEGLSurfaceSize.width, mEGLSurfaceSize.height));
  }
}

void
CompositorParent::ResumeCompositionAndResize(int width, int height)
{
  SetEGLSurfaceSize(width, height);
  ResumeComposition();
}

/*
 * This will execute a pause synchronously, waiting to make sure that the compositor
 * really is paused.
 */
void
CompositorParent::SchedulePauseOnCompositorThread()
{
  MonitorAutoLock lock(mPauseCompositionMonitor);

  CancelableTask *pauseTask = NewRunnableMethod(this,
                                                &CompositorParent::PauseComposition);
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(FROM_HERE, pauseTask);

  // Wait until the pause has actually been processed by the compositor thread
  lock.Wait();
}

bool
CompositorParent::ScheduleResumeOnCompositorThread()
{
  MonitorAutoLock lock(mResumeCompositionMonitor);

  CancelableTask *resumeTask =
    NewRunnableMethod(this, &CompositorParent::ResumeComposition);
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(FROM_HERE, resumeTask);

  // Wait until the resume has actually been processed by the compositor thread
  lock.Wait();

  return !mPaused;
}

bool
CompositorParent::ScheduleResumeOnCompositorThread(int width, int height)
{
  MonitorAutoLock lock(mResumeCompositionMonitor);

  CancelableTask *resumeTask =
    NewRunnableMethod(this, &CompositorParent::ResumeCompositionAndResize, width, height);
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(FROM_HERE, resumeTask);

  // Wait until the resume has actually been processed by the compositor thread
  lock.Wait();

  return !mPaused;
}

void
CompositorParent::ScheduleTask(CancelableTask* task, int time)
{
  if (time == 0) {
    MessageLoop::current()->PostTask(FROM_HERE, task);
  } else {
    MessageLoop::current()->PostDelayedTask(FROM_HERE, task, time);
  }
}

void
CompositorParent::NotifyShadowTreeTransaction(uint64_t aId, bool aIsFirstPaint,
    bool aScheduleComposite, uint32_t aPaintSequenceNumber,
    bool aIsRepeatTransaction)
{
  if (mApzcTreeManager &&
      !aIsRepeatTransaction &&
      mLayerManager &&
      mLayerManager->GetRoot()) {
    AutoResolveRefLayers resolve(mCompositionManager);
    mApzcTreeManager->UpdateHitTestingTree(this, mLayerManager->GetRoot(),
        aIsFirstPaint, aId, aPaintSequenceNumber);

    mLayerManager->NotifyShadowTreeTransaction();
  }
  if (aScheduleComposite) {
    ScheduleComposition();
  }
}

void
CompositorParent::ScheduleComposition()
{
  MOZ_ASSERT(IsInCompositorThread());
  if (mPaused) {
    return;
  }

  mCompositorScheduler->ScheduleComposition();
}

// Go down the composite layer tree, setting properties to match their
// content-side counterparts.
/* static */ void
CompositorParent::SetShadowProperties(Layer* aLayer)
{
  if (Layer* maskLayer = aLayer->GetMaskLayer()) {
    SetShadowProperties(maskLayer);
  }
  for (size_t i = 0; i < aLayer->GetAncestorMaskLayerCount(); i++) {
    SetShadowProperties(aLayer->GetAncestorMaskLayerAt(i));
  }

  // FIXME: Bug 717688 -- Do these updates in LayerTransactionParent::RecvUpdate.
  LayerComposite* layerComposite = aLayer->AsLayerComposite();
  // Set the layerComposite's base transform to the layer's base transform.
  layerComposite->SetShadowTransform(aLayer->GetBaseTransform());
  layerComposite->SetShadowTransformSetByAnimation(false);
  layerComposite->SetShadowVisibleRegion(aLayer->GetVisibleRegion());
  layerComposite->SetShadowClipRect(aLayer->GetClipRect());
  layerComposite->SetShadowOpacity(aLayer->GetOpacity());

  for (Layer* child = aLayer->GetFirstChild();
      child; child = child->GetNextSibling()) {
    SetShadowProperties(child);
  }
}

void
CompositorParent::CompositeToTarget(DrawTarget* aTarget, const gfx::IntRect* aRect)
{
  profiler_tracing("Paint", "Composite", TRACING_INTERVAL_START);
  PROFILER_LABEL("CompositorParent", "Composite",
    js::ProfileEntry::Category::GRAPHICS);

  MOZ_ASSERT(IsInCompositorThread(),
             "Composite can only be called on the compositor thread");
  TimeStamp start = TimeStamp::Now();

#ifdef COMPOSITOR_PERFORMANCE_WARNING
  TimeDuration scheduleDelta = TimeStamp::Now() - mCompositorScheduler->GetExpectedComposeStartTime();
  if (scheduleDelta > TimeDuration::FromMilliseconds(2) ||
      scheduleDelta < TimeDuration::FromMilliseconds(-2)) {
    printf_stderr("Compositor: Compose starting off schedule by %4.1f ms\n",
                  scheduleDelta.ToMilliseconds());
  }
#endif

  if (!CanComposite()) {
    TimeStamp end = TimeStamp::Now();
    DidComposite(start, end);
    return;
  }

#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
  // Still waiting on plugin update confirmation
  if (mPluginUpdateResponsePending) {
    return;
  }
#endif

  bool hasRemoteContent = false;
  bool pluginsUpdatedFlag = true;
  AutoResolveRefLayers resolve(mCompositionManager, this,
                               &hasRemoteContent,
                               &pluginsUpdatedFlag);

#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
  /*
   * AutoResolveRefLayers handles two tasks related to Windows and Linux
   * plugin window management:
   * 1) calculating if we have remote content in the view. If we do not have
   * remote content, all plugin windows for this CompositorParent (window)
   * can be hidden since we do not support plugins in chrome when running
   * under e10s.
   * 2) Updating plugin position, size, and clip. We do this here while the
   * remote layer tree is hooked up to to chrome layer tree. This is needed
   * since plugin clipping can depend on chrome (for example, due to tab modal
   * prompts). Updates in step 2 are applied via an async ipc message sent
   * to the main thread.
   * Windows specific: The compositor will wait for confirmation that plugin
   * updates have been applied before painting. Deferment of painting is
   * indicated by the mPluginUpdateResponsePending flag. The main thread
   * messages back using the RemotePluginsReady async ipc message.
   * This is neccessary since plugin windows can leave remnants of window
   * content if moved after the underlying window paints.
   */
  if (pluginsUpdatedFlag) {
    mPluginUpdateResponsePending = true;
    return;
  }

  // We do not support plugins in local content. When switching tabs
  // to local pages, hide every plugin associated with the window.
  if (!hasRemoteContent && BrowserTabsRemoteAutostart() &&
      mCachedPluginData.Length()) {
    Unused << SendHideAllPlugins((uintptr_t)GetWidget());
    mCachedPluginData.Clear();
    // Wait for confirmation the hide operation is complete.
    mPluginUpdateResponsePending = true;
    return;
  }
#endif

  if (aTarget) {
    mLayerManager->BeginTransactionWithDrawTarget(aTarget, *aRect);
  } else {
    mLayerManager->BeginTransaction();
  }

  SetShadowProperties(mLayerManager->GetRoot());

  if (mForceCompositionTask && !mOverrideComposeReadiness) {
    if (mCompositionManager->ReadyForCompose()) {
      mForceCompositionTask->Cancel();
      mForceCompositionTask = nullptr;
    } else {
      return;
    }
  }

  mCompositionManager->ComputeRotation();

  TimeStamp time = mIsTesting ? mTestTime : mCompositorScheduler->GetLastComposeTime();
  bool requestNextFrame = mCompositionManager->TransformShadowTree(time);
  if (requestNextFrame) {
    ScheduleComposition();
  }

  RenderTraceLayers(mLayerManager->GetRoot(), "0000");

#ifdef MOZ_DUMP_PAINTING
  if (gfxPrefs::DumpHostLayers()) {
    printf_stderr("Painting --- compositing layer tree:\n");
    mLayerManager->Dump();
  }
#endif
  mLayerManager->SetDebugOverlayWantsNextFrame(false);
  mLayerManager->EndTransaction(time);

  if (!aTarget) {
    TimeStamp end = TimeStamp::Now();
    DidComposite(start, end);
  }

  // We're not really taking advantage of the stored composite-again-time here.
  // We might be able to skip the next few composites altogether. However,
  // that's a bit complex to implement and we'll get most of the advantage
  // by skipping compositing when we detect there's nothing invalid. This is why
  // we do "composite until" rather than "composite again at".
  if (!mCompositor->GetCompositeUntilTime().IsNull() ||
      mLayerManager->DebugOverlayWantsNextFrame()) {
    ScheduleComposition();
  }

#ifdef COMPOSITOR_PERFORMANCE_WARNING
  TimeDuration executionTime = TimeStamp::Now() - mCompositorScheduler->GetLastComposeTime();
  TimeDuration frameBudget = TimeDuration::FromMilliseconds(15);
  int32_t frameRate = CalculateCompositionFrameRate();
  if (frameRate > 0) {
    frameBudget = TimeDuration::FromSeconds(1.0 / frameRate);
  }
  if (executionTime > frameBudget) {
    printf_stderr("Compositor: Composite execution took %4.1f ms\n",
                  executionTime.ToMilliseconds());
  }
#endif

  // 0 -> Full-tilt composite
  if (gfxPrefs::LayersCompositionFrameRate() == 0
    || mLayerManager->GetCompositor()->GetDiagnosticTypes() & DiagnosticTypes::FLASH_BORDERS) {
    // Special full-tilt composite mode for performance testing
    ScheduleComposition();
  }
  mCompositor->SetCompositionTime(TimeStamp());

  mozilla::Telemetry::AccumulateTimeDelta(mozilla::Telemetry::COMPOSITE_TIME, start);
  profiler_tracing("Paint", "Composite", TRACING_INTERVAL_END);
}

bool
CompositorParent::RecvRemotePluginsReady()
{
#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
  mPluginUpdateResponsePending = false;
  ScheduleComposition();
  return true;
#else
  NS_NOTREACHED("CompositorParent::RecvRemotePluginsReady calls "
                "unexpected on this platform.");
  return false;
#endif
}

void
CompositorParent::ForceComposeToTarget(DrawTarget* aTarget, const gfx::IntRect* aRect)
{
  PROFILER_LABEL("CompositorParent", "ForceComposeToTarget",
    js::ProfileEntry::Category::GRAPHICS);

  AutoRestore<bool> override(mOverrideComposeReadiness);
  mOverrideComposeReadiness = true;
  mCompositorScheduler->ForceComposeToTarget(aTarget, aRect);
}

bool
CompositorParent::CanComposite()
{
  return mLayerManager &&
         mLayerManager->GetRoot() &&
         !mPaused;
}

void
CompositorParent::ScheduleRotationOnCompositorThread(const TargetConfig& aTargetConfig,
                                                     bool aIsFirstPaint)
{
  MOZ_ASSERT(IsInCompositorThread());

  if (!aIsFirstPaint &&
      !mCompositionManager->IsFirstPaint() &&
      mCompositionManager->RequiresReorientation(aTargetConfig.orientation())) {
    if (mForceCompositionTask != nullptr) {
      mForceCompositionTask->Cancel();
    }
    mForceCompositionTask = NewRunnableMethod(this, &CompositorParent::ForceComposition);
    ScheduleTask(mForceCompositionTask, gfxPrefs::OrientationSyncMillis());
  }
}

void
CompositorParent::ShadowLayersUpdated(LayerTransactionParent* aLayerTree,
                                      const uint64_t& aTransactionId,
                                      const TargetConfig& aTargetConfig,
                                      const InfallibleTArray<PluginWindowData>& aUnused,
                                      bool aIsFirstPaint,
                                      bool aScheduleComposite,
                                      uint32_t aPaintSequenceNumber,
                                      bool aIsRepeatTransaction,
                                      int32_t aPaintSyncId)
{
  ScheduleRotationOnCompositorThread(aTargetConfig, aIsFirstPaint);

  // Instruct the LayerManager to update its render bounds now. Since all the orientation
  // change, dimension change would be done at the stage, update the size here is free of
  // race condition.
  mLayerManager->UpdateRenderBounds(aTargetConfig.naturalBounds());
  mLayerManager->SetRegionToClear(aTargetConfig.clearRegion());
  mLayerManager->GetCompositor()->SetScreenRotation(aTargetConfig.rotation());

  mCompositionManager->Updated(aIsFirstPaint, aTargetConfig, aPaintSyncId);
  Layer* root = aLayerTree->GetRoot();
  mLayerManager->SetRoot(root);

  if (mApzcTreeManager && !aIsRepeatTransaction) {
    AutoResolveRefLayers resolve(mCompositionManager);
    mApzcTreeManager->UpdateHitTestingTree(this, root, aIsFirstPaint,
        mRootLayerTreeID, aPaintSequenceNumber);
  }

  // The transaction ID might get reset to 1 if the page gets reloaded, see
  // https://bugzilla.mozilla.org/show_bug.cgi?id=1145295#c41
  // Otherwise, it should be continually increasing.
  MOZ_ASSERT(aTransactionId == 1 || aTransactionId > mPendingTransaction);
  mPendingTransaction = aTransactionId;

  if (root) {
    SetShadowProperties(root);
  }
  if (aScheduleComposite) {
    ScheduleComposition();
    if (mPaused) {
      TimeStamp now = TimeStamp::Now();
      DidComposite(now, now);
    }
  }
  mLayerManager->NotifyShadowTreeTransaction();
}

void
CompositorParent::ForceComposite(LayerTransactionParent* aLayerTree)
{
  ScheduleComposition();
}

bool
CompositorParent::SetTestSampleTime(LayerTransactionParent* aLayerTree,
                                    const TimeStamp& aTime)
{
  if (aTime.IsNull()) {
    return false;
  }

  mIsTesting = true;
  mTestTime = aTime;

  bool testComposite = mCompositionManager &&
                       mCompositorScheduler->NeedsComposite();

  // Update but only if we were already scheduled to animate
  if (testComposite) {
    AutoResolveRefLayers resolve(mCompositionManager);
    bool requestNextFrame = mCompositionManager->TransformShadowTree(aTime);
    if (!requestNextFrame) {
      CancelCurrentCompositeTask();
      // Pretend we composited in case someone is wating for this event.
      TimeStamp now = TimeStamp::Now();
      DidComposite(now, now);
    }
  }

  return true;
}

void
CompositorParent::LeaveTestMode(LayerTransactionParent* aLayerTree)
{
  mIsTesting = false;
}

void
CompositorParent::ApplyAsyncProperties(LayerTransactionParent* aLayerTree)
{
  // NOTE: This should only be used for testing. For example, when mIsTesting is
  // true or when called from test-only methods like
  // LayerTransactionParent::RecvGetAnimationTransform.

  // Synchronously update the layer tree
  if (aLayerTree->GetRoot()) {
    AutoResolveRefLayers resolve(mCompositionManager);
    SetShadowProperties(mLayerManager->GetRoot());

    TimeStamp time = mIsTesting ? mTestTime : mCompositorScheduler->GetLastComposeTime();
    bool requestNextFrame =
      mCompositionManager->TransformShadowTree(time,
        AsyncCompositionManager::TransformsToSkip::APZ);
    if (!requestNextFrame) {
      CancelCurrentCompositeTask();
      // Pretend we composited in case someone is waiting for this event.
      TimeStamp now = TimeStamp::Now();
      DidComposite(now, now);
    }
  }
}

bool
CompositorParent::RecvGetFrameUniformity(FrameUniformityData* aOutData)
{
  mCompositionManager->GetFrameUniformity(aOutData);
  return true;
}

bool
CompositorParent::RecvRequestOverfill()
{
  uint32_t overfillRatio = mCompositor->GetFillRatio();
  Unused << SendOverfill(overfillRatio);
  return true;
}

void
CompositorParent::FlushApzRepaints(const LayerTransactionParent* aLayerTree)
{
  MOZ_ASSERT(mApzcTreeManager);
  uint64_t layersId = aLayerTree->GetId();
  if (layersId == 0) {
    // The request is coming from the parent-process layer tree, so we should
    // use the compositor's root layer tree id.
    layersId = mRootLayerTreeID;
  }
  mApzcTreeManager->FlushApzRepaints(layersId);
}

void
CompositorParent::GetAPZTestData(const LayerTransactionParent* aLayerTree,
                                 APZTestData* aOutData)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  *aOutData = sIndirectLayerTrees[mRootLayerTreeID].mApzTestData;
}

class NotifyAPZConfirmedTargetTask : public Task
{
public:
  explicit NotifyAPZConfirmedTargetTask(const RefPtr<APZCTreeManager>& aAPZCTM,
                                        const uint64_t& aInputBlockId,
                                        const nsTArray<ScrollableLayerGuid>& aTargets)
   : mAPZCTM(aAPZCTM),
     mInputBlockId(aInputBlockId),
     mTargets(aTargets)
  {
  }

  virtual void Run() override {
    mAPZCTM->SetTargetAPZC(mInputBlockId, mTargets);
  }

private:
  RefPtr<APZCTreeManager> mAPZCTM;
  uint64_t mInputBlockId;
  nsTArray<ScrollableLayerGuid> mTargets;
};

void
CompositorParent::SetConfirmedTargetAPZC(const LayerTransactionParent* aLayerTree,
                                         const uint64_t& aInputBlockId,
                                         const nsTArray<ScrollableLayerGuid>& aTargets)
{
  if (!mApzcTreeManager) {
    return;
  }
  APZThreadUtils::RunOnControllerThread(new NotifyAPZConfirmedTargetTask(
    mApzcTreeManager, aInputBlockId, aTargets));
}

void
CompositorParent::InitializeLayerManager(const nsTArray<LayersBackend>& aBackendHints)
{
  NS_ASSERTION(!mLayerManager, "Already initialised mLayerManager");
  NS_ASSERTION(!mCompositor,   "Already initialised mCompositor");

  for (size_t i = 0; i < aBackendHints.Length(); ++i) {
    RefPtr<Compositor> compositor;
    if (aBackendHints[i] == LayersBackend::LAYERS_OPENGL) {
      compositor = new CompositorOGL(mWidget,
                                     mEGLSurfaceSize.width,
                                     mEGLSurfaceSize.height,
                                     mUseExternalSurfaceSize);
    } else if (aBackendHints[i] == LayersBackend::LAYERS_BASIC) {
#ifdef MOZ_WIDGET_GTK
      if (gfxPlatformGtk::GetPlatform()->UseXRender()) {
        compositor = new X11BasicCompositor(mWidget);
      } else
#endif
      {
        compositor = new BasicCompositor(mWidget);
      }
#ifdef XP_WIN
    } else if (aBackendHints[i] == LayersBackend::LAYERS_D3D11) {
      compositor = new CompositorD3D11(mWidget);
    } else if (aBackendHints[i] == LayersBackend::LAYERS_D3D9) {
      compositor = new CompositorD3D9(this, mWidget);
#endif
    }

    if (!compositor) {
      // We passed a backend hint for which we can't create a compositor.
      // For example, we sometime pass LayersBackend::LAYERS_NONE as filler in aBackendHints.
      continue;
    }

    compositor->SetCompositorID(mCompositorID);
    RefPtr<LayerManagerComposite> layerManager = new LayerManagerComposite(compositor);

    if (layerManager->Initialize()) {
      mLayerManager = layerManager;
      MOZ_ASSERT(compositor);
      mCompositor = compositor;
      MonitorAutoLock lock(*sIndirectLayerTreesLock);
      sIndirectLayerTrees[mRootLayerTreeID].mLayerManager = layerManager;
      return;
    }
  }
}

PLayerTransactionParent*
CompositorParent::AllocPLayerTransactionParent(const nsTArray<LayersBackend>& aBackendHints,
                                               const uint64_t& aId,
                                               TextureFactoryIdentifier* aTextureFactoryIdentifier,
                                               bool *aSuccess)
{
  MOZ_ASSERT(aId == 0);

  InitializeLayerManager(aBackendHints);

  if (!mLayerManager) {
    NS_WARNING("Failed to initialise Compositor");
    *aSuccess = false;
    LayerTransactionParent* p = new LayerTransactionParent(nullptr, this, 0);
    p->AddIPDLReference();
    return p;
  }

  mCompositionManager = new AsyncCompositionManager(mLayerManager);
  *aSuccess = true;

  *aTextureFactoryIdentifier = mCompositor->GetTextureFactoryIdentifier();
  LayerTransactionParent* p = new LayerTransactionParent(mLayerManager, this, 0);
  p->AddIPDLReference();
  return p;
}

bool
CompositorParent::DeallocPLayerTransactionParent(PLayerTransactionParent* actor)
{
  static_cast<LayerTransactionParent*>(actor)->ReleaseIPDLReference();
  return true;
}

CompositorParent* CompositorParent::GetCompositor(uint64_t id)
{
  CompositorMap::iterator it = sCompositorMap->find(id);
  return it != sCompositorMap->end() ? it->second : nullptr;
}

void CompositorParent::AddCompositor(CompositorParent* compositor, uint64_t* outID)
{
  static uint64_t sNextID = 1;

  ++sNextID;
  (*sCompositorMap)[sNextID] = compositor;
  *outID = sNextID;
}

CompositorParent* CompositorParent::RemoveCompositor(uint64_t id)
{
  CompositorMap::iterator it = sCompositorMap->find(id);
  if (it == sCompositorMap->end()) {
    return nullptr;
  }
  CompositorParent *retval = it->second;
  sCompositorMap->erase(it);
  return retval;
}

bool
CompositorParent::RecvNotifyChildCreated(const uint64_t& child)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  NotifyChildCreated(child);
  return true;
}

void
CompositorParent::NotifyChildCreated(const uint64_t& aChild)
{
  sIndirectLayerTreesLock->AssertCurrentThreadOwns();
  sIndirectLayerTrees[aChild].mParent = this;
  sIndirectLayerTrees[aChild].mLayerManager = mLayerManager;
}

bool
CompositorParent::RecvAdoptChild(const uint64_t& child)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  NotifyChildCreated(child);
  if (sIndirectLayerTrees[child].mLayerTree) {
    sIndirectLayerTrees[child].mLayerTree->mLayerManager = mLayerManager;
  }
  if (sIndirectLayerTrees[child].mRoot) {
    sIndirectLayerTrees[child].mRoot->AsLayerComposite()->SetLayerManager(mLayerManager);
  }
  return true;
}

/*static*/ uint64_t
CompositorParent::AllocateLayerTreeId()
{
  MOZ_ASSERT(CompositorLoop());
  MOZ_ASSERT(NS_IsMainThread());
  static uint64_t ids = 0;
  return ++ids;
}

static void
EraseLayerState(uint64_t aId)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees.erase(aId);
}

/*static*/ void
CompositorParent::DeallocateLayerTreeId(uint64_t aId)
{
  MOZ_ASSERT(NS_IsMainThread());
  // Here main thread notifies compositor to remove an element from
  // sIndirectLayerTrees. This removed element might be queried soon.
  // Checking the elements of sIndirectLayerTrees exist or not before using.
  if (!CompositorLoop()) {
    gfxCriticalError() << "Attempting to post to a invalid Compositor Loop";
    return;
  }
  CompositorLoop()->PostTask(FROM_HERE,
                             NewRunnableFunction(&EraseLayerState, aId));
}

/* static */ void
CompositorParent::SwapLayerTreeObservers(uint64_t aLayerId, uint64_t aOtherLayerId)
{
  EnsureLayerTreeMapReady();
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  NS_ASSERTION(sIndirectLayerTrees.find(aLayerId) != sIndirectLayerTrees.end(),
    "SwapLayerTrees missing layer 1");
  NS_ASSERTION(sIndirectLayerTrees.find(aOtherLayerId) != sIndirectLayerTrees.end(),
    "SwapLayerTrees missing layer 2");
  std::swap(sIndirectLayerTrees[aLayerId].mLayerTreeReadyObserver,
    sIndirectLayerTrees[aOtherLayerId].mLayerTreeReadyObserver);
}

static void
UpdateControllerForLayersId(uint64_t aLayersId,
                            GeckoContentController* aController)
{
  // Adopt ref given to us by SetControllerForLayerTree()
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees[aLayersId].mController =
    already_AddRefed<GeckoContentController>(aController);
}

ScopedLayerTreeRegistration::ScopedLayerTreeRegistration(APZCTreeManager* aApzctm,
                                                         uint64_t aLayersId,
                                                         Layer* aRoot,
                                                         GeckoContentController* aController)
    : mLayersId(aLayersId)
{
  EnsureLayerTreeMapReady();
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees[aLayersId].mRoot = aRoot;
  sIndirectLayerTrees[aLayersId].mController = aController;
}

ScopedLayerTreeRegistration::~ScopedLayerTreeRegistration()
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees.erase(mLayersId);
}

/*static*/ void
CompositorParent::SetControllerForLayerTree(uint64_t aLayersId,
                                            GeckoContentController* aController)
{
  // This ref is adopted by UpdateControllerForLayersId().
  aController->AddRef();
  CompositorLoop()->PostTask(FROM_HERE,
                             NewRunnableFunction(&UpdateControllerForLayersId,
                                                 aLayersId,
                                                 aController));
}

/*static*/ APZCTreeManager*
CompositorParent::GetAPZCTreeManager(uint64_t aLayersId)
{
  EnsureLayerTreeMapReady();
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  LayerTreeMap::iterator cit = sIndirectLayerTrees.find(aLayersId);
  if (sIndirectLayerTrees.end() == cit) {
    return nullptr;
  }
  LayerTreeState* lts = &cit->second;
  return (lts->mParent ? lts->mParent->mApzcTreeManager.get() : nullptr);
}

float
CompositorParent::ComputeRenderIntegrity()
{
  if (mLayerManager) {
    return mLayerManager->ComputeRenderIntegrity();
  }

  return 1.0f;
}

static void
InsertVsyncProfilerMarker(TimeStamp aVsyncTimestamp)
{
#ifdef MOZ_ENABLE_PROFILER_SPS
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  VsyncPayload* payload = new VsyncPayload(aVsyncTimestamp);
  PROFILER_MARKER_PAYLOAD("VsyncTimestamp", payload);
#endif
}

/*static */ void
CompositorParent::PostInsertVsyncProfilerMarker(TimeStamp aVsyncTimestamp)
{
  // Called in the vsync thread
  if (profiler_is_active() && sCompositorThreadHolder) {
    CompositorLoop()->PostTask(FROM_HERE,
      NewRunnableFunction(InsertVsyncProfilerMarker, aVsyncTimestamp));
  }
}

/* static */ void
CompositorParent::RequestNotifyLayerTreeReady(uint64_t aLayersId, CompositorUpdateObserver* aObserver)
{
  EnsureLayerTreeMapReady();
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees[aLayersId].mLayerTreeReadyObserver = aObserver;
}

/* static */ void
CompositorParent::RequestNotifyLayerTreeCleared(uint64_t aLayersId, CompositorUpdateObserver* aObserver)
{
  EnsureLayerTreeMapReady();
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees[aLayersId].mLayerTreeClearedObserver = aObserver;
}

/**
 * This class handles layer updates pushed directly from child
 * processes to the compositor thread.  It's associated with a
 * CompositorParent on the compositor thread.  While it uses the
 * PCompositor protocol to manage these updates, it doesn't actually
 * drive compositing itself.  For that it hands off work to the
 * CompositorParent it's associated with.
 */
class CrossProcessCompositorParent final : public PCompositorParent,
                                           public ShadowLayersManager
{
  friend class CompositorParent;

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING_WITH_MAIN_THREAD_DESTRUCTION(CrossProcessCompositorParent)
public:
  explicit CrossProcessCompositorParent(Transport* aTransport)
    : mTransport(aTransport)
    , mNotifyAfterRemotePaint(false)
  {
    MOZ_ASSERT(NS_IsMainThread());
  }

  // IToplevelProtocol::CloneToplevel()
  virtual IToplevelProtocol*
  CloneToplevel(const InfallibleTArray<mozilla::ipc::ProtocolFdMapping>& aFds,
                base::ProcessHandle aPeerProcess,
                mozilla::ipc::ProtocolCloneContext* aCtx) override;

  virtual void ActorDestroy(ActorDestroyReason aWhy) override;

  // FIXME/bug 774388: work out what shutdown protocol we need.
  virtual bool RecvRequestOverfill() override { return true; }
  virtual bool RecvWillStop() override { return true; }
  virtual bool RecvStop() override { return true; }
  virtual bool RecvPause() override { return true; }
  virtual bool RecvResume() override { return true; }
  virtual bool RecvNotifyHidden(const uint64_t& id) override;
  virtual bool RecvNotifyVisible(const uint64_t& id) override;
  virtual bool RecvNotifyChildCreated(const uint64_t& child) override;
  virtual bool RecvAdoptChild(const uint64_t& child) override { return false; }
  virtual bool RecvMakeSnapshot(const SurfaceDescriptor& aInSnapshot,
                                const gfx::IntRect& aRect) override
  { return true; }
  virtual bool RecvMakeWidgetSnapshot(const SurfaceDescriptor& aInSnapshot) override
  { return true; }
  virtual bool RecvFlushRendering() override { return true; }
  virtual bool RecvNotifyRegionInvalidated(const nsIntRegion& aRegion) override { return true; }
  virtual bool RecvStartFrameTimeRecording(const int32_t& aBufferSize, uint32_t* aOutStartIndex) override { return true; }
  virtual bool RecvStopFrameTimeRecording(const uint32_t& aStartIndex, InfallibleTArray<float>* intervals) override  { return true; }
  virtual bool RecvGetTileSize(int32_t* aWidth, int32_t* aHeight) override
  {
    *aWidth = gfxPlatform::GetPlatform()->GetTileWidth();
    *aHeight = gfxPlatform::GetPlatform()->GetTileHeight();
    return true;
  }

  virtual bool RecvGetFrameUniformity(FrameUniformityData* aOutData) override
  {
    // Don't support calculating frame uniformity on the child process and
    // this is just a stub for now.
    MOZ_ASSERT(false);
    return true;
  }

  /**
   * Tells this CompositorParent to send a message when the compositor has received the transaction.
   */
  virtual bool RecvRequestNotifyAfterRemotePaint() override;

  virtual PLayerTransactionParent*
    AllocPLayerTransactionParent(const nsTArray<LayersBackend>& aBackendHints,
                                 const uint64_t& aId,
                                 TextureFactoryIdentifier* aTextureFactoryIdentifier,
                                 bool *aSuccess) override;

  virtual bool DeallocPLayerTransactionParent(PLayerTransactionParent* aLayers) override;

  virtual void ShadowLayersUpdated(LayerTransactionParent* aLayerTree,
                                   const uint64_t& aTransactionId,
                                   const TargetConfig& aTargetConfig,
                                   const InfallibleTArray<PluginWindowData>& aPlugins,
                                   bool aIsFirstPaint,
                                   bool aScheduleComposite,
                                   uint32_t aPaintSequenceNumber,
                                   bool aIsRepeatTransaction,
                                   int32_t /*aPaintSyncId: unused*/) override;
  virtual void ForceComposite(LayerTransactionParent* aLayerTree) override;
  virtual void NotifyClearCachedResources(LayerTransactionParent* aLayerTree) override;
  virtual bool SetTestSampleTime(LayerTransactionParent* aLayerTree,
                                 const TimeStamp& aTime) override;
  virtual void LeaveTestMode(LayerTransactionParent* aLayerTree) override;
  virtual void ApplyAsyncProperties(LayerTransactionParent* aLayerTree)
               override;
  virtual void FlushApzRepaints(const LayerTransactionParent* aLayerTree) override;
  virtual void GetAPZTestData(const LayerTransactionParent* aLayerTree,
                              APZTestData* aOutData) override;
  virtual void SetConfirmedTargetAPZC(const LayerTransactionParent* aLayerTree,
                                      const uint64_t& aInputBlockId,
                                      const nsTArray<ScrollableLayerGuid>& aTargets) override;

  virtual AsyncCompositionManager* GetCompositionManager(LayerTransactionParent* aParent) override;
  virtual bool RecvRemotePluginsReady()  override { return false; }

  void DidComposite(uint64_t aId,
                    TimeStamp& aCompositeStart,
                    TimeStamp& aCompositeEnd);

protected:
  void OnChannelConnected(int32_t pid) override { mCompositorThreadHolder = sCompositorThreadHolder; }
private:
  // Private destructor, to discourage deletion outside of Release():
  virtual ~CrossProcessCompositorParent();

  void DeferredDestroy();

  // There can be many CPCPs, and IPDL-generated code doesn't hold a
  // reference to top-level actors.  So we hold a reference to
  // ourself.  This is released (deferred) in ActorDestroy().
  RefPtr<CrossProcessCompositorParent> mSelfRef;
  Transport* mTransport;

  RefPtr<CompositorThreadHolder> mCompositorThreadHolder;
  // If true, we should send a RemotePaintIsReady message when the layer transaction
  // is received
  bool mNotifyAfterRemotePaint;
};

void
CompositorParent::DidComposite(TimeStamp& aCompositeStart,
                               TimeStamp& aCompositeEnd)
{
  Unused << SendDidComposite(0, mPendingTransaction, aCompositeStart, aCompositeEnd);
  mPendingTransaction = 0;

  if (mLayerManager) {
    nsTArray<ImageCompositeNotification> notifications;
    mLayerManager->ExtractImageCompositeNotifications(&notifications);
    if (!notifications.IsEmpty()) {
      Unused << ImageBridgeParent::NotifyImageComposites(notifications);
    }
  }

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  for (LayerTreeMap::iterator it = sIndirectLayerTrees.begin();
       it != sIndirectLayerTrees.end(); it++) {
    LayerTreeState* lts = &it->second;
    if (lts->mParent == this && lts->mCrossProcessParent) {
      static_cast<CrossProcessCompositorParent*>(lts->mCrossProcessParent)->DidComposite(
        it->first, aCompositeStart, aCompositeEnd);
    }
  }
}

static void
OpenCompositor(CrossProcessCompositorParent* aCompositor,
               Transport* aTransport, ProcessId aOtherPid,
               MessageLoop* aIOLoop)
{
  DebugOnly<bool> ok = aCompositor->Open(aTransport, aOtherPid, aIOLoop);
  MOZ_ASSERT(ok);
}

/*static*/ PCompositorParent*
CompositorParent::Create(Transport* aTransport, ProcessId aOtherPid)
{
  gfxPlatform::InitLayersIPC();

  RefPtr<CrossProcessCompositorParent> cpcp =
    new CrossProcessCompositorParent(aTransport);

  cpcp->mSelfRef = cpcp;
  CompositorLoop()->PostTask(
    FROM_HERE,
    NewRunnableFunction(OpenCompositor, cpcp.get(),
                        aTransport, aOtherPid, XRE_GetIOMessageLoop()));
  // The return value is just compared to null for success checking,
  // we're not sharing a ref.
  return cpcp.get();
}

static void
UpdateIndirectTree(uint64_t aId, Layer* aRoot, const TargetConfig& aTargetConfig)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees[aId].mRoot = aRoot;
  sIndirectLayerTrees[aId].mTargetConfig = aTargetConfig;
}

/* static */ CompositorParent::LayerTreeState*
CompositorParent::GetIndirectShadowTree(uint64_t aId)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  LayerTreeMap::iterator cit = sIndirectLayerTrees.find(aId);
  if (sIndirectLayerTrees.end() == cit) {
    return nullptr;
  }
  return &cit->second;
}

bool
CrossProcessCompositorParent::RecvNotifyHidden(const uint64_t& id)
{
  RefPtr<CompositorLRU> lru = CompositorLRU::GetSingleton();
  lru->Add(this, id);
  return true;
}

bool
CrossProcessCompositorParent::RecvNotifyVisible(const uint64_t& id)
{
  RefPtr<CompositorLRU> lru = CompositorLRU::GetSingleton();
  lru->Remove(this, id);
  return true;
}

bool
CrossProcessCompositorParent::RecvRequestNotifyAfterRemotePaint()
{
  mNotifyAfterRemotePaint = true;
  return true;
}

void
CrossProcessCompositorParent::ActorDestroy(ActorDestroyReason aWhy)
{
  RefPtr<CompositorLRU> lru = CompositorLRU::GetSingleton();
  lru->Remove(this);

  MessageLoop::current()->PostTask(
    FROM_HERE,
    NewRunnableMethod(this, &CrossProcessCompositorParent::DeferredDestroy));
}

PLayerTransactionParent*
CrossProcessCompositorParent::AllocPLayerTransactionParent(const nsTArray<LayersBackend>&,
                                                           const uint64_t& aId,
                                                           TextureFactoryIdentifier* aTextureFactoryIdentifier,
                                                           bool *aSuccess)
{
  MOZ_ASSERT(aId != 0);

  MonitorAutoLock lock(*sIndirectLayerTreesLock);

  CompositorParent::LayerTreeState* state = nullptr;
  LayerTreeMap::iterator itr = sIndirectLayerTrees.find(aId);
  if (sIndirectLayerTrees.end() != itr) {
    state = &itr->second;
  }

  if (state && state->mLayerManager) {
    state->mCrossProcessParent = this;
    LayerManagerComposite* lm = state->mLayerManager;
    *aTextureFactoryIdentifier = lm->GetCompositor()->GetTextureFactoryIdentifier();
    *aSuccess = true;
    LayerTransactionParent* p = new LayerTransactionParent(lm, this, aId);
    p->AddIPDLReference();
    sIndirectLayerTrees[aId].mLayerTree = p;
    return p;
  }

  NS_WARNING("Created child without a matching parent?");
  // XXX: should be false, but that causes us to fail some tests on Mac w/ OMTC.
  // Bug 900745. change *aSuccess to false to see test failures.
  *aSuccess = true;
  LayerTransactionParent* p = new LayerTransactionParent(nullptr, this, aId);
  p->AddIPDLReference();
  return p;
}

bool
CrossProcessCompositorParent::DeallocPLayerTransactionParent(PLayerTransactionParent* aLayers)
{
  LayerTransactionParent* slp = static_cast<LayerTransactionParent*>(aLayers);
  EraseLayerState(slp->GetId());
  static_cast<LayerTransactionParent*>(aLayers)->ReleaseIPDLReference();
  return true;
}

bool
CrossProcessCompositorParent::RecvNotifyChildCreated(const uint64_t& child)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  for (LayerTreeMap::iterator it = sIndirectLayerTrees.begin();
       it != sIndirectLayerTrees.end(); it++) {
    CompositorParent::LayerTreeState* lts = &it->second;
    if (lts->mParent && lts->mCrossProcessParent == this) {
      lts->mParent->NotifyChildCreated(child);
      return true;
    }
  }
  return false;
}

void
CrossProcessCompositorParent::ShadowLayersUpdated(
  LayerTransactionParent* aLayerTree,
  const uint64_t& aTransactionId,
  const TargetConfig& aTargetConfig,
  const InfallibleTArray<PluginWindowData>& aPlugins,
  bool aIsFirstPaint,
  bool aScheduleComposite,
  uint32_t aPaintSequenceNumber,
  bool aIsRepeatTransaction,
  int32_t /*aPaintSyncId: unused*/)
{
  uint64_t id = aLayerTree->GetId();

  MOZ_ASSERT(id != 0);

  CompositorParent::LayerTreeState* state = CompositorParent::GetIndirectShadowTree(id);
  if (!state) {
    return;
  }
  MOZ_ASSERT(state->mParent);
  state->mParent->ScheduleRotationOnCompositorThread(aTargetConfig, aIsFirstPaint);

  Layer* shadowRoot = aLayerTree->GetRoot();
  if (shadowRoot) {
    CompositorParent::SetShadowProperties(shadowRoot);
  }
  UpdateIndirectTree(id, shadowRoot, aTargetConfig);

  // Cache the plugin data for this remote layer tree
  state->mPluginData = aPlugins;
  state->mUpdatedPluginDataAvailable = true;

  state->mParent->NotifyShadowTreeTransaction(id, aIsFirstPaint, aScheduleComposite,
      aPaintSequenceNumber, aIsRepeatTransaction);

  // Send the 'remote paint ready' message to the content thread if it has already asked.
  if(mNotifyAfterRemotePaint)  {
    Unused << SendRemotePaintIsReady();
    mNotifyAfterRemotePaint = false;
  }

  if (state->mLayerTreeReadyObserver) {
    RefPtr<CompositorUpdateObserver> observer = state->mLayerTreeReadyObserver;
    state->mLayerTreeReadyObserver = nullptr;
    observer->ObserveUpdate(id, true);
  }

  aLayerTree->SetPendingTransactionId(aTransactionId);
}

#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
//#define PLUGINS_LOG(...) printf_stderr("CP [%s]: ", __FUNCTION__);
//                         printf_stderr(__VA_ARGS__);
//                         printf_stderr("\n");
#define PLUGINS_LOG(...)

bool
CompositorParent::UpdatePluginWindowState(uint64_t aId)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  CompositorParent::LayerTreeState& lts = sIndirectLayerTrees[aId];
  if (!lts.mParent) {
    PLUGINS_LOG("[%" PRIu64 "] layer tree compositor parent pointer is null", aId);
    return false;
  }

  // Check if this layer tree has received any shadow layer updates
  if (!lts.mUpdatedPluginDataAvailable) {
    PLUGINS_LOG("[%" PRIu64 "] no plugin data", aId);
    return false;
  }

  // pluginMetricsChanged tracks whether we need to send plugin update
  // data to the main thread. If we do we'll have to block composition,
  // which we want to avoid if at all possible.
  bool pluginMetricsChanged = false;

  // Same layer tree checks
  if (mLastPluginUpdateLayerTreeId == aId) {
    // no plugin data and nothing has changed, bail.
    if (!mCachedPluginData.Length() && !lts.mPluginData.Length()) {
      PLUGINS_LOG("[%" PRIu64 "] no data, no changes", aId);
      return false;
    }

    if (mCachedPluginData.Length() == lts.mPluginData.Length()) {
      // check for plugin data changes
      for (uint32_t idx = 0; idx < lts.mPluginData.Length(); idx++) {
        if (!(mCachedPluginData[idx] == lts.mPluginData[idx])) {
          pluginMetricsChanged = true;
          break;
        }
      }
    } else {
      // array lengths don't match, need to update
      pluginMetricsChanged = true;
    }
  } else {
    // exchanging layer trees, we need to update
    pluginMetricsChanged = true;
  }

  // Check if plugin windows are currently hidden due to scrolling
  if (mDeferPluginWindows) {
    PLUGINS_LOG("[%" PRIu64 "] suppressing", aId);
    return false;
  }

  if (!lts.mPluginData.Length()) {
    // We will pass through here in cases where the previous shadow layer
    // tree contained visible plugins and the new tree does not. All we need
    // to do here is hide the plugins for the old tree, so don't waste time
    // calculating clipping.
    mPluginsLayerOffset = nsIntPoint(0,0);
    mPluginsLayerVisibleRegion.SetEmpty();
    uintptr_t parentWidget = (uintptr_t)lts.mParent->GetWidget();
    Unused << lts.mParent->SendHideAllPlugins(parentWidget);
    lts.mUpdatedPluginDataAvailable = false;
    PLUGINS_LOG("[%" PRIu64 "] hide all", aId);
  } else {
    // Retrieve the offset and visible region of the layer that hosts
    // the plugins, CompositorChild needs these in calculating proper
    // plugin clipping.
    LayerTransactionParent* layerTree = lts.mLayerTree;
    Layer* contentRoot = layerTree->GetRoot();
    if (contentRoot) {
      nsIntPoint offset;
      nsIntRegion visibleRegion;
      if (contentRoot->GetVisibleRegionRelativeToRootLayer(visibleRegion,
                                                            &offset)) {
        // Check to see if these values have changed, if so we need to
        // update plugin window position within the window.
        if (!pluginMetricsChanged &&
            mPluginsLayerVisibleRegion == visibleRegion &&
            mPluginsLayerOffset == offset) {
          PLUGINS_LOG("[%" PRIu64 "] no change", aId);
          return false;
        }
        mPluginsLayerOffset = offset;
        mPluginsLayerVisibleRegion = visibleRegion;
        Unused << lts.mParent->SendUpdatePluginConfigurations(
          LayoutDeviceIntPoint::FromUnknownPoint(offset),
          LayoutDeviceIntRegion::FromUnknownRegion(visibleRegion),
          lts.mPluginData);
        lts.mUpdatedPluginDataAvailable = false;
        PLUGINS_LOG("[%" PRIu64 "] updated", aId);
      } else {
        PLUGINS_LOG("[%" PRIu64 "] no visibility data", aId);
        return false;
      }
    } else {
      PLUGINS_LOG("[%" PRIu64 "] no content root", aId);
      return false;
    }
  }

  mLastPluginUpdateLayerTreeId = aId;
  mCachedPluginData = lts.mPluginData;
  return true;
}

void
CompositorParent::ScheduleShowAllPluginWindows()
{
  CancelableTask *pluginTask =
    NewRunnableMethod(this, &CompositorParent::ShowAllPluginWindows);
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(FROM_HERE, pluginTask);
}

void
CompositorParent::ShowAllPluginWindows()
{
  MOZ_ASSERT(!NS_IsMainThread());
  mDeferPluginWindows = false;
  ScheduleComposition();
}

void
CompositorParent::ScheduleHideAllPluginWindows()
{
  CancelableTask *pluginTask =
    NewRunnableMethod(this, &CompositorParent::HideAllPluginWindows);
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(FROM_HERE, pluginTask);
}

void
CompositorParent::HideAllPluginWindows()
{
  MOZ_ASSERT(!NS_IsMainThread());
  // No plugins in the cache implies no plugins to manage
  // in this content.
  if (!mCachedPluginData.Length() || mDeferPluginWindows) {
    return;
  }
  mDeferPluginWindows = true;
  mPluginUpdateResponsePending = true;
  Unused << SendHideAllPlugins((uintptr_t)GetWidget());
  ScheduleComposition();
}
#endif // #if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)

void
CrossProcessCompositorParent::DidComposite(uint64_t aId,
                                           TimeStamp& aCompositeStart,
                                           TimeStamp& aCompositeEnd)
{
  sIndirectLayerTreesLock->AssertCurrentThreadOwns();
  if (LayerTransactionParent *layerTree = sIndirectLayerTrees[aId].mLayerTree) {
    Unused << SendDidComposite(aId, layerTree->GetPendingTransactionId(), aCompositeStart, aCompositeEnd);
    layerTree->SetPendingTransactionId(0);
  }
}

void
CrossProcessCompositorParent::ForceComposite(LayerTransactionParent* aLayerTree)
{
  uint64_t id = aLayerTree->GetId();
  MOZ_ASSERT(id != 0);
  CompositorParent* parent;
  { // scope lock
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    parent = sIndirectLayerTrees[id].mParent;
  }
  if (parent) {
    parent->ForceComposite(aLayerTree);
  }
}

void
CrossProcessCompositorParent::NotifyClearCachedResources(LayerTransactionParent* aLayerTree)
{
  uint64_t id = aLayerTree->GetId();
  MOZ_ASSERT(id != 0);

  RefPtr<CompositorUpdateObserver> observer;
  { // scope lock
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    observer = sIndirectLayerTrees[id].mLayerTreeClearedObserver;
    sIndirectLayerTrees[id].mLayerTreeClearedObserver = nullptr;
  }
  if (observer) {
    observer->ObserveUpdate(id, false);
  }
}

bool
CrossProcessCompositorParent::SetTestSampleTime(
  LayerTransactionParent* aLayerTree, const TimeStamp& aTime)
{
  uint64_t id = aLayerTree->GetId();
  MOZ_ASSERT(id != 0);
  const CompositorParent::LayerTreeState* state = CompositorParent::GetIndirectShadowTree(id);
  if (!state) {
    return false;
  }

  MOZ_ASSERT(state->mParent);
  return state->mParent->SetTestSampleTime(aLayerTree, aTime);
}

void
CrossProcessCompositorParent::LeaveTestMode(LayerTransactionParent* aLayerTree)
{
  uint64_t id = aLayerTree->GetId();
  MOZ_ASSERT(id != 0);
  const CompositorParent::LayerTreeState* state = CompositorParent::GetIndirectShadowTree(id);
  if (!state) {
    return;
  }

  MOZ_ASSERT(state->mParent);
  state->mParent->LeaveTestMode(aLayerTree);
}

void
CrossProcessCompositorParent::ApplyAsyncProperties(
    LayerTransactionParent* aLayerTree)
{
  uint64_t id = aLayerTree->GetId();
  MOZ_ASSERT(id != 0);
  const CompositorParent::LayerTreeState* state =
    CompositorParent::GetIndirectShadowTree(id);
  if (!state) {
    return;
  }

  MOZ_ASSERT(state->mParent);
  state->mParent->ApplyAsyncProperties(aLayerTree);
}

void
CrossProcessCompositorParent::FlushApzRepaints(const LayerTransactionParent* aLayerTree)
{
  uint64_t id = aLayerTree->GetId();
  MOZ_ASSERT(id != 0);
  const CompositorParent::LayerTreeState* state =
    CompositorParent::GetIndirectShadowTree(id);
  if (!state) {
    return;
  }

  MOZ_ASSERT(state->mParent);
  state->mParent->FlushApzRepaints(aLayerTree);
}

void
CrossProcessCompositorParent::GetAPZTestData(const LayerTransactionParent* aLayerTree,
                                             APZTestData* aOutData)
{
  uint64_t id = aLayerTree->GetId();
  MOZ_ASSERT(id != 0);
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  *aOutData = sIndirectLayerTrees[id].mApzTestData;
}

void
CrossProcessCompositorParent::SetConfirmedTargetAPZC(const LayerTransactionParent* aLayerTree,
                                                     const uint64_t& aInputBlockId,
                                                     const nsTArray<ScrollableLayerGuid>& aTargets)
{
  uint64_t id = aLayerTree->GetId();
  MOZ_ASSERT(id != 0);
  const CompositorParent::LayerTreeState* state = CompositorParent::GetIndirectShadowTree(id);
  if (!state) {
    return;
  }

  MOZ_ASSERT(state->mParent);
  state->mParent->SetConfirmedTargetAPZC(aLayerTree, aInputBlockId, aTargets);
}

AsyncCompositionManager*
CrossProcessCompositorParent::GetCompositionManager(LayerTransactionParent* aLayerTree)
{
  uint64_t id = aLayerTree->GetId();
  const CompositorParent::LayerTreeState* state = CompositorParent::GetIndirectShadowTree(id);
  if (!state) {
    return nullptr;
  }

  MOZ_ASSERT(state->mParent);
  return state->mParent->GetCompositionManager(aLayerTree);
}

void
CrossProcessCompositorParent::DeferredDestroy()
{
  MOZ_ASSERT(mCompositorThreadHolder);
  mCompositorThreadHolder = nullptr;
  mSelfRef = nullptr;
}

CrossProcessCompositorParent::~CrossProcessCompositorParent()
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(XRE_GetIOMessageLoop());
  XRE_GetIOMessageLoop()->PostTask(FROM_HERE,
                                   new DeleteTask<Transport>(mTransport));
}

IToplevelProtocol*
CrossProcessCompositorParent::CloneToplevel(const InfallibleTArray<mozilla::ipc::ProtocolFdMapping>& aFds,
                                            base::ProcessHandle aPeerProcess,
                                            mozilla::ipc::ProtocolCloneContext* aCtx)
{
  for (unsigned int i = 0; i < aFds.Length(); i++) {
    if (aFds[i].protocolId() == (unsigned)GetProtocolId()) {
      Transport* transport = OpenDescriptor(aFds[i].fd(),
                                            Transport::MODE_SERVER);
      PCompositorParent* compositor =
        CompositorParent::Create(transport, base::GetProcId(aPeerProcess));
      compositor->CloneManagees(this, aCtx);
      compositor->IToplevelProtocol::SetTransport(transport);
      // The reference to the compositor thread is held in OnChannelConnected().
      // We need to do this for cloned actors, too.
      compositor->OnChannelConnected(base::GetProcId(aPeerProcess));
      return compositor;
    }
  }
  return nullptr;
}

} // namespace layers
} // namespace mozilla
