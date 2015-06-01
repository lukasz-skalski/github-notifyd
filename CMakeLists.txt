cmake_minimum_required(VERSION 2.6)

project(github-notifyd)

enable_language(C)

set(CMAKE_C_FLAGS_DEBUG "-Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -DDEBUG -g -O0 -pipe")
set(CMAKE_C_FLAGS_RELEASE "-Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -O2 -pipe -fomit-frame-pointer ${ACCESS_TOKEN}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(NOTIFY REQUIRED libnotify)
pkg_check_modules(JSON REQUIRED jansson)
pkg_check_modules(GLIB2 REQUIRED glib-2.0)

add_definitions(${CURL_CFLAGS} ${NOTIFY_CFLAGS} ${JSON_CFLAGS} ${GLIB2_CFLAGS} ${ACCESS_TOKEN})

set(SRCS github-notifyd.c)

add_executable(${PROJECT_NAME} ${SRCS})
target_link_libraries(${PROJECT_NAME} ${CURL_LDFLAGS} ${NOTIFY_LDFLAGS} ${JSON_LDFLAGS} ${GLIB2_LDFLAGS} ${ACCESS_TOKEN})

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
