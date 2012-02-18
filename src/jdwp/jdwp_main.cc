/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * JDWP initialization.
 */

#include "atomic.h"
#include "debugger.h"
#include "jdwp/jdwp_priv.h"
#include "logging.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

namespace art {

namespace JDWP {

static void* StartJdwpThread(void* arg);

/*
 * JdwpNetStateBase class implementation
 */
JdwpNetStateBase::JdwpNetStateBase() : socket_lock_("JdwpNetStateBase lock") {
  clientSock = -1;
}

/*
 * Write a packet. Grabs a mutex to assure atomicity.
 */
ssize_t JdwpNetStateBase::writePacket(ExpandBuf* pReply) {
  MutexLock mu(socket_lock_);
  return write(clientSock, expandBufGetBuffer(pReply), expandBufGetLength(pReply));
}

/*
 * Write a buffered packet. Grabs a mutex to assure atomicity.
 */
ssize_t JdwpNetStateBase::writeBufferedPacket(const iovec* iov, int iov_count) {
  MutexLock mu(socket_lock_);
  return writev(clientSock, iov, iov_count);
}

bool JdwpState::IsConnected() {
  return (*transport->isConnected)(this);
}

bool JdwpState::SendRequest(ExpandBuf* pReq) {
  return (*transport->sendRequest)(this, pReq);
}

/*
 * Get the next "request" serial number.  We use this when sending
 * packets to the debugger.
 */
uint32_t JdwpState::NextRequestSerial() {
  MutexLock mu(serial_lock_);
  return requestSerial++;
}

/*
 * Get the next "event" serial number.  We use this in the response to
 * message type EventRequest.Set.
 */
uint32_t JdwpState::NextEventSerial() {
  MutexLock mu(serial_lock_);
  return eventSerial++;
}

JdwpState::JdwpState(const JdwpOptions* options)
    : options_(options),
      thread_start_lock_("JDWP thread start lock"),
      thread_start_cond_("JDWP thread start condition variable"),
      debug_thread_started_(false),
      debugThreadId(0),
      run(false),
      transport(NULL),
      netState(NULL),
      attach_lock_("JDWP attach lock"),
      attach_cond_("JDWP attach condition variable"),
      lastActivityWhen(0),
      requestSerial(0x10000000),
      eventSerial(0x20000000),
      serial_lock_("JDWP serial lock"),
      numEvents(0),
      eventList(NULL),
      event_lock_("JDWP event lock"),
      event_thread_lock_("JDWP event thread lock"),
      event_thread_cond_("JDWP event thread condition variable"),
      eventThreadId(0),
      ddmActive(false) {
}

/*
 * Initialize JDWP.
 *
 * Does not return until JDWP thread is running, but may return before
 * the thread is accepting network connections.
 */
JdwpState* JdwpState::Create(const JdwpOptions* options) {
  /* comment this out when debugging JDWP itself */
  //android_setMinPriority(LOG_TAG, ANDROID_LOG_DEBUG);

  UniquePtr<JdwpState> state(new JdwpState(options));
  switch (options->transport) {
  case kJdwpTransportSocket:
    // LOGD("prepping for JDWP over TCP");
    state->transport = SocketTransport();
    break;
#ifdef HAVE_ANDROID_OS
  case kJdwpTransportAndroidAdb:
    // LOGD("prepping for JDWP over ADB");
    state->transport = AndroidAdbTransport();
    break;
#endif
  default:
    LOG(FATAL) << "Unknown transport: " << options->transport;
  }

  if (!(*state->transport->startup)(state.get(), options)) {
    return NULL;
  }

  /*
   * Grab a mutex or two before starting the thread.  This ensures they
   * won't signal the cond var before we're waiting.
   */
  state->thread_start_lock_.Lock();
  if (options->suspend) {
    state->attach_lock_.Lock();
  }

  /*
   * We have bound to a port, or are trying to connect outbound to a
   * debugger.  Create the JDWP thread and let it continue the mission.
   */
  CHECK_PTHREAD_CALL(pthread_create, (&state->pthread_, NULL, StartJdwpThread, state.get()), "JDWP thread");

  /*
   * Wait until the thread finishes basic initialization.
   * TODO: cond vars should be waited upon in a loop
   */
  state->thread_start_cond_.Wait(state->thread_start_lock_);
  state->thread_start_lock_.Unlock();

  /*
   * For suspend=y, wait for the debugger to connect to us or for us to
   * connect to the debugger.
   *
   * The JDWP thread will signal us when it connects successfully or
   * times out (for timeout=xxx), so we have to check to see what happened
   * when we wake up.
   */
  if (options->suspend) {
    {
      ScopedThreadStateChange tsc(Thread::Current(), Thread::kVmWait);

      state->attach_cond_.Wait(state->attach_lock_);
      state->attach_lock_.Unlock();
    }

    if (!state->IsActive()) {
      LOG(ERROR) << "JDWP connection failed";
      return NULL;
    }

    LOG(INFO) << "JDWP connected";

    /*
     * Ordinarily we would pause briefly to allow the debugger to set
     * breakpoints and so on, but for "suspend=y" the VM init code will
     * pause the VM when it sends the VM_START message.
     */
  }

  return state.release();
}

/*
 * Reset all session-related state.  There should not be an active connection
 * to the client at this point.  The rest of the VM still thinks there is
 * a debugger attached.
 *
 * This includes freeing up the debugger event list.
 */
void JdwpState::ResetState() {
  /* could reset the serial numbers, but no need to */

  UnregisterAll();
  CHECK(eventList == NULL);

  /*
   * Should not have one of these in progress.  If the debugger went away
   * mid-request, though, we could see this.
   */
  if (eventThreadId != 0) {
    LOG(WARNING) << "Resetting state while event in progress";
    DCHECK(false);
  }
}

/*
 * Tell the JDWP thread to shut down.  Frees "state".
 */
JdwpState::~JdwpState() {
  if (transport != NULL) {
    if (IsConnected()) {
      PostVMDeath();
    }

    /*
     * Close down the network to inspire the thread to halt.
     */
    VLOG(jdwp) << "JDWP shutting down net...";
    (*transport->shutdown)(this);

    if (debug_thread_started_) {
      run = false;
      void* threadReturn;
      if (pthread_join(pthread_, &threadReturn) != 0) {
        LOG(WARNING) << "JDWP thread join failed";
      }
    }

    VLOG(jdwp) << "JDWP freeing netstate...";
    (*transport->free)(this);
    netState = NULL;
  }
  CHECK(netState == NULL);

  ResetState();
}

/*
 * Are we talking to a debugger?
 */
bool JdwpState::IsActive() {
  return IsConnected();
}

/*
 * Entry point for JDWP thread.  The thread was created through the VM
 * mechanisms, so there is a java/lang/Thread associated with us.
 */
static void* StartJdwpThread(void* arg) {
  JdwpState* state = reinterpret_cast<JdwpState*>(arg);
  CHECK(state != NULL);

  state->Run();
  return NULL;
}

void JdwpState::Run() {
  Runtime* runtime = Runtime::Current();
  runtime->AttachCurrentThread("JDWP", true);

  VLOG(jdwp) << "JDWP: thread running";

  /*
   * Finish initializing, then notify the creating thread that
   * we're running.
   */
  thread_ = Thread::Current();
  run = true;
  android_atomic_release_store(true, &debug_thread_started_);

  thread_start_lock_.Lock();
  thread_start_cond_.Broadcast();
  thread_start_lock_.Unlock();

  /* set the thread state to VMWAIT so GCs don't wait for us */
  Dbg::ThreadWaiting();

  /*
   * Loop forever if we're in server mode, processing connections.  In
   * non-server mode, we bail out of the thread when the debugger drops
   * us.
   *
   * We broadcast a notification when a debugger attaches, after we
   * successfully process the handshake.
   */
  while (run) {
    if (options_->server) {
      /*
       * Block forever, waiting for a connection.  To support the
       * "timeout=xxx" option we'll need to tweak this.
       */
      if (!(*transport->accept)(this)) {
        break;
      }
    } else {
      /*
       * If we're not acting as a server, we need to connect out to the
       * debugger.  To support the "timeout=xxx" option we need to
       * have a timeout if the handshake reply isn't received in a
       * reasonable amount of time.
       */
      if (!(*transport->establish)(this)) {
        /* wake anybody who was waiting for us to succeed */
        MutexLock mu(attach_lock_);
        attach_cond_.Broadcast();
        break;
      }
    }

    /* prep debug code to handle the new connection */
    Dbg::Connected();

    /* process requests until the debugger drops */
    bool first = true;
    while (!Dbg::IsDisposed()) {
      // sanity check -- shouldn't happen?
      if (Thread::Current()->GetState() != Thread::kVmWait) {
        LOG(ERROR) << "JDWP thread no longer in VMWAIT (now " << Thread::Current()->GetState() << "); resetting";
        Dbg::ThreadWaiting();
      }

      if (!(*transport->processIncoming)(this)) {
        /* blocking read */
        break;
      }

      if (first && !(*transport->awaitingHandshake)(this)) {
        /* handshake worked, tell the interpreter that we're active */
        first = false;

        /* set thread ID; requires object registry to be active */
        debugThreadId = Dbg::GetThreadSelfId();

        /* wake anybody who's waiting for us */
        MutexLock mu(attach_lock_);
        attach_cond_.Broadcast();
      }
    }

    (*transport->close)(this);

    if (ddmActive) {
      ddmActive = false;

      /* broadcast the disconnect; must be in RUNNING state */
      Dbg::ThreadRunning();
      Dbg::DdmDisconnected();
      Dbg::ThreadWaiting();
    }

    /* release session state, e.g. remove breakpoint instructions */
    ResetState();

    /* tell the interpreter that the debugger is no longer around */
    Dbg::Disconnected();

    /* if we had threads suspended, resume them now */
    Dbg::UndoDebuggerSuspensions();

    /* if we connected out, this was a one-shot deal */
    if (!options_->server) {
      run = false;
    }
  }

  /* back to running, for thread shutdown */
  Dbg::ThreadRunning();

  VLOG(jdwp) << "JDWP: thread detaching and exiting...";
  runtime->DetachCurrentThread();
}

Thread* JdwpState::GetDebugThread() {
  return thread_;
}

/*
 * Support routines for waitForDebugger().
 *
 * We can't have a trivial "waitForDebugger" function that returns the
 * instant the debugger connects, because we run the risk of executing code
 * before the debugger has had a chance to configure breakpoints or issue
 * suspend calls.  It would be nice to just sit in the suspended state, but
 * most debuggers don't expect any threads to be suspended when they attach.
 *
 * There's no JDWP event we can post to tell the debugger, "we've stopped,
 * and we like it that way".  We could send a fake breakpoint, which should
 * cause the debugger to immediately send a resume, but the debugger might
 * send the resume immediately or might throw an exception of its own upon
 * receiving a breakpoint event that it didn't ask for.
 *
 * What we really want is a "wait until the debugger is done configuring
 * stuff" event.  We can approximate this with a "wait until the debugger
 * has been idle for a brief period".
 */

/*
 * Return the time, in milliseconds, since the last debugger activity.
 *
 * Returns -1 if no debugger is attached, or 0 if we're in the middle of
 * processing a debugger request.
 */
int64_t JdwpState::LastDebuggerActivity() {
  if (!Dbg::IsDebuggerConnected()) {
    LOG(DEBUG) << "no active debugger";
    return -1;
  }

  int64_t last = QuasiAtomicRead64(&lastActivityWhen);

  /* initializing or in the middle of something? */
  if (last == 0) {
    VLOG(jdwp) << "+++ last=busy";
    return 0;
  }

  /* now get the current time */
  int64_t now = MilliTime();
  CHECK_GE(now, last);

  VLOG(jdwp) << "+++ debugger interval=" << (now - last);
  return now - last;
}

static const char* kTransportNames[] = {
  "Unknown",
  "Socket",
  "AndroidAdb",
};
std::ostream& operator<<(std::ostream& os, const JdwpTransportType& value) {
  int32_t int_value = static_cast<int32_t>(value);
  if (value >= kJdwpTransportUnknown && value <= kJdwpTransportAndroidAdb) {
    os << kTransportNames[int_value];
  } else {
    os << "JdwpTransportType[" << int_value << "]";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const JdwpLocation& rhs) {
  os << "JdwpLocation["
     << Dbg::GetClassName(rhs.classId) << "." << Dbg::GetMethodName(rhs.classId, rhs.methodId)
     << "@" << rhs.idx << " " << rhs.typeTag << "]";
  return os;
}

bool operator==(const JdwpLocation& lhs, const JdwpLocation& rhs) {
  return lhs.idx == rhs.idx && lhs.methodId == rhs.methodId &&
      lhs.classId == rhs.classId && lhs.typeTag == rhs.typeTag;
}

bool operator!=(const JdwpLocation& lhs, const JdwpLocation& rhs) {
  return !(lhs == rhs);
}

std::ostream& operator<<(std::ostream& os, const JdwpTag& value) {
  switch (value) {
  case JT_ARRAY: os << "JT_ARRAY"; break;
  case JT_BYTE: os << "JT_BYTE"; break;
  case JT_CHAR: os << "JT_CHAR"; break;
  case JT_OBJECT: os << "JT_OBJECT"; break;
  case JT_FLOAT: os << "JT_FLOAT"; break;
  case JT_DOUBLE: os << "JT_DOUBLE"; break;
  case JT_INT: os << "JT_INT"; break;
  case JT_LONG: os << "JT_LONG"; break;
  case JT_SHORT: os << "JT_SHORT"; break;
  case JT_VOID: os << "JT_VOID"; break;
  case JT_BOOLEAN: os << "JT_BOOLEAN"; break;
  case JT_STRING: os << "JT_STRING"; break;
  case JT_THREAD: os << "JT_THREAD"; break;
  case JT_THREAD_GROUP: os << "JT_THREAD_GROUP"; break;
  case JT_CLASS_LOADER: os << "JT_CLASS_LOADER"; break;
  case JT_CLASS_OBJECT: os << "JT_CLASS_OBJECT"; break;
  default:
    os << "JdwpTag[" << static_cast<int32_t>(value) << "]";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const JdwpTypeTag& value) {
  switch (value) {
  case TT_CLASS: os << "TT_CLASS"; break;
  case TT_INTERFACE: os << "TT_INTERFACE"; break;
  case TT_ARRAY: os << "TT_ARRAY"; break;
  default:
    os << "JdwpTypeTag[" << static_cast<int32_t>(value) << "]";
  }
  return os;
}

}  // namespace JDWP

}  // namespace art
