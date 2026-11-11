add_library(gs_support STATIC
    include/gs/support/Crypto.h
    include/gs/support/Expected.h
    include/gs/support/Files.h
    include/gs/support/InplaceVector.h
    include/gs/support/Log.h
    include/gs/support/Thread.h
    include/gs/support/UdpSocket.h
    include/gs/support/UsbWatcher.h
    src/Crypto.cpp
    src/Files.cpp
    src/Log.cpp
    src/Thread.cpp
    src/UdpSocket.cpp
    src/UsbWatcher.cpp
)
add_library(gs::support ALIAS gs_support)

target_include_directories(gs_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gs_support
    PUBLIC gs::tl_expected
    PRIVATE bcrypt cfgmgr32 ws2_32)
gs_apply_compiler_policy(gs_support)
