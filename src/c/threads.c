/*
 * Copyright 2017 Charles University in Prague
 * Copyright 2017 Vojtech Horky
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _BSD_SOURCE // For glibc 2.18 to include caddr_t
#define _DEFAULT_SOURCE // For glibc 2.20 to include caddr_t
#define _POSIX_C_SOURCE 200809L

#include "ubench.h"

#pragma warning(push, 0)
/* Ensure compatibility of JNI function types. */
#include "cz_cuni_mff_d3s_perf_NativeThreads.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#pragma warning(pop)

#ifdef __GNUC__
#pragma warning(push, 0)
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
#pragma warning(pop)
#endif

#ifdef __APPLE__
#pragma warning(push, 0)
#include <pthread.h>
#pragma warning(pop)
#endif


#ifdef _MSC_VER
#pragma warning(push, 0)
#include <Windows.h>
#pragma warning(pop)
#endif

typedef struct {
	native_tid_t native_id;
	java_tid_t java_id;
} thread_mapping_t;

static ubench_spinlock_t thread_mapping_guard = UBENCH_SPINLOCK_INITIALIZER;
static thread_mapping_t* thread_mappings = NULL;
static int thread_mapping_count = 0;

static ubench_spinlock_t java_lang_Thread_guard = UBENCH_SPINLOCK_INITIALIZER;
static jclass class_java_lang_Thread = NULL;
static jmethodID method_java_lang_Thread_getId = NULL;

static void
ensure_java_lang_Thread_resolved(JNIEnv* jni_env) {
	ubench_spinlock_lock(&java_lang_Thread_guard);

	if (class_java_lang_Thread != NULL) {
		ubench_spinlock_unlock(&java_lang_Thread_guard);
		return;
	}

	class_java_lang_Thread = (*jni_env)->FindClass(jni_env, "java/lang/Thread");
	if (class_java_lang_Thread == NULL) {
		fprintf(stderr, "Failed to find java.lang.Thread, aborting!\n");
		exit(1);
	}
	method_java_lang_Thread_getId = (*jni_env)->GetMethodID(jni_env, class_java_lang_Thread, "getId", "()J");
	if (method_java_lang_Thread_getId == NULL) {
		fprintf(stderr, "Failed to find java.lang.Thread.getId(), aborting!\n");
		exit(1);
	}

	ubench_spinlock_unlock(&java_lang_Thread_guard);
}

INTERNAL void JNICALL
ubench_jvm_callback_on_thread_start(
	jvmtiEnv* UNUSED_PARAMETER(jvmti), JNIEnv* jni, jthread thread
) {
	ensure_java_lang_Thread_resolved(jni);

	java_tid_t java_id = (*jni)->CallLongMethod(jni, thread, method_java_lang_Thread_getId);
	native_tid_t native_id = ubench_get_current_thread_native_id();

	DEBUG_PRINTF("JVM callback: thread %" PRId_JAVA_TID " [%" PRId_NATIVE_TID "] started.", java_id, native_id);

	ubench_register_thread_id_mapping(java_id, native_id);
}

INTERNAL void JNICALL
ubench_jvm_callback_on_thread_end(
	jvmtiEnv* UNUSED_PARAMETER(jvmti), JNIEnv* UNUSED_PARAMETER(jni),
	jthread UNUSED_PARAMETER(thread)
) {
	native_tid_t native_id = ubench_get_current_thread_native_id();

	DEBUG_PRINTF("JVM callback: thread [%" PRId_NATIVE_TID "] ended.", native_id);

	ubench_unregister_thread_id_mapping_by_native_id(native_id);
}

INTERNAL int
ubench_register_thread_id_mapping(java_tid_t java_thread_id, native_tid_t native_thread_id) {
	int res = 0;

	ubench_spinlock_lock(&thread_mapping_guard);

	for (int i = 0; i < thread_mapping_count; i++) {
		if (thread_mappings[i].java_id == java_thread_id) {
			res = 1;
			goto leave;
		}
		if (thread_mappings[i].native_id == UBENCH_THREAD_ID_INVALID) {
			thread_mappings[i].java_id = java_thread_id;
			thread_mappings[i].native_id = native_thread_id;
			res = 0;
			goto leave;
		}
	}
	thread_mapping_t* new_mapping = realloc(thread_mappings, sizeof(thread_mapping_t) * (thread_mapping_count + 1));
	if (new_mapping == NULL) {
		res = 2;
		goto leave;
	}


	thread_mappings = new_mapping;
	thread_mappings[thread_mapping_count].java_id = java_thread_id;
	thread_mappings[thread_mapping_count].native_id = native_thread_id;
	thread_mapping_count++;

leave:
	ubench_spinlock_unlock(&thread_mapping_guard);

	return res;
}

INTERNAL int
ubench_unregister_thread_id_mapping_by_native_id(native_tid_t native_thread_id) {
	int res = 1;

	ubench_spinlock_lock(&thread_mapping_guard);

	for (int i = 0; i < thread_mapping_count; i++) {
		if (thread_mappings[i].native_id == native_thread_id) {
			thread_mappings[i].native_id = UBENCH_THREAD_ID_INVALID;
			res = 0;
			break;
		}
	}

	ubench_spinlock_unlock(&thread_mapping_guard);

	return res;
}

INTERNAL native_tid_t
ubench_get_native_thread_id(java_tid_t java_thread_id) {
	ubench_spinlock_lock(&thread_mapping_guard);

	for (int i = 0; i < thread_mapping_count; i++) {
		if (thread_mappings[i].java_id == java_thread_id) {
			native_tid_t res = thread_mappings[i].native_id;

			ubench_spinlock_unlock(&thread_mapping_guard);

			return res;
		}
	}

	ubench_spinlock_unlock(&thread_mapping_guard);

	return UBENCH_THREAD_ID_INVALID;
}

INTERNAL native_tid_t
ubench_get_current_thread_native_id(void) {
#if defined(_MSC_VER)
	return (native_tid_t) GetCurrentThreadId();
#elif defined(__APPLE__)
	pthread_t tid = pthread_self();
	return (native_tid_t) tid;
#elif defined(__GNUC__)
	pid_t answer = syscall(__NR_gettid);
	return (native_tid_t) answer;
#else
#error "Threading not supported on this platform."
	return UBENCH_THREAD_ID_INVALID;
#endif
}

//

JNIEXPORT java_tid_t JNICALL
Java_cz_cuni_mff_d3s_perf_NativeThreads_getNativeId(
	JNIEnv* UNUSED_PARAMETER(jni), jclass UNUSED_PARAMETER(threads_class),
	java_tid_t java_thread_id
) {
	native_tid_t answer = ubench_get_native_thread_id(java_thread_id);
	if (answer == UBENCH_THREAD_ID_INVALID) {
		// TODO: throw an error
		return (java_tid_t) cz_cuni_mff_d3s_perf_NativeThreads_INVALID_THREAD_ID;
	}

	return (java_tid_t) answer;
}

JNIEXPORT jboolean JNICALL
Java_cz_cuni_mff_d3s_perf_NativeThreads_registerJavaThread(
	JNIEnv* UNUSED_PARAMETER(jni), jclass UNUSED_PARAMETER(threads_class),
	java_tid_t java_thread_id, java_tid_t jnative_thread_id
) {
	int res = ubench_register_thread_id_mapping(java_thread_id, (native_tid_t) jnative_thread_id);
	return res == 0;
}
