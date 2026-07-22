#include "thread_priority.h"

#include "Logging/app_logger.h"

#ifndef CDSP_TEST
static const logger_t g_logger = {"dsp.threadpriority"};
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif
#include <pthread.h>
#include <stdio.h>

/// Bind the *calling* thread to a Mach time-constraint scheduling policy
/// tailored to the given audio buffer parameters.
///
/// This is the standard Darwin/macOS idiom for real-time audio threads.
///
/// - Parameters:
///   - name: A descriptive name of the thread (e.g. Capture, Playback,
///   Processing).
///   - buffer_frames: The buffer size in frames.
///   - sample_rate: The sample rate in Hz.
#ifdef __APPLE__
void set_realtime_thread_priority(const char* name, size_t buffer_frames,
                                  size_t sample_rate) {
#ifdef CDSP_TEST
  (void)name;
  (void)buffer_frames;
  (void)sample_rate;
  return;
#else
  if (buffer_frames == 0 || sample_rate == 0) {
    logger_warn(&g_logger,
                "[%s] Invalid audio parameters for real-time priority: "
                "frames=%d, rate=%d",
                name ? name : "unknown", buffer_frames, sample_rate);
    return;
  }

  mach_timebase_info_data_t tb_info;
  kern_return_t status = mach_timebase_info(&tb_info);
  if (status != KERN_SUCCESS) {
    logger_error(&g_logger, "[%s] Failed to retrieve Mach timebase info: %d",
                 name ? name : "unknown", status);
    return;
  }

  // Calculate nominal buffer period in nanoseconds.
  double period_ns =
      ((double)buffer_frames * 1000000000.0) / (double)sample_rate;

  // Allocate a computation budget (50% of the period) and constraint (100% of
  // the period).
  double computation_ns = period_ns * 0.5;
  double constraint_ns = period_ns;

  // Cap computation budget at 50ms per macOS limits.
  // Darwin prevents setting real-time quanta larger than 50ms to protect
  // the system from lockups if a real-time thread spins endlessly.
  double max_quantum_ns = 50000000.0;
  if (computation_ns > max_quantum_ns) {
    logger_info(
        &g_logger,
        "[%s] Thread computation budget capped at 50.0ms (%.1fms requested)",
        name ? name : "unknown", computation_ns / 1000000.0);
    computation_ns = max_quantum_ns;
  }

  // Convert nanoseconds to Mach absolute time units:
  // mach_units = nanoseconds * denom / numer.
  // This is required because macOS scheduling policies operate on CPU-specific
  // bus cycles / absolute time units rather than standard SI time.
  double numer = (double)tb_info.numer;
  double denom = (double)tb_info.denom;

  uint32_t period_mach = (uint32_t)((period_ns * denom) / numer);
  uint32_t computation_mach = (uint32_t)((computation_ns * denom) / numer);
  uint32_t constraint_mach = (uint32_t)((constraint_ns * denom) / numer);

  thread_time_constraint_policy_data_t policy = {
      .period = period_mach,
      .computation = computation_mach,
      .constraint = constraint_mach,
      .preemptible = 1};

  mach_msg_type_number_t count =
      sizeof(thread_time_constraint_policy_data_t) / sizeof(integer_t);
  thread_port_t thread = mach_thread_self();

  kern_return_t result = thread_policy_set(
      thread, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy, count);
  mach_port_deallocate(mach_task_self(), thread);

  if (result == KERN_SUCCESS) {
    logger_info(&g_logger,
                "[%s] Thread promoted to real-time priority: period=%.1fms, "
                "computation=%.1fms, constraint=%.1fms",
                name ? name : "unknown", period_ns / 1000000.0,
                computation_ns / 1000000.0, constraint_ns / 1000000.0);
  } else {
    logger_warn(&g_logger, "[%s] Failed to set real-time thread policy: %d",
                name ? name : "unknown", result);
  }
#endif
}
#elif defined(__linux__)
#if !defined(NO_DBUS) && !defined(DISABLE_DBUS)
#include <dbus/dbus.h>
#define CDSP_HAS_DBUS 1
#endif
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
  long thread_id;
  pthread_t pthread_id;
  pid_t pid;
  int policy;
  struct sched_param sched_param;
} RtPriorityThreadInfoInternal;

typedef struct {
  RtPriorityThreadInfoInternal thread_info;
} RtPriorityHandleInternal;

