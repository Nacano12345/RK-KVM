set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TC /home/zsy/rk3506-ipkvm/toolchain/armv7-eabihf--musl--stable-2024.05-1)
set(STAGE /home/zsy/rk3506-ipkvm/build/stage)
set(CMAKE_C_COMPILER   ${TC}/bin/arm-linux-gcc)
set(CMAKE_CXX_COMPILER ${TC}/bin/arm-linux-g++)
set(CMAKE_AR           ${TC}/bin/arm-linux-ar)
set(CMAKE_RANLIB       ${TC}/bin/arm-linux-ranlib)
set(CMAKE_STRIP        ${TC}/bin/arm-linux-strip)
set(CMAKE_FIND_ROOT_PATH ${TC}/arm-buildroot-linux-musleabihf/sysroot ${STAGE})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
