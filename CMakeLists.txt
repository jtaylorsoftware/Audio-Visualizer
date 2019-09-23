cmake_minimum_required(VERSION 3.1)

project(hellopulse)

find_path(  PulseAudio_INCLUDE_DIRS
            NAMES pulse/pulseaudio.h
            DOC "The PulseAudio include directory")

if( PulseAudio_INCLUDE_DIRS STREQUAL "" )
    message("ERROR NO PULSEAUDIO INCLUDES FOUND")
    return()
endif()

if( PulseAudio_LIBRARIES STREQUAL "" )
    message("ERROR NO PULSEAUDIO LIBRARY FOUND")
    return()
endif()

find_package(glfw3 REQUIRED)

find_package(Boost REQUIRED COMPONENTS thread)

set(Glad_INCLUDE_DIRS "glad/include")
set(Glad_SRC "glad/src/glad.c")

set(glm_INCLUDE_DIRS "glm/")

set(PROJECT_SRC "main.cpp" ${Glad_SRC})

add_executable(hellopulse ${PROJECT_SRC})

set_target_properties(  hellopulse PROPERTIES
                        CXX_STANDARD 11
                        CXX_STANDARD_REQUIRED YES)

target_include_directories( hellopulse   
                            PUBLIC
                            ${PulseAudio_INCLUDE_DIRS}
                            ${glfw3_INCLUDE_DIRS}
                            ${Glad_INCLUDE_DIRS}
                            ${glm_INCLUDE_DIRS}
                            ${Boost_INCLUDE_DIRS})

target_link_libraries(  hellopulse
                        dl 
                        glfw
                        pulse
                        pulse-simple
                        ${Boost_LIBRARIES})