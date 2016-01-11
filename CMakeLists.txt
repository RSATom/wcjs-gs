cmake_minimum_required( VERSION 2.8 )

project( wcjs-gs )

add_definitions( -std=c++11 )

find_package( PkgConfig REQUIRED )
pkg_search_module( GSTREAMER REQUIRED gstreamer-1.0 )
pkg_search_module( GSTREAMER_APP REQUIRED gstreamer-app-1.0 )
pkg_search_module( GSTREAMER_VIDEO_META REQUIRED gstreamer-video-1.0 )

file( GLOB SOURCE_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    src/[^.]*.cpp
    src/[^.]*.h
    README.md
    package.json
    )

include_directories( ${CMAKE_JS_INC} deps )
add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/deps/nah )

add_library( ${PROJECT_NAME} SHARED ${SOURCE_FILES} )
set_target_properties( ${PROJECT_NAME} PROPERTIES PREFIX "" SUFFIX ".node" )
target_include_directories( ${PROJECT_NAME} PUBLIC ${GSTREAMER_INCLUDE_DIRS} )
target_link_libraries( ${PROJECT_NAME} ${CMAKE_JS_LIB} nah ${GSTREAMER_LIBRARIES} ${GSTREAMER_APP_LIBRARIES} ${GSTREAMER_VIDEO_META_LIBRARIES} )

#get_cmake_property( _variableNames VARIABLES )
#foreach( _variableName ${_variableNames} )
#    message( STATUS "${_variableName}=${${_variableName}}" )
#endforeach()
