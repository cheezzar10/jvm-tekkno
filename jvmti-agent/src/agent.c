#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>

#include "jvmti.h"

#include <pthread.h>
#include <sys/stat.h>
#include <sys/inotify.h>

#include "hashmap.h"
#include "classload.h"

const char* const DEFAULT_CLASSES_DIR = "bin";

typedef struct {
	JavaVM* jvm;
	jvmtiEnv* jvmti;
	FILE* log_file;
	HashMap* classes;
	int inotify_fd;
	char* classes_dir;
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

static void redefine_class(AgentData* agent_data, FILE* class_file, jint class_bytes_count) {
	JNIEnv* jni = NULL;
	jint attach_thread_status = (*agent_data->jvm)->AttachCurrentThread(agent_data->jvm, (void**)&jni, NULL);
	if (attach_thread_status != JNI_OK) {
		log_debug("failed to attach 'redefine class' thread");
		return;
	}

	log_debug("reading class file bytes");

	uint8_t class_file_bytes[class_bytes_count];
	size_t blocks_read = fread(class_file_bytes, class_bytes_count, 1, class_file);
	if (blocks_read < 1) {
		log_debug("failed to read class file bytes");
	} else {
		JClass* loaded_class = jclass_load(class_file_bytes);

		char class_signature[strnlen(loaded_class->name, PATH_MAX) + 3];
		// TODO use snprintf, check return value for buffer overrun
		sprintf(class_signature, "L%s;", loaded_class->name);

		jclass_free(loaded_class);

		jclass klass = hash_map_get(agent_data->classes, class_signature);
		jvmtiClassDefinition class_definitions[] = {
				{ klass, class_bytes_count, class_file_bytes }
		};

		log_debug("redefining class: %s", class_signature);
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
	log_debug("'redefine class' thread is running");

	const size_t event_buf_size = sizeof(struct inotify_event) + PATH_MAX + 1;
	char event_buf[event_buf_size];

	AgentData* agent_data = (AgentData*)atomic_load(&agent_data_ref);

	for (;;) {
		log_debug("watching classes directory: %s", agent_data->classes_dir);

		ssize_t event_size = read(agent_data->inotify_fd, event_buf, event_buf_size);
		if (event_size == 0 || event_size == -1) {
			log_debug("failed to read event - stopping 'redefine class' thread");
			break;
		}

		struct inotify_event* event = (struct inotify_event*)event_buf;

		size_t classes_dir_path_len = strnlen(agent_data->classes_dir, PATH_MAX);
		size_t file_name_len = strnlen(event->name, NAME_MAX);

		// allocating enougn bytes for class dir path length + '/' + class file name length + '\0'
		char class_file_path[classes_dir_path_len + 1 + file_name_len + 1];
		// TODO check return value
		sprintf(class_file_path, "%s/%s", agent_data->classes_dir, event->name);

		log_debug("class file %s changed", class_file_path);

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

		redefine_class(agent_data, class_file, class_file_stat.st_size);

		fclose(class_file);
	}

	log_debug("'redefine class' thread stopping...");

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

	// TODO detach 'redefine class' thread

	log_debug("VM initialization completed");
}

static void JNICALL VMDeathEventHandler(jvmtiEnv* jvmti, JNIEnv* jni) {
	log_debug("VM is dead");
}

// copying passed in string to dynamically allocated buffer
// client is responsible for memory reclaiming
static char* copy_string(const char* str, size_t max_length) {
	size_t copy_buf_size = strnlen(str, max_length) + 1;

	char* copy_buf = malloc(copy_buf_size);
	if (copy_buf == NULL) {
		return NULL;
	}

	strncpy(copy_buf, str, copy_buf_size);

	return copy_buf;
}

static char* get_agent_option_value(char* options, const char* name, const char* default_value) {
	if (options == NULL) {
		char* default_value_copy = copy_string(default_value, PATH_MAX);
		if (default_value_copy == NULL) {
			return NULL;
		}

		return default_value_copy;
	}

	char* name_value_sep_pos = strchr(options, '=');

	if (name_value_sep_pos == NULL) {
		char* default_value_copy = copy_string(default_value, PATH_MAX);
		if (default_value_copy == NULL) {
			return NULL;
		}

		return default_value_copy;
	} else {
		char* option_value_copy = copy_string(name_value_sep_pos + 1, PATH_MAX);
		if (option_value_copy == NULL) {
			return NULL;
		}

		return option_value_copy;
	}
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* jvm, char* options, void* reserved) {
    FILE* log_file = fopen("agent.log", "w");
    if (log_file == NULL) {
    	fprintf(stderr, "failed to open log file\n");
    	return JNI_ERR;
    }

    fprintf(log_file, "loading agent - options: '%s'\n", options);

	char* classes_dir = get_agent_option_value(options, "classes_dir", DEFAULT_CLASSES_DIR);
	fprintf(log_file, "classes dir: %s\n", classes_dir);

	int inotify_fd = inotify_init();
	if (inotify_fd == -1) {
		fprintf(log_file, "failed to open inotify descriptor\n");
		return JNI_ERR;
	}

    jvmtiEnv* jvmti = NULL;
    if ((*jvm)->GetEnv(jvm, (void**)&jvmti, JVMTI_VERSION_1_0) != JNI_OK) {
        return JNI_ERR;
    }

    static AgentData agent_data;
    agent_data.jvm = jvm;
    agent_data.jvmti = jvmti;
    agent_data.log_file = log_file;

	int add_watch_status = inotify_add_watch(inotify_fd, classes_dir, IN_CLOSE_WRITE);	
	if (add_watch_status == -1) {
		fprintf(log_file, "failed to add classes dir inotify watch\n");
		return JNI_ERR;
	}

	agent_data.inotify_fd = inotify_fd;
	agent_data.classes_dir = classes_dir;

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

	free(agent_data->classes_dir);

	log_debug("unloading agent");

	fclose(agent_data->log_file);
}
