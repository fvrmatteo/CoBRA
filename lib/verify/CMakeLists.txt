# Exactly one backend is compiled: both sources implement the same
# Z3Verify/Z3VerifyExprs entry points from cobra/verify/Z3Verifier.h.
if(COBRA_VERIFY_BACKEND STREQUAL "Bitwuzla")
    add_library(cobra-verify STATIC BitwuzlaVerify.cpp)
    set(_cobra_verify_solver PkgConfig::BITWUZLA)
else()
    add_library(cobra-verify STATIC Z3Verifier.cpp)
    set(_cobra_verify_solver z3::libz3)
endif()

set_target_properties(cobra-verify PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(cobra-verify PUBLIC cobra-core ${_cobra_verify_solver})
target_include_directories(cobra-verify PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

unset(_cobra_verify_solver)
