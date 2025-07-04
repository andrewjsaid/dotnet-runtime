project(Runtime)

include(FindPython)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(EVENT_MANIFEST ${VM_DIR}/ClrEtwAll.man)
set(EVENT_EXCLUSIONS ${VM_DIR}/ClrEtwAllMeta.lst)
set(EVENT_INCLUSION_FILE ${CMAKE_CURRENT_SOURCE_DIR}/gen-eventing-event-inc.lst)
set(GENERATED_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/inc)
set(NONEXTERN_ARG "--nonextern")
set(NOXPLATHEADER_ARG "--noxplatheader")

# Platform agnostic source generation, clretwallmain.h/cpp, disabledclretwallmain.cpp, clreventpipewriteevents.h, etmdummy.h
set (GEN_EVENTPIPE_PLAT_AGNOSTIC_SOURCES ${GENERATED_INCLUDE_DIR}/clretwallmain.cpp)

set (GEN_EVENTPIPE_PLAT_AGNOSTIC_DISABLED_SOURCES ${GENERATED_INCLUDE_DIR}/disabledclretwallmain.cpp)

set(GENERATE_PLAT_AGNOSTIC_SCRIPT ${CLR_DIR}/scripts/genEventing.py)

set (EventingHeaders
  ${GENERATED_INCLUDE_DIR}/etmdummy.h
  ${GENERATED_INCLUDE_DIR}/clretwallmain.h
  ${GENERATED_INCLUDE_DIR}/clreventpipewriteevents.h
  ${GENERATED_INCLUDE_DIR}/clrproviders.h
  ${GENERATED_INCLUDE_DIR}/disabledclretwallmain.cpp
  ${GENERATED_INCLUDE_DIR}/clretwallmain.cpp
)

if(CMAKE_CROSSCOMPILING)
  set(GEN_EVENTING_TARGETOS --targetos)
  set(GEN_EVENTING_TARGETOS_ARG ${CLR_CMAKE_TARGET_OS})
endif()

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/aot_eventing_headers.timestamp
  COMMAND ${Python_EXECUTABLE} ${GENERATE_PLAT_AGNOSTIC_SCRIPT} --man ${EVENT_MANIFEST} --incdir ${GENERATED_INCLUDE_DIR} --inc ${EVENT_INCLUSION_FILE} ${GEN_EVENTING_TARGETOS} ${GEN_EVENTING_TARGETOS_ARG} --dummy ${GENERATED_INCLUDE_DIR}/etmdummy.h  --runtimeflavor nativeaot ${NONEXTERN_ARG} ${NOXPLATHEADER_ARG}
  COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/aot_eventing_headers.timestamp
  DEPENDS ${EVENT_MANIFEST} ${GENERATE_PLAT_AGNOSTIC_SCRIPT}
  VERBATIM
)

set_source_files_properties(${EventingHeaders} PROPERTIES GENERATED TRUE)

add_custom_target(aot_eventing_headers DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/aot_eventing_headers.timestamp)

if(CLR_CMAKE_TARGET_WIN32)

  set(ETW_PROVIDER_SCRIPT ${CLR_DIR}/scripts/genEtwProvider.py)

  set (ETW_PROVIDER_OUTPUTS
    ${GENERATED_INCLUDE_DIR}/etw/ClrEtwAll.h
    ${GENERATED_INCLUDE_DIR}/etw/ClrEtwAll.rc
    ${GENERATED_INCLUDE_DIR}/etw/etwmacros.h
    ${GENERATED_INCLUDE_DIR}/etw/ClrEtwAll_MSG00001.bin
    ${GENERATED_INCLUDE_DIR}/etw/ClrEtwAllTEMP.bin
    ${GENERATED_INCLUDE_DIR}/clrxplatevents.h
  )

  set_source_files_properties(${ETW_PROVIDER_OUTPUTS} PROPERTIES GENERATED TRUE)

  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/eventprovider.timestamp
    COMMAND ${Python_EXECUTABLE} ${ETW_PROVIDER_SCRIPT} --man ${EVENT_MANIFEST} --exc ${EVENT_EXCLUSIONS} --intermediate ${GENERATED_INCLUDE_DIR}
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/eventprovider.timestamp
    DEPENDS ${EVENT_MANIFEST} ${EVENT_EXCLUSIONS} ${ETW_PROVIDER_SCRIPT}
  )

  add_custom_target(aot_etw_headers DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/eventprovider.timestamp)

endif()

