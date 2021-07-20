cmake_minimum_required(VERSION 3.2.0)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -Wno-unknown-pragmas -O3 -Wno-unused-function -Wno-unused-variable")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")

project(8BitVita)

add_executable(${PROJECT_NAME}
  src/bluetooth.c
)

set_target_properties(${PROJECT_NAME}
	PROPERTIES LINK_FLAGS "-nostdlib"
	COMPILE_FLAGS "-D__VITA_KERNEL__"
)

target_link_libraries(${PROJECT_NAME}
  gcc
  taihenForKernel_stub
  SceSysclibForDriver_stub
  SceSysmemForDriver_stub
  SceSysmemForKernel_stub
  SceThreadmgrForDriver_stub
  SceIofilemgrForDriver_stub
  taihenModuleUtils_stub
  SceBtForDriver_stub
  SceDebugForDriver_stub
  SceModulemgrForDriver_stub 
  SceKernelSuspendForDriver_stub
  SceModulemgrForKernel_stub 
  SceCtrlForDriver_stub
  SceSblAIMgrForDriver_stub
  SceSysrootForDriver_stub 
  SceProcessmgrForDriver_stub
)

vita_create_stubs(stubs ${PROJECT_NAME} ${PROJECT_NAME}.yml KERNEL)

add_executable(8BitVitaCompanion
  src/debug.c
)

target_link_libraries(8BitVitaCompanion
  SceCtrl_stub
  ${CMAKE_SOURCE_DIR}/build/stubs/lib8BitVita_stub_weak.a
  vita2d
  SceShellSvc_stub
  SceDisplay_stub
  SceGxm_stub
  ScePgf_stub
  ScePvf_stub
  SceSysmodule_stub
  SceVshBridge_stub
  SceAppMgr_stub 
  m
  SceCommonDialog_stub
)

vita_create_self(8BitVitaCompanion.self 8BitVitaCompanion UNSAFE)
vita_create_self(${PROJECT_NAME}.skprx ${PROJECT_NAME} CONFIG ${PROJECT_NAME}.yml UNSAFE)

vita_create_vpk("8BitVitaCompanion.vpk" "DEBG00002" "8BitVitaCompanion.self"
    VERSION "01.00"
    NAME "8BitVita Companion"
)