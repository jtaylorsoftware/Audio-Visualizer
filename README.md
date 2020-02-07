Audio visualizer using PulseAudio C API and OpenGL.

Currently only records and displays audio data from the default device.


To build:
Download and install <a href="https://github.com/g-truc/glm">glm</a>, <a href="https://www.boost.org/">Boost</a>, <a href="https://www.glfw.org/">Glfw3</a>, and generate an OpenGL 3.3 core profile using <a href="https://glad.dav1d.de/">glad</a> (place this folder in the root of the project). From there, you should be able to build with CMake from within the project's root directory using the given CMakeLists file.