# EventPipe provider source, currently dotnetruntime provider and helper file
set(GENERATE_EVENTPIPE_SCRIPT ${CLR_DIR}/scripts/genEventPipe.py)
set(GENERATE_COMMAND ${Python_EXECUTABLE} ${GENERATE_EVENTPIPE_SCRIPT} --man ${EVENT_MANIFEST} --exc ${EVENT_EXCLUSIONS} --inc ${EVENT_INCLUSION_FILE} --intermediate ${CMAKE_CURRENT_BINARY_DIR} --runtimeflavor nativeaot ${NONEXTERN_ARG})

execute_process(
  COMMAND ${GENERATE_COMMAND} --dry-run
  RESULT_VARIABLE GEN_EVENTPIPE_RESULT
  OUTPUT_VARIABLE GEN_EVENTPIPE_SOURCE_PATHS
  ERROR_VARIABLE GEN_EVENTPIPE_ERRORS
)

if (NOT GEN_EVENTPIPE_RESULT EQUAL 0)
  message(FATAL_ERROR "Failed to generate EventPipe: ${GEN_EVENTPIPE_ERRORS}")
endif()

string(REPLACE "\n" ";" GEN_EVENTPIPE_SOURCE_PATHS ${GEN_EVENTPIPE_SOURCE_PATHS}) # turn the outputted list of files into a CMake list

set (GEN_EVENTPIPE_PROVIDER_SOURCES "")
foreach(GEN_EVENTPIPE_SOURCE_PATH ${GEN_EVENTPIPE_SOURCE_PATHS})
  file(TO_CMAKE_PATH ${GEN_EVENTPIPE_SOURCE_PATH} GEN_EVENTPIPE_SOURCE)
  list(APPEND GEN_EVENTPIPE_PROVIDER_SOURCES ${GEN_EVENTPIPE_SOURCE})
endforeach()

# Generate the provider source files via python auto-gen
add_custom_command(OUTPUT ${GEN_EVENTPIPE_PROVIDER_SOURCES}
  COMMAND ${GENERATE_COMMAND}
  DEPENDS ${GENERATE_EVENTPIPE_SCRIPT} ${EVENT_MANIFEST} ${EVENT_EXCLUSIONS})

set_source_files_properties(${GEN_EVENTPIPE_PROVIDER_SOURCES} PROPERTIES GENERATED TRUE)

set(EP_GENERATED_HEADER_PATH "${GENERATED_INCLUDE_DIR}")
include (${CLR_SRC_NATIVE_DIR}/eventpipe/configure.cmake)
include_directories(${EP_GENERATED_HEADER_PATH})

if (CLR_CMAKE_TARGET_WIN32)
  set(EP_ETW_GENERATED_HEADER_PATH "${GENERATED_INCLUDE_DIR}/etw")
  include_directories(${EP_ETW_GENERATED_HEADER_PATH})
endif()

