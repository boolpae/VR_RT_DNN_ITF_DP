PRJ_HOME	:= $(shell echo $(PROJECT_ROOT) | sed 's/\ /\\ /g')
KERNEL_VERSION	:= $(shell uname -r | awk -F. '{print $1}')
CUDA_VERSION := $(shell nvcc --version | grep release | sed 's/.*release //' | sed 's/\,.*//')
-include $(PRJ_HOME)/Makefile
PWD	:= $(shell pwd | sed 's/\ /\\ /g')
ifeq ($(BUILD), )
BUILD	:= $(PWD:$(shell dirname $(PWD))/%=%)
endif

###############################################################################
#ifeq ($(CUDA_VERSION), 8.0)
#CUDA_PATH		:= /usr/local/cuda-8.0
#else ifeq ($(CUDA_VERSION), 6.0)
#CUDA_PATH		:= /usr/local/cuda-6.0
#endif
CUDA_PATH		:= /usr/local/cuda-$(CUDA_VERSION)

SOURCE			:= vr_server.cc vr.cc rt.cc restapi.cc
SOURCE			+= v1/restapi_v1.cc v1/servers.cc v1/waves.cc
INCLUDE_PATH	:= $(PRJ_HOME)/include/dnn $(PRJ_HOME)/include/chilkat
LIBRARIES		:= ${DIST}/itf_worker ${DIST}/itf_common
LIBRARIES		+= dnn/libsplproc dnn/libfrontend dnn/libmsearch dnn/libasearch
LIBRARIES		+= dnn/libbase dnn/liblsearch dnn/liblaserdnn2 dnn/libdnnapi dnn/libdnn.gpu chilkat/libchilkat-9.5.0
#LIBRARIES		+= dnn/libsplproc
#LIBRARIES		+= lstm/libfrontend lstm/libmsearch lstm/libasearch lstm/libbase lstm/liblaserdlnet lstm/libesdlapi lstm/libesdl.gpu lstm/libesdl4userdef
#LIBRARIES		+= chilkat/libchilkat-9.5.0
FLAGS			:= -pthread -D_REENTRANT
FLAGS			+= -DUSE_REALTIME_POOL
SHARED_LIBS		:= -lboost_program_options -lboost_filesystem -lboost_system
SHARED_LIBS		+= -llog4cpp -lgearman -lmicrohttpd -lcurl
SHARED_LIBS		+= -fopenmp -lrt -lm 
#SHARED_LIBS		+= -L/usr/lib64/atlas -llapack -lcblas -latlas -lf77blas
#SHARED_LIBS		+= -L/usr/lib64/atlas -lsatlas -ltatlas
ifeq ($(KERNEL_VERSION), 2)
SHARED_LIBS		+= -L/usr/lib64/atlas -llapack -lcblas -latlas -lf77blas
else
SHARED_LIBS		+= -L/usr/lib64/atlas -lsatlas -ltatlas
endif
#SHARED_LIBS		+= -L/usr/local/cuda/lib64 -lcudart -lcublas -lcuda
#SHARED_LIBS		+= -L/usr/local/cuda-6.0/lib64 -lcudart -lcublas -lcuda
#SHARED_LIBS		+= /usr/local/cuda-6.0/lib64/libcudart.so /usr/local/cuda-6.0/lib64/libcublas.so
SHARED_LIBS		+= -L$(CUDA_PATH)/lib64 -lcudart -lcublas -L/usr/lib64 -lcuda
###############################################################################

ifeq ($(MAKECMDGOALS), $(BUILD)_all)
-include $(DEPEND_FILE)
endif

OBJ_DIR		:= $(shell echo $(OBJS_PATH)/$(BUILD) | sed 's/\ /\\ /g')
LIB_DIR		:= $(shell echo $(LIBS_PATH) | sed 's/\ /\\ /g')
BUILD_DIR	:= $(shell echo $(BINS_PATH) | sed 's/\ /\\ /g')

$(BUILD)_OBJS	:= $(SOURCE:%.cc=$(OBJ_DIR)/%.o)
$(BUILD)_LIBS	:= $(LIBRARIES:%=$(LIB_DIR)/%.a)
BUILD_NAME		:= $(BUILD_DIR)/$(PROJECT_NAME)_$(BUILD)

$(BUILD)_all: $($(BUILD)_OBJS)
	$(CPP) -o "$(BUILD_NAME)" $($(BUILD)_OBJS) $($(BUILD)_LIBS) $(SHARED_LIBS)

.SECONDEXPANSION:
$(OBJ_DIR)/%.o: %.cc
	@`[ -d "$(OBJ_DIR)" ] || $(MKDIR) "$(OBJ_DIR)"`
	@`[ -d "$(OBJ_DIR)/$(shell dirname $<)" ] || $(MKDIR) "$(OBJ_DIR)/$(shell dirname $<)"`
	$(CPP) $(CFLAGS) $(FLAGS) $(INCLUDE) $(INCLUDE_PATH:%=-I"%") -c $< -o "$@"

$(BUILD)_depend:
	@$(ECHO) "# $(OBJ_DIR)" > $(DEPEND_FILE)
	@for FILE in $(SOURCE:%.cc=%); do \
		$(CPP) -MM -MT "$(OBJ_DIR)/$$FILE.o" $$FILE.c $(CFLAGS) $(FLAGS) $(INCLUDE) >> $(DEPEND_FILE); \
	done

$(BUILD)_clean:
	$(RM) -rf "$(OBJ_DIR)"
	$(RM) -f "$(BUILD_NAME)"

$(BUILD)_mrproper:
	@$(RM) -f $(DEPEND_FILE)