#if defined(CDSP_HAS_DBUS)
static bool get_rtkit_property(DBusConnection* conn, const char* prop_name,
                               int64_t* out_val, DBusError* err) {
  DBusMessage* msg = dbus_message_new_method_call(
      "org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1",
      "org.freedesktop.DBus.Properties", "Get");
  if (!msg) {
    dbus_set_error(err, "org.freedesktop.DBus.Error.Failed",
                   "Failed to create method call");
    return false;
  }

  const char* iface = "org.freedesktop.RealtimeKit1";
  if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING,
                                &prop_name, DBUS_TYPE_INVALID)) {
    dbus_message_unref(msg);
    dbus_set_error(err, "org.freedesktop.DBus.Error.Failed",
                   "Failed to append arguments");
    return false;
  }

  DBusMessage* reply =
      dbus_connection_send_with_reply_and_block(conn, msg, 10000, err);
  dbus_message_unref(msg);

  if (dbus_error_is_set(err)) {
    if (reply) dbus_message_unref(reply);
    return false;
  }

  if (!reply) {
    dbus_set_error(err, "org.freedesktop.DBus.Error.Failed",
                   "No reply received");
    return false;
  }

  bool success = false;
  DBusMessageIter iter;
  if (dbus_message_iter_init(reply, &iter)) {
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
      DBusMessageIter sub_iter;
      dbus_message_iter_recurse(&iter, &sub_iter);
      int type = dbus_message_iter_get_arg_type(&sub_iter);
      if (type == DBUS_TYPE_INT32) {
        dbus_int32_t val;
        dbus_message_iter_get_basic(&sub_iter, &val);
        *out_val = val;
        success = true;
      } else if (type == DBUS_TYPE_INT64) {
        dbus_int64_t val;
        dbus_message_iter_get_basic(&sub_iter, &val);
        *out_val = val;
        success = true;
      }
    }
  }

  if (!success) {
    dbus_set_error(err, "org.freedesktop.DBus.Error.Failed",
                   "Failed to parse variant reply");
  }

  dbus_message_unref(reply);
  return success;
}

static bool rtkit_set_realtime(DBusConnection* conn, uint64_t thread,
                               uint64_t pid, uint32_t prio, DBusError* err) {
  DBusMessage* m;
  pid_t my_pid = getpid();
  if ((uint64_t)my_pid == pid) {
    m = dbus_message_new_method_call(
        "org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1",
        "org.freedesktop.RealtimeKit1", "MakeThreadRealtime");
    if (!m) {
      dbus_set_error(err, "org.freedesktop.DBus.Error.Failed",
                     "Failed to create method call");
      return false;
    }
    dbus_uint64_t dbus_thread = thread;
    dbus_uint32_t dbus_prio = prio;
    if (!dbus_message_append_args(m, DBUS_TYPE_UINT64, &dbus_thread,
                                  DBUS_TYPE_UINT32, &dbus_prio,
                                  DBUS_TYPE_INVALID)) {
      dbus_message_unref(m);
      dbus_set_error(err, "org.freedesktop.DBus.Error.Failed",
                     "Failed to append args");
      return false;
    }
  } else {
    m = dbus_message_new_method_call(
        "org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1",
        "org.freedesktop.RealtimeKit1", "MakeThreadRealtimeWithPID");
    if (!m) {
      dbus_set_error(err, "org.freedesktop.DBus.Error.Failed",
                     "Failed to create method call");
      return false;
    }
    dbus_uint64_t dbus_pid = pid;
    dbus_uint64_t dbus_thread = thread;
    dbus_uint32_t dbus_prio = prio;
    if (!dbus_message_append_args(
            m, DBUS_TYPE_UINT64, &dbus_pid, DBUS_TYPE_UINT64, &dbus_thread,
            DBUS_TYPE_UINT32, &dbus_prio, DBUS_TYPE_INVALID)) {
      dbus_message_unref(m);
      dbus_set_error(err, "org.freedesktop.DBus.Error.Failed",
                     "Failed to append args");
      return false;
    }
  }

  DBusMessage* reply =
      dbus_connection_send_with_reply_and_block(conn, m, 10000, err);
  dbus_message_unref(m);

  if (dbus_error_is_set(err)) {
    if (reply) dbus_message_unref(reply);
    return false;
  }

  if (reply) dbus_message_unref(reply);
  return true;
}

static bool get_limits(DBusConnection* conn, int64_t* max_prio,
                       uint64_t* max_rttime, struct rlimit* current_limit,
                       DBusError* err) {
  int64_t val_prio;
  if (!get_rtkit_property(conn, "MaxRealtimePriority", &val_prio, err)) {
    return false;
  }
  if (val_prio < 0) {
    dbus_set_error(err, "org.freedesktop.DBus.Error.InvalidArgs",
                   "invalid negative MaxRealtimePriority");
    return false;
  }
  *max_prio = val_prio;

  int64_t val_rttime;
  if (!get_rtkit_property(conn, "RTTimeUSecMax", &val_rttime, err)) {
    return false;
  }
  if (val_rttime < 0) {
    dbus_set_error(err, "org.freedesktop.DBus.Error.InvalidArgs",
                   "invalid negative RTTimeUSecMax");
    return false;
  }
  *max_rttime = (uint64_t)val_rttime;

  if (getrlimit(RLIMIT_RTTIME, current_limit) < 0) {
    dbus_set_error(err, "org.freedesktop.DBus.Error.Failed",
                   "getrlimit failed");
    return false;
  }

  return true;
}

