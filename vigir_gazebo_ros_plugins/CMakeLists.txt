cmake_minimum_required(VERSION 2.8.3)
project(vigir_gazebo_ros_plugins)

## Find catkin macros and libraries
## if COMPONENTS list like find_package(catkin REQUIRED COMPONENTS xyz)
## is used, also find other catkin packages
find_package(catkin REQUIRED COMPONENTS
  atlas_msgs
  gazebo_msgs
  gazebo_plugins
  gazebo_ros
  geometry_msgs
  roscpp
  sensor_msgs
  std_msgs
)

catkin_package(
  INCLUDE_DIRS include
#  LIBRARIES drcsim_gazebo_ros_plugins
  CATKIN_DEPENDS gazebo_plugins
#  DEPENDS system_lib
)

find_package(gazebo REQUIRED)

###########
## Build ##
###########

## Specify additional locations of header files
## Your package locations should be listed before other locations
# include_directories(include)
include_directories(
  ${PROJECT_SOURCE_DIR}/include
  ${catkin_INCLUDE_DIRS}
  ${GAZEBO_INCLUDE_DIRS}
)

link_directories(
  ${GAZEBO_LIBRARY_DIRS}
)

add_library(VigirRobotiqHandPlugin src/VigirRobotiqHandPlugin.cpp)
set_target_properties(VigirRobotiqHandPlugin PROPERTIES LINK_FLAGS "${ld_flags}")
set_target_properties(VigirRobotiqHandPlugin PROPERTIES COMPILE_FLAGS "${cxx_flags}")
target_link_libraries(VigirRobotiqHandPlugin ${catkin_LIBRARIES})
add_dependencies(VigirRobotiqHandPlugin handle_msgs_gencpp atlas_msgs_gencpp)

add_library(VigirVRCPlugin src/VigirVRCPlugin.cpp)
set_target_properties(VigirVRCPlugin PROPERTIES LINK_FLAGS "${ld_flags}")
set_target_properties(VigirVRCPlugin PROPERTIES COMPILE_FLAGS "${cxx_flags}")
target_link_libraries(VigirVRCPlugin ${catkin_LIBRARIES})
add_dependencies(VigirVRCPlugin handle_msgs_gencpp atlas_msgs_gencpp)

install(TARGETS
  VigirRobotiqHandPlugin
  VigirVRCPlugin
  DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}/${PROJECT_NAME}/plugins/
)

install(DIRECTORY include/${PROJECT_NAME}/
  DESTINATION ${CATKIN_PACKAGE_INCLUDE_DESTINATION}
  FILES_MATCHING PATTERN "*.h"
  PATTERN ".svn" EXCLUDE
)

