cmake_minimum_required(VERSION 2.8)
#project name
PROJECT(worker)

MESSAGE(STATUS "Project:WORKER")

SET(CMAKE_C_FLAGS_DEBUG "-g -Wall")

AUX_SOURCE_DIRECTORY(. DIR_SRCS)
AUX_SOURCE_DIRECTORY(./utils UTILS)
AUX_SOURCE_DIRECTORY(./worker WORKER)

SET(INCLUDES ${DIR_SRCS} ./utils ./worker)
INCLUDE_DIRECTORIES(
    ${INCLUDES}
        )

    #ADD_SUBDIRECTORY(worker)

# package
#FIND_PACKAGE(Threads)

SET(CMAKE_EXE_LINKER_FLAGS "-lpthread")
#SET(EXECUTABLE_OUTPUT_PATH ../bin)

# target 
ADD_EXECUTABLE(./bin/worker ${DIR_SRCS} ${UTILS} ${WORKER})
#ADD_EXECUTABLE(../bin/worker ${DIR_SRCS} ${UTILS})
#TARGET_LINK_LIBRARIES(../bin/worker ${CMAKE_THREAD_LIBS_INIT})
#TARGET_LINK_LIBRARIES(../bin/worker)
#SET_TARGET_PROPERTIES(../bin/worker)