static bool set_limits(uint64_t request, uint64_t max, DBusError* err) {
  struct rlimit new_limit;
  new_limit.rlim_cur = (rlim_t)request;
  new_limit.rlim_max = (rlim_t)max;

  if (setrlimit(RLIMIT_RTTIME, &new_limit) < 0) {
    dbus_set_error(err, "org.freedesktop.DBus.Error.Failed",
                   "setrlimit failed");
    return false;
  }
  return true;
}

static bool get_current_thread_info_internal(
    RtPriorityThreadInfoInternal* out_info, DBusError* err) {
  long thread_id = syscall(SYS_gettid);
  pthread_t pthread_id = pthread_self();
  struct sched_param param;
  int policy = 0;

  if (pthread_getschedparam(pthread_id, &policy, &param) < 0) {
    dbus_set_error(err, "org.freedesktop.DBus.Error.Failed",
                   "pthread_getschedparam failed");
    return false;
  }

  pid_t pid = getpid();

  out_info->pid = pid;
  out_info->thread_id = thread_id;
  out_info->pthread_id = pthread_id;
  out_info->policy = policy;
  out_info->sched_param = param;

  return true;
}

static bool set_real_time_hard_limit_internal(DBusConnection* conn,
                                              uint32_t audio_buffer_frames,
                                              uint32_t audio_samplerate_hz,
                                              DBusError* err) {
  uint32_t buffer_frames = audio_buffer_frames > 0 ? audio_buffer_frames
                                                   : (audio_samplerate_hz / 20);
  uint64_t budget_us =
      ((uint64_t)buffer_frames * 1000000ULL) / (uint64_t)audio_samplerate_hz;

  int64_t max_prio = 0;
  uint64_t max_rttime = 0;
  struct rlimit limits;
  if (!get_limits(conn, &max_prio, &max_rttime, &limits, err)) {
    return false;
  }

  uint64_t rttime_request = budget_us < max_rttime ? budget_us : max_rttime;
  return set_limits(rttime_request, max_rttime, err);
}

static bool promote_thread_to_real_time_internal(
    DBusConnection* conn, RtPriorityThreadInfoInternal thread_info,
    uint32_t audio_buffer_frames, uint32_t audio_samplerate_hz,
    RtPriorityHandleInternal* out_handle, DBusError* err) {
  out_handle->thread_info = thread_info;

  if (!set_real_time_hard_limit_internal(conn, audio_buffer_frames,
                                         audio_samplerate_hz, err)) {
    return false;
  }

  if (rtkit_set_realtime(conn, thread_info.thread_id, thread_info.pid, 10,
                         err)) {
    return true;
  }

  DBusError limits_err;
  dbus_error_init(&limits_err);
  int64_t max_prio = 0;
  uint64_t max_rttime = 0;
  struct rlimit limits;
  if (get_limits(conn, &max_prio, &max_rttime, &limits, &limits_err)) {
    if (limits.rlim_cur != RLIM_INFINITY) {
      setrlimit(RLIMIT_RTTIME, &limits);
    }
  }
  dbus_error_free(&limits_err);

  return false;
}

static bool promote_current_thread_to_real_time_internal(
    DBusConnection* conn, uint32_t audio_buffer_frames,
    uint32_t audio_samplerate_hz, RtPriorityHandleInternal* out_handle,
    DBusError* err) {
  RtPriorityThreadInfoInternal thread_info;
  if (!get_current_thread_info_internal(&thread_info, err)) {
    return false;
  }
  return promote_thread_to_real_time_internal(
      conn, thread_info, audio_buffer_frames, audio_samplerate_hz, out_handle,
      err);
}
#endif

