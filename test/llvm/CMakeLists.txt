# LLVM pass plugin FileCheck tests.
# Each .ll file uses CHECK directives validated by FileCheck.

find_program(FILECHECK_BIN FileCheck
    HINTS ${LLVM_TOOLS_BINARY_DIR} ${LLVM_INSTALL_PREFIX}/bin)
find_program(OPT_BIN opt
    HINTS ${LLVM_TOOLS_BINARY_DIR} ${LLVM_INSTALL_PREFIX}/bin)

if(NOT FILECHECK_BIN OR NOT OPT_BIN)
    message(STATUS "opt or FileCheck not found — skipping LLVM pass tests")
    return()
endif()

message(STATUS "LLVM pass tests: opt=${OPT_BIN} FileCheck=${FILECHECK_BIN}")

# A test named `test_*_verified.ll` holds a comparison, which the pass only
# rewrites under a proof: it runs with `cobra-simplify<verified>` and needs the
# solver backend. One named `test_*_dying_cost.ll` is about what a rewrite
# removes, and runs with `cobra-simplify<dying-cost>`.
function(cobra_add_ll_test TEST_FILE)
    get_filename_component(TEST_NAME "${TEST_FILE}" NAME_WE)
    set(FULL_NAME "LLVMPass.${TEST_NAME}")
    set(FULL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${TEST_FILE}")
    set(PIPELINE "cobra-simplify")
    if(TEST_NAME MATCHES "_verified$")
        if(NOT COBRA_VERIFY_BACKEND)
            return()
        endif()
        set(PIPELINE "cobra-simplify<verified>")
    elseif(TEST_NAME MATCHES "_dying_cost$")
        set(PIPELINE "cobra-simplify<dying-cost>")
    endif()

    add_test(
        NAME "${FULL_NAME}"
        COMMAND sh -c "${OPT_BIN} -load-pass-plugin=$<TARGET_FILE:CobraPass> -passes='${PIPELINE}' -S '${FULL_PATH}' | ${FILECHECK_BIN} '${FULL_PATH}'"
    )
    set_tests_properties("${FULL_NAME}" PROPERTIES LABELS "llvm")
endfunction()

file(GLOB LL_TEST_FILES "*.ll")
foreach(LL_FILE ${LL_TEST_FILES})
    get_filename_component(LL_NAME "${LL_FILE}" NAME)
    cobra_add_ll_test("${LL_NAME}")
endforeach()

# The demanded bits the pass answers from a value's readers have to be the ones
# LLVM's whole-function analysis gives. Every file here is run with each
# question answered both ways (`COBRA_DEMANDED_CHECK`), and a disagreement - or
# the pass failing - fails the test.
function(cobra_add_demanded_check_test TEST_FILE)
    get_filename_component(TEST_NAME "${TEST_FILE}" NAME_WE)
    set(FULL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${TEST_FILE}")
    add_test(
        NAME "LLVMPass.demanded_check.${TEST_NAME}"
        COMMAND sh -c "out=$(COBRA_DEMANDED_CHECK=1 ${OPT_BIN} -load-pass-plugin=$<TARGET_FILE:CobraPass> -passes=cobra-simplify -disable-output '${FULL_PATH}' 2>&1) || { echo \"$out\"; exit 1; }; if echo \"$out\" | grep COBRA_DEMANDED_CHECK: ; then exit 1; fi"
    )
    set_tests_properties("LLVMPass.demanded_check.${TEST_NAME}" PROPERTIES LABELS "llvm")
endfunction()

foreach(LL_FILE ${LL_TEST_FILES})
    get_filename_component(LL_NAME "${LL_FILE}" NAME)
    cobra_add_demanded_check_test("${LL_NAME}")
endforeach()
