SRC_DIR := src
OUTPUT_DIR := bin

JAVA_HOME := $(HOME)/.sdkman/candidates/java/current

# TODO may be parameterize on MAKE_HOST variable (linux or darwin)
.INCLUDE_DIRS += $(JAVA_HOME)/include $(JAVA_HOME)/include/linux
INCLUDES := $(foreach include_dir,$(.INCLUDE_DIRS),-I $(include_dir))

CFLAGS = -Wall -std=gnu11
LDFLAGS := -shared -fPIC

AGENT_NAME := agent
AGENT_LIB := $(AGENT_NAME).so

COMPILE.c = $(CC) $(INCLUDES) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c

.PHONY: all
all: $(OUTPUT_DIR)/$(AGENT_LIB)

# TODO collect all object files
$(OUTPUT_DIR)/$(AGENT_LIB): $(OUTPUT_DIR)/$(AGENT_NAME).o $(OUTPUT_DIR)/hashmap.o $(OUTPUT_DIR)/classload.o
	$(LINK.o) -o $@ $^ 

define compile-obj
	$(COMPILE.c) $(OUTPUT_OPTION) $?
endef

vpath %.c $(SRC_DIR)

.INTERMEDIATE: $(OUTPUT_DIR)/$(AGENT_NAME).o
$(OUTPUT_DIR)/$(AGENT_NAME).o: $(AGENT_NAME).c
	$(compile-obj)

.INTERMEDIATE: $(OUTPUT_DIR)/hashmap.o
$(OUTPUT_DIR)/hashmap.o: hashmap.c
	$(compile-obj)

.INTERMEDIATE: $(OUTPUT_DIR)/classload.o
$(OUTPUT_DIR)/classload.o: classload.c
	$(compile-obj)

.PHONY: clean
clean:
	rm -f $(OUTPUT_DIR)/*.so $(OUTPUT_DIR)/*.o
