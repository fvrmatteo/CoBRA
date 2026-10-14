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
# rewrite proved by the solver (`cobra-simplify<verified;boolean>`); one named
# `test_*_unverified_boolean.ll` asks for that reading without the solver, which
# must then change nothing (`cobra-simplify<boolean>`).
#
# A test may also add pass parameters of its own, next to the test itself:
#
#     ; COBRA_PARAMS: families=semilinear;timeout=10000
#
# Two things are worth pinning there. The technique families, because a consumer
# enables its own set (the LLVM-IR pass is configured per project) and with every
# family on one of them may answer the question a test about what the *search*
# cannot do is asking - so the test would pass for a reason the consumer never
# sees. And the proof budget, because a rewrite that needs a proof is otherwise
# accepted or declined by how fast the machine happened to be, which makes the
# test flaky under load rather than wrong.
#
# Parameters are separated by `;`, which CMake reads as a list separator, so the
# pipeline is built with `|` and turned into `$<SEMICOLON>` at the end.
function(cobra_add_ll_test TEST_FILE)
    get_filename_component(TEST_NAME "${TEST_FILE}" NAME_WE)
    set(FULL_NAME "LLVMPass.${TEST_NAME}")
    set(FULL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${TEST_FILE}")
    set(PARAMS "")
    if(TEST_NAME MATCHES "_verified_sweep$")
        if(NOT COBRA_VERIFY_BACKEND)
            return()
        endif()
        set(PARAMS "verified|sweep")
    elseif(TEST_NAME MATCHES "_verified_boolean$")
        if(NOT COBRA_VERIFY_BACKEND)
            return()
        endif()
        set(PARAMS "verified|boolean")
    elseif(TEST_NAME MATCHES "_unverified_boolean$")
        set(PARAMS "boolean")
    elseif(TEST_NAME MATCHES "_verified$")
        if(NOT COBRA_VERIFY_BACKEND)
            return()
        endif()
        set(PARAMS "verified")
    elseif(TEST_NAME MATCHES "_dying_cost$")
        set(PARAMS "dying-cost")
    endif()

    file(STRINGS "${FULL_PATH}" PARAM_LINES REGEX "^; COBRA_PARAMS:")
    if(PARAM_LINES)
        list(GET PARAM_LINES 0 PARAM_LINE)
        string(REGEX REPLACE "^; COBRA_PARAMS:[ \t]*" "" PARAM_TEXT "${PARAM_LINE}")
        string(STRIP "${PARAM_TEXT}" PARAM_TEXT)
        string(REPLACE ";" "|" PARAM_TEXT "${PARAM_TEXT}")
        if(PARAMS)
            set(PARAMS "${PARAMS}|${PARAM_TEXT}")
        else()
            set(PARAMS "${PARAM_TEXT}")
        endif()
    endif()

    set(PIPELINE "cobra-simplify")
    if(PARAMS)
        set(PIPELINE "cobra-simplify<${PARAMS}>")
    endif()
    string(REPLACE "|" "$<SEMICOLON>" PIPELINE "${PIPELINE}")

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
# question answered both ways (`cobra-simplify<demanded-check>`), and a
# disagreement - or the pass failing - fails the test.
function(cobra_add_demanded_check_test TEST_FILE)
    get_filename_component(TEST_NAME "${TEST_FILE}" NAME_WE)
    set(FULL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${TEST_FILE}")
    add_test(
        NAME "LLVMPass.demanded_check.${TEST_NAME}"
        COMMAND sh -c "out=$(${OPT_BIN} -load-pass-plugin=$<TARGET_FILE:CobraPass> -passes='cobra-simplify<demanded-check>' -disable-output '${FULL_PATH}' 2>&1) || { echo \"$out\"; exit 1; }; if echo \"$out\" | grep 'demanded-bits mismatch' ; then exit 1; fi"
    )
    set_tests_properties("LLVMPass.demanded_check.${TEST_NAME}" PROPERTIES LABELS "llvm")
endfunction()

foreach(LL_FILE ${LL_TEST_FILES})
    get_filename_component(LL_NAME "${LL_FILE}" NAME)
    cobra_add_demanded_check_test("${LL_NAME}")
endforeach()

# `cobra-stats` prints the pass's report after the functions it ran on.
add_test(
    NAME "LLVMPass.cobra_stats_reports"
    COMMAND sh -c "${OPT_BIN} -load-pass-plugin=$<TARGET_FILE:CobraPass> -passes='function(cobra-simplify),cobra-stats' -disable-output '${CMAKE_CURRENT_SOURCE_DIR}/test_basic.ll' 2>&1 | grep -q 'candidates solved  *1'"
)
set_tests_properties("LLVMPass.cobra_stats_reports" PROPERTIES LABELS "llvm")

