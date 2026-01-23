#   _____         _                                                   
#  |_   _|__  ___| |_   _ __  _ __ ___   __ _ _ __ __ _ _ __ ___  ___ 
#    | |/ _ \/ __| __| | '_ \| '__/ _ \ / _` | '__/ _` | '_ ` _ \/ __|
#    | |  __/\__ \ |_  | |_) | | | (_) | (_| | | | (_| | | | | | \__ \
#    |_|\___||___/\__| | .__/|_|  \___/ \__, |_|  \__,_|_| |_| |_|___/
#                      |_|              |___/                         
# This subproject builds test programs, not intended for installation.

include_directories(${CMAKE_SOURCE_DIR}/src)

# CURVE authentication test programs
add_executable(curve_server server.cpp)
target_link_libraries(curve_server PRIVATE ${LIB_LIST})

add_executable(curve_client client.cpp)
target_link_libraries(curve_client PRIVATE ${LIB_LIST})

add_executable(c_test c_test.c)
target_link_libraries(c_test PRIVATE mads-lib)