set(AOT_EVENTPIPE_SHIM_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set (EVENTPIPE_SOURCES "")

list(APPEND AOT_EVENTPIPE_SHIM_SOURCES
  ${AOT_EVENTPIPE_SHIM_DIR}/ep-rt-aot.cpp
  ${AOT_EVENTPIPE_SHIM_DIR}/ds-rt-aot.cpp
)

list(APPEND AOT_EVENTPIPE_SHIM_HEADERS
  ${AOT_EVENTPIPE_SHIM_DIR}/ds-rt-aot.h
  ${AOT_EVENTPIPE_SHIM_DIR}/ds-rt-types-aot.h
  ${AOT_EVENTPIPE_SHIM_DIR}/ep-rt-aot.h
  ${AOT_EVENTPIPE_SHIM_DIR}/ep-rt-config-aot.h
  ${AOT_EVENTPIPE_SHIM_DIR}/ep-rt-types-aot.h
)


list(APPEND AOT_EVENTPIPE_MANAGED_TO_NATIVE_SOURCES
   ${NATIVEAOT_RUNTIME_DIR}/eventpipeinternal.cpp
   ${NATIVEAOT_RUNTIME_DIR}/EnabledEventPipeInterface.cpp
   ${NATIVEAOT_RUNTIME_DIR}/runtimeeventinternal.cpp
)

if (FEATURE_EVENT_TRACE)
  list(APPEND AOT_EVENTTRACE_SOURCES
    ${NATIVEAOT_RUNTIME_DIR}/eventtrace.cpp
    ${NATIVEAOT_RUNTIME_DIR}/eventtrace_bulktype.cpp
    ${NATIVEAOT_RUNTIME_DIR}/eventtrace_gcheap.cpp
    ${NATIVEAOT_RUNTIME_DIR}/profheapwalkhelper.cpp
  )

  if(CLR_CMAKE_TARGET_WIN32)
    set_source_files_properties(${GEN_EVENTPIPE_PROVIDER_SOURCES} PROPERTIES COMPILE_FLAGS "/FI\"${NATIVEAOT_RUNTIME_DIR}/eventpipe/NativeaotEventPipeSupport.h\"")
    set_source_files_properties(${GEN_EVENTPIPE_PLAT_AGNOSTIC_SOURCES} PROPERTIES COMPILE_FLAGS "/FI\"${NATIVEAOT_RUNTIME_DIR}/eventpipe/NativeaotEventPipeSupport.h\"")
    set_source_files_properties(${AOT_EVENTTRACE_SOURCES} PROPERTIES COMPILE_FLAGS "/FI\"${NATIVEAOT_RUNTIME_DIR}/eventpipe/NativeaotEventPipeSupport.h\"")
  endif()
endif()

# The NativeAOT EventPipe runtime is C++, but the EventPipe and DiagnosticServer sources are C.
# Forcibly override them to build as C++.
get_property(diagnosticserver_sources TARGET dn-diagnosticserver PROPERTY INTERFACE_SOURCES)
get_property(diagnosticserver_pal_sources TARGET dn-diagnosticserver-pal PROPERTY INTERFACE_SOURCES)
get_property(eventpipe_sources TARGET dn-eventpipe PROPERTY INTERFACE_SOURCES)
set_source_files_properties(${diagnosticserver_sources} ${diagnosticserver_pal_sources} ${eventpipe_sources} PROPERTIES LANGUAGE CXX)

add_library(eventpipe-shared-objects OBJECT)
target_link_libraries(eventpipe-shared-objects PRIVATE dn-diagnosticserver dn-eventpipe dn-diagnosticserver-pal)
# Build EventPipe and DiagnosticServer as unity-builds for better inlining.
set_target_properties(eventpipe-shared-objects PROPERTIES
  UNITY_BUILD ON
  UNITY_BUILD_BATCH_SIZE 0
  COMPILE_PDB_NAME "eventpipe-shared-objects"
  COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
)
if (CLR_CMAKE_TARGET_WIN32)
    target_compile_options(eventpipe-shared-objects PRIVATE
        "/FI${NATIVEAOT_RUNTIME_DIR}/eventpipe/NativeaotEventPipeSupport.h")

    # Install the compile PDB for the eventpipe unity builds.
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/eventpipe-shared-objects.pdb" DESTINATION aotsdk COMPONENT nativeaot)
    # For the container library, we need to produce a compile PDB and install it
    set_target_properties(dn-containers-no-lto PROPERTIES
        COMPILE_PDB_NAME "dn-containers"
        COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/dn-containers.pdb" DESTINATION aotsdk COMPONENT nativeaot)
endif()

list(APPEND EVENTPIPE_SOURCES
  ${AOT_EVENTPIPE_SHIM_SOURCES}
  ${AOT_EVENTPIPE_MANAGED_TO_NATIVE_SOURCES}
  ${AOT_EVENTTRACE_SOURCES}
  ${GEN_EVENTPIPE_PROVIDER_SOURCES}
  ${GEN_EVENTPIPE_PLAT_AGNOSTIC_SOURCES}
)

list(APPEND AOT_EVENTPIPE_DISABLED_SOURCES
  ${NATIVEAOT_RUNTIME_DIR}/DisabledEventPipeInterface.cpp
  ${NATIVEAOT_RUNTIME_DIR}/disabledeventpipeinternal.cpp
  ${NATIVEAOT_RUNTIME_DIR}/disabledeventtrace.cpp
  ${NATIVEAOT_RUNTIME_DIR}/disabledruntimeeventinternal.cpp
  ${GEN_EVENTPIPE_PLAT_AGNOSTIC_DISABLED_SOURCES}
)

add_library(eventpipe-enabled STATIC ${EVENTPIPE_SOURCES})

target_link_libraries(eventpipe-enabled PRIVATE eventpipe-shared-objects dn-containers-no-lto)
add_dependencies(eventpipe-enabled aot_eventing_headers)
if(CLR_CMAKE_TARGET_WIN32)
  add_dependencies(eventpipe-enabled aot_etw_headers)
endif()

add_library(eventpipe-disabled STATIC ${AOT_EVENTPIPE_DISABLED_SOURCES})
add_dependencies(eventpipe-disabled aot_eventing_headers)

install_static_library(eventpipe-enabled aotsdk nativeaot)
install_static_library(eventpipe-disabled aotsdk nativeaot)
