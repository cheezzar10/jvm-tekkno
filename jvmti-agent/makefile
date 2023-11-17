SRC_DIR := src
OUTPUT_DIR := bin

JAVA_HOME := $(HOME)/.sdkman/candidates/java/current

.INCLUDE_DIRS += $(JAVA_HOME)/include $(JAVA_HOME)/include/darwin
INCLUDES := $(foreach include_dir,$(.INCLUDE_DIRS),-I $(include_dir))

CFLAGS = -Wall -std=c11
LDFLAGS := -dynamiclib

AGENT_NAME := agent
AGENT_LIB := $(AGENT_NAME).dylib

COMPILE.c = $(CC) $(INCLUDES) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c

.PHONY: all
all: $(OUTPUT_DIR)/$(AGENT_LIB)

$(OUTPUT_DIR)/$(AGENT_LIB): $(OUTPUT_DIR)/$(AGENT_NAME).o $(OUTPUT_DIR)/hashmap.o
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

.PHONY: clean
clean:
	rm -f $(OUTPUT_DIR)/*.dylib $(OUTPUT_DIR)/*.o