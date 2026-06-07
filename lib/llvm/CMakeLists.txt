if(NOT LLVM_FOUND)
    message(FATAL_ERROR "cobra-llvm requires LLVM — set COBRA_BUILD_LLVM_LIBRARY=OFF to skip")
endif()

set(cobra_llvm_sources
    CobraPass.cpp
    MBADetector.cpp
    IRReconstructor.cpp
)

set(cobra_llvm_compile_definitions)
set(cobra_llvm_compile_options)
separate_arguments(cobra_llvm_raw_llvm_definitions NATIVE_COMMAND "${LLVM_DEFINITIONS}")
foreach(cobra_llvm_flag IN LISTS cobra_llvm_raw_llvm_definitions)
    if(cobra_llvm_flag MATCHES "^[-/]D(.+)$")
        list(APPEND cobra_llvm_compile_definitions "${CMAKE_MATCH_1}")
    elseif(NOT cobra_llvm_flag STREQUAL "")
        list(APPEND cobra_llvm_compile_options "${cobra_llvm_flag}")
    endif()
endforeach()

add_library(cobra-llvm-obj OBJECT ${cobra_llvm_sources})
set_target_properties(cobra-llvm-obj PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(cobra-llvm-obj PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(cobra_llvm_compile_definitions)
    target_compile_definitions(cobra-llvm-obj PRIVATE ${cobra_llvm_compile_definitions})
endif()

if(cobra_llvm_compile_options)
    target_compile_options(cobra-llvm-obj PRIVATE ${cobra_llvm_compile_options})
endif()

llvm_map_components_to_libnames(cobra_llvm_llvm_libs
    Analysis
    Core
    Passes
    Support
)

add_library(cobra-llvm STATIC $<TARGET_OBJECTS:cobra-llvm-obj>)
set_target_properties(cobra-llvm PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(cobra-llvm PUBLIC
    cobra-core
    ${cobra_llvm_llvm_libs}
)

target_include_directories(cobra-llvm PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${LLVM_INCLUDE_DIRS}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(cobra_llvm_compile_definitions)
    target_compile_definitions(cobra-llvm PUBLIC ${cobra_llvm_compile_definitions})
endif()

if(cobra_llvm_compile_options)
    target_compile_options(cobra-llvm PUBLIC ${cobra_llvm_compile_options})
endif()

if(COBRA_BUILD_LLVM_PASS)
    add_library(CobraPass MODULE
        CobraPassPlugin.cpp
        $<TARGET_OBJECTS:cobra-llvm-obj>
    )

    target_link_libraries(CobraPass PRIVATE cobra-core)

    target_include_directories(CobraPass PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${LLVM_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    if(cobra_llvm_compile_definitions)
        target_compile_definitions(CobraPass PRIVATE ${cobra_llvm_compile_definitions})
    endif()

    if(cobra_llvm_compile_options)
        target_compile_options(CobraPass PRIVATE ${cobra_llvm_compile_options})
    endif()

    # Don't link LLVM libraries — loaded into opt/clang which provides them.
    set_target_properties(CobraPass PROPERTIES
        PREFIX ""
        SUFFIX "${CMAKE_SHARED_MODULE_SUFFIX}"
    )

    if(APPLE)
        target_link_options(CobraPass PRIVATE -undefined dynamic_lookup)
    endif()

    if(Z3_FOUND)
        target_link_libraries(CobraPass PRIVATE cobra-verify)
        target_compile_definitions(CobraPass PRIVATE COBRA_HAS_Z3=1)
    endif()
endif()
