APP_STL := c++_shared
APP_OPTIM := release
APP_ABI := armeabi-v7a,arm64-v8a,x86,x86_64
APP_PLATFORM := android-21
NDK_TOOLCHAIN_VERSION := clang
NDK_APP_LIBS_OUT=../jniLibs
APP_CPPFLAGS += -std=c++11
APP_SHORT_COMMANDS := true
