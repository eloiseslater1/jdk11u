/*
 * Copyright (c) 2011, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "com_sun_management_internal_OperatingSystemImpl.h"

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#ifndef __NetBSD__
  #include <sys/user.h>
#endif
#include <unistd.h>
#ifdef __FreeBSD__
  #include <stdlib.h>
  #include <malloc_np.h>
#endif

#include "jvm.h"

struct ticks {
    jlong used;
    jlong total;
};

typedef struct ticks ticks;

static struct perfbuf {
    int   nProcs;
    ticks jvmTicks;
    ticks cpuTicks;
    ticks *cpus;
} counters;

/**
 * This method must be called first, before any data can be gathererd.
 */
int perfInit() {
    static int initialized = 0;

    if (!initialized) {
        int mib[2];
        size_t len;
        int cpu_val;

        mib[0] = CTL_HW;
        mib[1] = HW_NCPU;
        len = sizeof(cpu_val);
        if (sysctl(mib, 2, &cpu_val, &len, NULL, 0) == -1 || cpu_val < 1) {
            cpu_val = 1;
        }

        counters.nProcs = cpu_val;
#ifdef __FreeBSD__
        // Initialise JVM
        counters.jvmTicks.used  = 0;
        counters.jvmTicks.total = 0;

        // Initialise CPU
        counters.cpuTicks.used  = 0;
        counters.cpuTicks.total = 0;

        counters.cpus = calloc(cpu_val, sizeof(ticks));
        if (counters.cpus != NULL) {
            // Initialise per CPU
            for (int i = 0; i < counters.nProcs; i++) {
                counters.cpus[i].used = 0;
                counters.cpus[i].total = 0;
            }
        }
#endif
        initialized = 1;
    }

    return initialized ? 0 : -1;
}

JNIEXPORT jdouble JNICALL
Java_com_sun_management_internal_OperatingSystemImpl_getSystemCpuLoad0
(JNIEnv *env, jobject dummy)
{
#ifdef __FreeBSD__
    if (perfInit() == 0) {
        /* This is based on the MacOS X implementation */

        /* Load CPU times */
        long cp_time[CPUSTATES];
        size_t len = sizeof(cp_time);
        if (sysctlbyname("kern.cp_time", &cp_time, &len, NULL, 0) == -1) {
            return -1.;
        }

        jlong used  = cp_time[CP_USER] + cp_time[CP_NICE] + cp_time[CP_SYS] + cp_time[CP_INTR];
        jlong total = used + cp_time[CP_IDLE];

        if (counters.cpuTicks.used == 0 || counters.cpuTicks.total == 0) {
            // First call, just set the last values
            counters.cpuTicks.used = used;
            counters.cpuTicks.total = total;
            // return 0 since we have no data, not -1 which indicates error
            return 0.;
        }

        jlong used_delta  = used - counters.cpuTicks.used;
        jlong total_delta = total - counters.cpuTicks.total;

        jdouble cpu = (jdouble) used_delta / total_delta;

        counters.cpuTicks.used = used;
        counters.cpuTicks.total = total;

        return cpu;
    }
    else {
#endif
    // Not implemented yet
    return -1.;
#ifdef __FreeBSD__
    }
#endif
}


#define TIME_VALUE_TO_TIMEVAL(a, r) do {  \
     (r)->tv_sec = (a)->seconds;          \
     (r)->tv_usec = (a)->microseconds;    \
} while (0)


#define TIME_VALUE_TO_MICROSECONDS(TV) \
     ((TV).tv_sec * 1000 * 1000 + (TV).tv_usec)


JNIEXPORT jdouble JNICALL
Java_com_sun_management_internal_OperatingSystemImpl_getProcessCpuLoad0
(JNIEnv *env, jobject dummy)
{
#ifdef __FreeBSD__
    if (perfInit() == 0) {
        /* This is based on the MacOS X implementation */

        struct timeval now;
        struct kinfo_proc kp;
        int mib[4];
        size_t len = sizeof(struct kinfo_proc);

        mib[0] = CTL_KERN;
        mib[1] = KERN_PROC;
        mib[2] = KERN_PROC_PID;
        mib[3] = getpid();

        if (sysctl(mib, 4, &kp, &len, NULL, 0) == -1) {
            return -1.;
        }

        if (gettimeofday(&now, NULL) == -1) {
            return -1.;
        }

        jint ncpus      = JVM_ActiveProcessorCount();
        jlong time      = TIME_VALUE_TO_MICROSECONDS(now) * ncpus;
        jlong task_time = TIME_VALUE_TO_MICROSECONDS(kp.ki_rusage.ru_utime) +
                          TIME_VALUE_TO_MICROSECONDS(kp.ki_rusage.ru_stime);

        if (counters.jvmTicks.used == 0 || counters.jvmTicks.total == 0) {
            // First call, just set the last values.
            counters.jvmTicks.used  = task_time;
            counters.jvmTicks.total = time;
            // return 0 since we have no data, not -1 which indicates error
            return 0.;
        }

        jlong task_time_delta = task_time - counters.jvmTicks.used;
        jlong time_delta      = time - counters.jvmTicks.total;
        if (time_delta == 0) {
            return -1.;
        }

        jdouble cpu = (jdouble) task_time_delta / time_delta;

        counters.jvmTicks.used  = task_time;
        counters.jvmTicks.total = time;

        return cpu;
    }
    else {
#endif
    // Not implemented yet
    return -1.0;
#ifdef __FreeBSD__
    }
#endif
}

JNIEXPORT jdouble JNICALL
Java_com_sun_management_internal_OperatingSystemImpl_getSingleCpuLoad0
(JNIEnv *env, jobject dummy, jint cpu_number)
{
#ifdef __FreeBSD__
    if (perfInit() == 0 && 0 <= cpu_number && cpu_number < counters.nProcs) {
        /* Load CPU times */
        long cp_times[CPUSTATES * counters.nProcs];
        size_t len = sizeof(cp_times);
        if (sysctlbyname("kern.cp_times", &cp_times, &len, NULL, 0) == -1) {
            return -1.;
        }

        size_t offset = cpu_number * CPUSTATES;
        jlong used  = cp_times[offset + CP_USER] + cp_times[offset + CP_NICE] + cp_times[offset + CP_SYS] + cp_times[offset + CP_INTR];
        jlong total = used + cp_times[offset + CP_IDLE];

        if (counters.cpus[cpu_number].used == 0 || counters.cpus[cpu_number].total == 0) {
            // First call, just set the last values
            counters.cpus[cpu_number].used  = used;
            counters.cpus[cpu_number].total = total;
            // return 0 since we have no data, not -1 which indicates error
            return 0.;
        }

        jlong used_delta  = used - counters.cpus[cpu_number].used;
        jlong total_delta = total - counters.cpus[cpu_number].total;

        jdouble cpu = (jdouble) used_delta / total_delta;

        counters.cpus[cpu_number].used  = used;
        counters.cpus[cpu_number].total = total;

        return cpu;
    }
    else {
#endif
    return -1.0;
#ifdef __FreeBSD__
    }
#endif
}

JNIEXPORT jint JNICALL
Java_com_sun_management_internal_OperatingSystemImpl_getHostConfiguredCpuCount0
(JNIEnv *env, jobject mbean)
{
    if (perfInit() == 0) {
        return counters.nProcs;
    }
    else {
        return -1;
    }
}
