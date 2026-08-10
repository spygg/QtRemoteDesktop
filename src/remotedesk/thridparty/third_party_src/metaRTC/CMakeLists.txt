cmake_minimum_required(VERSION 3.5)
project(metaRTC C CXX)

# metaRTC 各静态库：libyangutil8 / libmetartccore8 / libmetartc8 / libyangwhip8
add_subdirectory(libyangutil8)
add_subdirectory(libmetartccore8)
add_subdirectory(libmetartc8)
add_subdirectory(libyangwhip8)
