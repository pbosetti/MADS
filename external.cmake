include(ExternalProject)
set(semicolon_smuggle "-+-")
string(REPLACE ";" "${semicolon_smuggle}" CMAKE_OSX_ARCHITECTURES "${CMAKE_OSX_ARCHITECTURES}")

ExternalProject_Add(libzmq
  PREFIX ${EXTERNAL_DIR}
  SOURCE_DIR ${EXTERNAL_DIR}/libzmq
  INSTALL_DIR ${USR_DIR}
  GIT_REPOSITORY https://github.com/zeromq/libzmq.git
  GIT_TAG 90b4f410a07222fa2e9a5f53b454a09d4533e45a
  GIT_SHALLOW TRUE
  LIST_SEPARATOR ${semicolon_smuggle}
  CMAKE_ARGS
    -DCMAKE_OSX_ARCHITECTURES:STRING=${CMAKE_OSX_ARCHITECTURES}
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTS=OFF
    -DWITH_DOCS=OFF
    -DWITH_TLS=OFF
    -DZMQ_BUILD_TESTS=OFF
    -DBUILD_SHARED=OFF
    -DBUILD_STATIC=ON
    -DCMAKE_INSTALL_PREFIX:PATH=${USR_DIR}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

ExternalProject_Add(zmqpp
  PREFIX ${EXTERNAL_DIR}
  SOURCE_DIR ${EXTERNAL_DIR}/zmqpp
  INSTALL_DIR ${USR_DIR}
  GIT_REPOSITORY https://github.com/zeromq/zmqpp.git
  GIT_TAG ba4230d5d03d29ced9ca788e3bd1095477db08ae
  GIT_SHALLOW TRUE
  LIST_SEPARATOR ${semicolon_smuggle}
  CMAKE_ARGS
    -DCMAKE_OSX_ARCHITECTURES:STRING=${CMAKE_OSX_ARCHITECTURES}
    -DCMAKE_BUILD_TYPE=Release
    -DIS_TRAVIS_CI_BUILD=OFF
    -DZMQPP_BUILD_CLIENT=OFF
    -DZMQPP_BUILD_EXAMPLES=OFF
    -DZEROMQ_INCLUDE_DIR=${USR_DIR}/include
    -DZMQPP_BUILD_SHARED=OFF
    -DZMQPP_BUILD_STATIC=ON
    -DZMQPP_LIBZMQ_CMAKE=ON
    -DCMAKE_INSTALL_PREFIX:PATH=${USR_DIR}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)


ExternalProject_Add(snappy
  PREFIX ${EXTERNAL_DIR}
  SOURCE_DIR ${EXTERNAL_DIR}/snappy
  INSTALL_DIR ${USR_DIR}
  GIT_REPOSITORY https://github.com/google/snappy.git
  GIT_TAG 32ded457c0b1fe78ceb8397632c416568d6714a0
  GIT_SHALLOW TRUE
  LIST_SEPARATOR ${semicolon_smuggle}
  CMAKE_ARGS
    -DCMAKE_OSX_ARCHITECTURES:STRING=${CMAKE_OSX_ARCHITECTURES}
    -DCMAKE_BUILD_TYPE=Release
    -DSNAPPY_BUILD_BENCHMARKS=OFF
    -DSNAPPY_BUILD_TESTS=OFF
    -DCMAKE_INSTALL_PREFIX:PATH=${USR_DIR}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
    
add_dependencies(zmqpp libzmq)


if(${MADS_ENABLE_LOGGER})
  ExternalProject_Add(mongocxx
    PREFIX ${EXTERNAL_DIR}
    SOURCE_DIR ${EXTERNAL_DIR}/mongocxx
    INSTALL_DIR ${USR_DIR}
    GIT_REPOSITORY https://github.com/mongodb/mongo-cxx-driver.git
    GIT_TAG r3.9.0
    GIT_SHALLOW TRUE
    LIST_SEPARATOR ${semicolon_smuggle}
    CMAKE_ARGS 
      -DCMAKE_OSX_ARCHITECTURES:STRING=${CMAKE_OSX_ARCHITECTURES}
      -DCMAKE_BUILD_TYPE=Release
      -DENABLE_SNAPPY=OFF
      -DCMAKE_CXX_STANDARD=17
      -DENABLE_STATIC=ON
      -DENABLE_SHARED=ON
      -DBUILD_SHARED_AND_STATIC_LIBS=ON
      -DENABLE_TESTS=OFF
      -DENABLE_ZSTD=OFF
      -DENABLE_SSL=OFF
      -DMONGOCXX_ENABLE_SSL=$<IF:$<BOOL:${WIN32}>,OFF,AUTO> 
      -DENABLE_EXAMPLES=OFF
      -DCMAKE_INSTALL_PREFIX:PATH=${USR_DIR}
      -DENABLE_MONGODB_AWS_AUTH=${MADS_ENABLE_MONGODB_AWS}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  add_dependencies(mongocxx snappy)
endif()
