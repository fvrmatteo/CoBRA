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
# solver backend. One named `test_*_verified_sweep.ll` also wants the word-level
# sweep, which is what makes the proofs of a tree equal to one of its own inputs
# affordable. One named `test_*_dying_cost.ll` is about what a rewrite removes,
# and runs with `cobra-simplify<dying-cost>`. One named
# `test_*_verified_boolean.ll` reads a tree's conditions as variables, with every
# rewrite proved by the solver (`cobra-simplify<verified-boolean>`); one named
# `test_*_unverified_boolean.ll` asks for that reading without the solver, which
# must then change nothing (`cobra-simplify<boolean>`).
#
# A test may also pin the environment the pass runs in, next to the test itself:
#
#     ; COBRA_ENV: COBRA_FAMILIES=semilinear COBRA_TIMEOUT_MS=10000
#
# Two things are worth pinning there. The technique families, because a consumer
# enables its own set (the LLVM-IR pass is configured per project) and with every
# family on one of them may answer the question a test about what the *search*
# cannot do is asking - so the test would pass for a reason the consumer never
# sees. And the proof budget, because a rewrite that needs a proof is otherwise
# accepted or declined by how fast the machine happened to be, which makes the
# test flaky under load rather than wrong.
function(cobra_add_ll_test TEST_FILE)
    get_filename_component(TEST_NAME "${TEST_FILE}" NAME_WE)
    set(FULL_NAME "LLVMPass.${TEST_NAME}")
    set(FULL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${TEST_FILE}")
    set(PIPELINE "cobra-simplify")
    if(TEST_NAME MATCHES "_verified_sweep$")
        if(NOT COBRA_VERIFY_BACKEND)
            return()
        endif()
        set(PIPELINE "cobra-simplify<verified-sweep>")
    elseif(TEST_NAME MATCHES "_verified_boolean$")
        if(NOT COBRA_VERIFY_BACKEND)
            return()
        endif()
        set(PIPELINE "cobra-simplify<verified-boolean>")
    elseif(TEST_NAME MATCHES "_unverified_boolean$")
        set(PIPELINE "cobra-simplify<boolean>")
    elseif(TEST_NAME MATCHES "_verified$")
        if(NOT COBRA_VERIFY_BACKEND)
            return()
        endif()
        set(PIPELINE "cobra-simplify<verified>")
    elseif(TEST_NAME MATCHES "_dying_cost$")
        set(PIPELINE "cobra-simplify<dying-cost>")
    endif()

    file(STRINGS "${FULL_PATH}" ENV_LINES REGEX "^; COBRA_ENV:")
    set(TEST_ENV "")
    if(ENV_LINES)
        list(GET ENV_LINES 0 ENV_LINE)
        string(REGEX REPLACE "^; COBRA_ENV:[ \t]*" "" ENV_TEXT "${ENV_LINE}")
        string(STRIP "${ENV_TEXT}" ENV_TEXT)
        string(REPLACE " " ";" TEST_ENV "${ENV_TEXT}")
    endif()

    add_test(
        NAME "${FULL_NAME}"
        COMMAND sh -c "${OPT_BIN} -load-pass-plugin=$<TARGET_FILE:CobraPass> -passes='${PIPELINE}' -S '${FULL_PATH}' | ${FILECHECK_BIN} '${FULL_PATH}'"
    )
    set_tests_properties("${FULL_NAME}" PROPERTIES LABELS "llvm")
    if(TEST_ENV)
        set_tests_properties("${FULL_NAME}" PROPERTIES ENVIRONMENT "${TEST_ENV}")
    endif()
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
