cmake_minimum_required(VERSION 3.21)
project(G2HMatterCamera LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
foreach(v MATTER_ROOT SDK_SOURCE SDK_OUTPUT RTC_OUTPUT CRYPTO_PREFIX MEDIA_PREFIX)
 if(NOT DEFINED ${v})
  message(FATAL_ERROR "${v} must be set (use tools/build.py)")
 endif()
endforeach()
set(APP "${CMAKE_CURRENT_LIST_DIR}/../app/src")
add_library(g2h-app-objects OBJECT "${APP}/g2h_camera_main.cpp" "${APP}/g2h_matter_signaling.cpp")
target_compile_options(g2h-app-objects PRIVATE -fno-rtti)
target_compile_definitions(g2h-app-objects PRIVATE RTC_ENABLE_MEDIA=1 RTC_ENABLE_WEBSOCKET=0 RTC_STATIC G2H_WITH_MATTER_CORE CHIP_HAVE_CONFIG_H=1)
target_include_directories(g2h-app-objects PRIVATE
 "${MATTER_ROOT}/src" "${MATTER_ROOT}/src/include"
 "${SDK_OUTPUT}/gen/include" "${SDK_OUTPUT}/gen/model/zapgen" "${SDK_SOURCE}/include"
 "${MATTER_ROOT}/config/standalone" "${MATTER_ROOT}/zzz_generated/app-common"
 "${MATTER_ROOT}/third_party/nlassert/repo/include" "${MATTER_ROOT}/third_party/nlio/repo/include"
 "${MATTER_ROOT}/third_party/mbedtls/repo/include" "${MATTER_ROOT}/third_party/inipp/repo/inipp"
 "${MATTER_ROOT}/third_party/libdatachannel/repo/include"
 "${MATTER_ROOT}/examples/platform/linux"
 "${MATTER_ROOT}/examples/all-clusters-app/all-clusters-common/include"
 "${CRYPTO_PREFIX}/include" "${MEDIA_PREFIX}/include")
add_executable(g2h-matter-camera $<TARGET_OBJECTS:g2h-app-objects>)
target_link_options(g2h-matter-camera PRIVATE -static -Wl,--gc-sections -Wl,-z,stack-size=262144)
target_link_libraries(g2h-matter-camera PRIVATE -Wl,--start-group
 "${SDK_OUTPUT}/lib/G2HCameraSDK.a" "${SDK_OUTPUT}/lib/libCHIP.a"
 "${RTC_OUTPUT}/libdatachannel.a"
 "${RTC_OUTPUT}/deps/usrsctp/usrsctplib/libusrsctp.a"
 "${RTC_OUTPUT}/deps/libsrtp/libsrtp2.a" "${RTC_OUTPUT}/deps/libjuice/libjuice.a"
 "${CRYPTO_PREFIX}/lib/libssl.a" "${CRYPTO_PREFIX}/lib/libcrypto.a"
 "${MEDIA_PREFIX}/lib/libavcodec.a" "${MEDIA_PREFIX}/lib/libswscale.a"
 "${MEDIA_PREFIX}/lib/libswresample.a" "${MEDIA_PREFIX}/lib/libopus.a"
 "${MEDIA_PREFIX}/lib/libavutil.a" -Wl,--end-group pthread m)
if(EXISTS "${APP}/g2h_media_broker.cpp")
 add_executable(g2h-media-broker "${APP}/g2h_media_broker.cpp")
 target_link_libraries(g2h-media-broker PRIVATE pthread)
 target_link_options(g2h-media-broker PRIVATE -static -Wl,--gc-sections -Wl,-z,stack-size=262144)
endif()
