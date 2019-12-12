cmake_minimum_required(VERSION 3.4.1)

# Standard Android dependencies
find_library(android-lib android)
find_library(EGL-lib EGL)
find_library(GLESv2-lib GLESv2)
find_library(log-lib log)

#set(libs_dir ${CMAKE_CURRENT_SOURCE_DIR}/libraries)
set(cardboard_libs_dir ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/)


#C add
set(RX_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${RX_CORE_DIR}/libs/glm)
#GLPrograms
include_directories(${RX_CORE_DIR}/src/main/cpp/GLPrograms)
add_library( GLPrograms SHARED
        ${RX_CORE_DIR}/src/main/cpp/GLPrograms/GLProgramVC.cpp
        ${RX_CORE_DIR}/src/main/cpp/GLPrograms/GLProgramText.cpp
        ${RX_CORE_DIR}/src/main/cpp/GLPrograms/GLProgramTexture.cpp
        ${RX_CORE_DIR}/src/main/cpp/GLPrograms/GLProgramLine.cpp
        #${RX_CORE_DIR}/src/main/cpp/Sphere/Sphere.cpp
        #${RX_CORE_DIR}/src/main/cpp/Sphere/GvrSphere.cpp
        #${RX_CORE_DIR}/src/main/cpp/GLPrograms/GLProgramSpherical.cpp
        ${RX_CORE_DIR}/src/main/cpp/GLPrograms/DistortionCorrection/DistortionManager.cpp
        ${RX_CORE_DIR}/src/main/cpp/GLPrograms/DistortionCorrection/MLensDistortion.cpp
        ${RX_CORE_DIR}/src/main/cpp/GLPrograms/DistortionCorrection/MPolynomialRadialDistortion.cpp
        )
target_link_libraries( GLPrograms ${log-lib} android GLESv2 ${cardboard_libs_dir}/my_cardboard_api_output_lib/${ANDROID_ABI}/libcardboard_api.so)
target_include_directories(GLPrograms PRIVATE ${cardboard_libs_dir}/include/)
#C add end

# === Cardboard Sample ===
# Sources
file(GLOB native_srcs "src/main/jni/*.cc")
# Output binary
add_library(cardboard_jni SHARED ${native_srcs})
# Includes
target_include_directories(cardboard_jni PRIVATE ${cardboard_libs_dir}/include/)
# Build
target_link_libraries(cardboard_jni
    ${android-lib}
    ${EGL-lib}
    ${GLESv2-lib}
    ${log-lib}
    ${cardboard_libs_dir}/my_cardboard_api_output_lib/${ANDROID_ABI}/libcardboard_api.so
    GLPrograms)
