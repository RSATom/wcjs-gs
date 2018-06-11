cmake_minimum_required( VERSION 2.8 )

project(wcjs-gs)

add_definitions(-std=c++14)

find_package(PkgConfig REQUIRED)
pkg_search_module(GSTREAMER REQUIRED gstreamer-1.0)
pkg_search_module(GSTREAMER_APP REQUIRED gstreamer-app-1.0)
pkg_search_module(GSTREAMER_VIDEO_META REQUIRED gstreamer-video-1.0)

file(GLOB SOURCE_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    src/[^.]*.cpp
    src/[^.]*.h
    *.html
    *.js
    README.md
    package.json
    )

add_library( ${PROJECT_NAME} SHARED ${SOURCE_FILES} )
set_target_properties( ${PROJECT_NAME} PROPERTIES PREFIX "" SUFFIX ".node")
target_include_directories(${PROJECT_NAME} PUBLIC ${GSTREAMER_INCLUDE_DIRS})
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_JS_INC}")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/node_modules")
	target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/node-addon-api")
	target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/node-addon-api-helpers")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../node_modules")
	target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../node-addon-api")
	target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../node-addon-api-helpers")
else()
	message(FATAL_ERROR "Can't locate node_modules")
endif()
target_link_libraries(${PROJECT_NAME} ${CMAKE_JS_LIB})
target_link_libraries(${PROJECT_NAME} ${GSTREAMER_LIBRARIES} ${GSTREAMER_APP_LIBRARIES} ${GSTREAMER_VIDEO_META_LIBRARIES})

#get_cmake_property( _variableNames VARIABLES )
#foreach( _variableName ${_variableNames} )
#    message( STATUS "${_variableName}=${${_variableName}}" )
#endforeach()