void set_realtime_thread_priority(const char* name, size_t buffer_frames,
                                  size_t sample_rate) {
#ifdef CDSP_TEST
  (void)name;
  (void)buffer_frames;
  (void)sample_rate;
  return;
#else
  pthread_t thread = pthread_self();
  struct sched_param param;
  int policy;

  // 1. Try native POSIX scheduling first.
  if (pthread_getschedparam(thread, &policy, &param) == 0) {
    param.sched_priority = 10;  // Use default RT priority 10 (which fits under
                                // standard rtprio 95 limits)
    int res = pthread_setschedparam(thread, SCHED_FIFO, &param);
    if (res == 0) {
      logger_info(&g_logger,
                  "[%s] Thread promoted to Linux SCHED_FIFO real-time priority "
                  "via pthread_setschedparam",
                  name ? name : "unknown");
      return;
    }
  }

#if defined(CDSP_HAS_DBUS)
  // 2. Fall back to RealtimeKit (rtkit) via direct D-Bus client connection.
  DBusError dbus_err;
  dbus_error_init(&dbus_err);

  DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &dbus_err);
  if (dbus_error_is_set(&dbus_err)) {
    logger_warn(&g_logger, "[%s] Failed to connect to system D-Bus: %s",
                name ? name : "unknown", dbus_err.message);
    dbus_error_free(&dbus_err);
    goto fallback;
  }
  dbus_connection_set_exit_on_disconnect(conn, FALSE);

  RtPriorityHandleInternal handle;
  if (promote_current_thread_to_real_time_internal(
          conn, (uint32_t)buffer_frames, (uint32_t)sample_rate, &handle,
          &dbus_err)) {
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    logger_info(&g_logger,
                "[%s] Thread promoted to Linux real-time priority via direct "
                "D-Bus call to rtkit-daemon",
                name ? name : "unknown");
    return;
  }

  logger_warn(&g_logger, "[%s] rtkit-daemon MakeThreadRealtime failed: %s",
              name ? name : "unknown", dbus_err.message);
  dbus_error_free(&dbus_err);
  dbus_connection_close(conn);
  dbus_connection_unref(conn);
#endif

fallback:
  logger_warn(&g_logger,
              "[%s] Failed to promote thread to real-time priority (both "
              "pthread_setschedparam and RealtimeKit failed)",
              name ? name : "unknown");
#endif
}
#elif defined(_WIN32)
#include <windows.h>

typedef HANDLE(WINAPI* AvSetMmThreadCharacteristicsWFn)(LPCWSTR, LPDWORD);
typedef BOOL(WINAPI* AvRevertMmThreadCharacteristicsFn)(HANDLE);

static pthread_key_t g_win32_avrt_key;
static pthread_once_t g_win32_avrt_once = PTHREAD_ONCE_INIT;

static void win32_avrt_cleanup(void* val) {
  if (val) {
    HANDLE task_handle = (HANDLE)val;
    HMODULE avrt_module = LoadLibraryW(L"avrt.dll");
    if (avrt_module) {
      AvRevertMmThreadCharacteristicsFn revert_fn =
          (AvRevertMmThreadCharacteristicsFn)GetProcAddress(
              avrt_module, "AvRevertMmThreadCharacteristics");
      if (revert_fn) {
        revert_fn(task_handle);
      }
      FreeLibrary(avrt_module);
    }
  }
}

static void win32_avrt_init_key(void) {
  pthread_key_create(&g_win32_avrt_key, win32_avrt_cleanup);
}

void set_realtime_thread_priority(const char* name, size_t buffer_frames,
                                  size_t sample_rate) {
#ifdef CDSP_TEST
  (void)name;
  (void)buffer_frames;
  (void)sample_rate;
  return;
#else
  (void)buffer_frames;
  (void)sample_rate;

  HMODULE avrt_module = LoadLibraryW(L"avrt.dll");
  if (avrt_module) {
    AvSetMmThreadCharacteristicsWFn set_fn =
        (AvSetMmThreadCharacteristicsWFn)GetProcAddress(
            avrt_module, "AvSetMmThreadCharacteristicsW");
    if (set_fn) {
      DWORD task_index = 0;
      HANDLE task_handle = set_fn(L"Audio", &task_index);
      if (task_handle) {
        logger_info(
            &g_logger,
            "[%s] Thread promoted to Windows MMCSS (Audio task, index=%lu)",
            name ? name : "unknown", task_index);

        pthread_once(&g_win32_avrt_once, win32_avrt_init_key);
        void* old_val = pthread_getspecific(g_win32_avrt_key);
        if (old_val) {
          win32_avrt_cleanup(old_val);
        }
        pthread_setspecific(g_win32_avrt_key, (void*)task_handle);

        FreeLibrary(avrt_module);
        return;
      }
    }
    FreeLibrary(avrt_module);
  }

  // Fallback to standard SetThreadPriority if MMCSS failed
  HANDLE thread = GetCurrentThread();
  BOOL success = SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
  if (success) {
    logger_warn(&g_logger,
                "[%s] MMCSS failed; thread promoted to fallback Windows "
                "THREAD_PRIORITY_TIME_CRITICAL",
                name ? name : "unknown");
  } else {
    logger_warn(&g_logger,
                "[%s] Failed to set thread priority on Windows: err=%lu",
                name ? name : "unknown", GetLastError());
  }
#endif
}
#else
void set_realtime_thread_priority(const char* name, size_t buffer_frames,
                                  size_t sample_rate) {
  (void)name;
  (void)buffer_frames;
  (void)sample_rate;
}
#endif
