#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>

#include "jvmti.h"

#include <pthread.h>
#include <sys/stat.h>

#include "hashmap.h"

typedef struct {
	JavaVM* jvm;
	jvmtiEnv* jvmti;
	FILE* log_file;
	HashMap* classes;
} AgentData;

static atomic_uintptr_t agent_data_ref = ATOMIC_VAR_INIT(0);

static void log_debug(const char* format, ...) {
	va_list args;
	va_start(args, format);

	AgentData* agent_data = (AgentData*)atomic_load(&agent_data_ref);
	FILE* log_file = agent_data->log_file;

	vfprintf(log_file, format, args);
	va_end(args);

	fprintf(log_file, "\n");

	fflush(log_file);
}

static void redefine_class(FILE* class_file, jint class_bytes_count) {
	AgentData* agent_data = (AgentData*)atomic_load(&agent_data_ref);

	JNIEnv* jni = NULL;
	jint attach_thread_status = (*agent_data->jvm)->AttachCurrentThread(agent_data->jvm, (void**)&jni, NULL);
	if (attach_thread_status != JNI_OK) {
		log_debug("failed to attach 'redefine class' thread");
		return;
	}

	log_debug("reading class file bytes");

	unsigned char class_file_bytes[class_bytes_count];
	size_t blocks_read = fread(class_file_bytes, class_bytes_count, 1, class_file);
	if (blocks_read < 1) {
		log_debug("failed to read class file bytes");
	} else {
		jclass klass = hash_map_get(agent_data->classes, "LService;");
		jvmtiClassDefinition class_definitions[] = {
				{ klass, class_bytes_count, class_file_bytes }
		};

		log_debug("redefining class");
		jvmtiError error = (*agent_data->jvmti)->RedefineClasses(agent_data->jvmti, 1, class_definitions);
		if (error != JVMTI_ERROR_NONE) {
			log_debug("failed to redefine class - error code: %d", error);
		} else {
			log_debug("class redefined");
		}
	}

	(*agent_data->jvm)->DetachCurrentThread(agent_data->jvm);
}

static void* redefine_class_activity(void* arg) {
	AgentData* agent_data = (AgentData*)atomic_load(&agent_data_ref);

	log_debug("'redefine class' thread is running");

	int i = 0;
	for (;;) {
		log_debug("%d: 'redefine class' thread running", ++i);

		sleep(1);

		// TODO check class modification time
		if (i == 15) {
			const char* class_file_path = "bin/Service.class";

			struct stat class_file_stat;
			if (stat(class_file_path, &class_file_stat) == -1) {
				log_debug("failed to find class file");
				break;
			}

			log_debug("class file size: %lld", class_file_stat.st_size);

			FILE* class_file = fopen(class_file_path, "rb");
			if (class_file == NULL) {
				log_debug("failed to open class file");
				break;
			}

			redefine_class(class_file, class_file_stat.st_size);

			fclose(class_file);
		}
	}

	log_debug("'redefine class' thread exiting");

	return NULL;
}

static void JNICALL ClassPreparedHandler(jvmtiEnv* jvmti, JNIEnv* jni, jthread thread, jclass klass) {
	char* class_signature;
	jvmtiError error = (*jvmti)->GetClassSignature(jvmti, klass, &class_signature, NULL);
	if (error != JVMTI_ERROR_NONE) {
		log_debug("failed to get class signature");
	} else {
		log_debug("class loaded: %s", class_signature);

		AgentData* agent_data = (AgentData*)atomic_load(&agent_data_ref);

		jclass class_ref = (*jni)->NewGlobalRef(jni, klass);
		hash_map_put(agent_data->classes, class_signature, class_ref);

		(*jvmti)->Deallocate(jvmti, (unsigned char*)class_signature);
	}
}

static void JNICALL VMInitEventHandler(jvmtiEnv* jvmti, JNIEnv* jni, jthread thread) {
	pthread_t redefine_class_thread;
    int thread_create_status = pthread_create(&redefine_class_thread, NULL, redefine_class_activity, NULL);
    if (thread_create_status != 0) {
    	log_debug("failed to start 'redefine class' service thread");
    	return;
    }

    log_debug("'redefine class' service thread started");

	log_debug("VM initialization completed");
}

static void JNICALL VMDeathEventHandler(jvmtiEnv* jvmti, JNIEnv* jni) {
	log_debug("VM is dead");
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* jvm, char* options, void* reserved) {
    FILE* log_file = fopen("agent.log", "w");
    if (log_file == NULL) {
    	fprintf(stderr, "failed to open log file\n");
    	return JNI_ERR;
    }

    fprintf(log_file, "loading agent\n");

    jvmtiEnv* jvmti = NULL;
    if ((*jvm)->GetEnv(jvm, (void**)&jvmti, JVMTI_VERSION_1_0) != JNI_OK) {
        return JNI_ERR;
    }

    static AgentData agent_data;
    agent_data.jvm = jvm;
    agent_data.jvmti = jvmti;
    agent_data.log_file = log_file;

	// TODO check new hash map allocation success
	agent_data.classes = hash_map_new(16, NULL);

    atomic_store(&agent_data_ref, (uintptr_t)&agent_data);

    log_debug("got JVMTI environment");

    log_debug("configuring capabilities");

    static jvmtiCapabilities capabilities;
    memset(&capabilities, 0, sizeof(jvmtiCapabilities));

    capabilities.can_redefine_classes = JNI_TRUE;

    jvmtiError error = (*jvmti)->AddCapabilities(jvmti, &capabilities);
    if (error != JVMTI_ERROR_NONE) {
    	log_debug("failed to configure capabilities - error: %d", error);
    	return JNI_ERR;
    }

    log_debug("capabilities configured");

    log_debug("configuring event handlers");

    error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE, JVMTI_EVENT_CLASS_PREPARE, NULL);
    if (error != JVMTI_ERROR_NONE) {
    	log_debug("failed to enable 'CLASS_PREPARE' event notification");
    	return JNI_ERR;
    }

    error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL);
    if (error != JVMTI_ERROR_NONE) {
    	log_debug("failed to enable 'VM_INIT' event notification");
    	return JNI_ERR;
    }

	error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE, JVMTI_EVENT_VM_DEATH, NULL);
	if (error != JVMTI_ERROR_NONE) {
		log_debug("failed to enable 'VM_DEATH' event notification");
		return JNI_ERR;
	}

    jvmtiEventCallbacks eventCallbacks;
    memset(&eventCallbacks, 0, sizeof(jvmtiEventCallbacks));

    eventCallbacks.ClassPrepare = ClassPreparedHandler;
    eventCallbacks.VMInit = VMInitEventHandler;
	eventCallbacks.VMDeath = VMDeathEventHandler;

    error = (*jvmti)->SetEventCallbacks(jvmti, &eventCallbacks, sizeof(eventCallbacks));
    if (error != JVMTI_ERROR_NONE) {
    	log_debug("failed to configure event handlers");
    	return JNI_ERR;
    }

    log_debug("event handlers configured");

    log_debug("agent loaded");

    return JNI_OK;
}

// TODO store reference to jclass ( only one class can be reloaded right now )
// TODO scan commands directory for new version of the recompiled class

JNIEXPORT void JNICALL Agent_OnUnload(JavaVM* jvm) {
	AgentData* agent_data = (AgentData*)atomic_load(&agent_data_ref);

	hash_map_free(agent_data->classes);

	log_debug("unloading agent");

	fclose(agent_data->log_file);
}
