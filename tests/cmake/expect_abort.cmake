# Runs TEST_EXECUTABLE and asserts it died abnormally AND printed EXPECT_REGEX.
#
# CTest's PASS_REGULAR_EXPRESSION does not rescue a test that dies from a signal
# — an abort is reported as "Subprocess aborted" no matter what was printed. This
# wrapper inverts that: abnormal termination is the expected outcome, and exiting
# cleanly is the failure. Written in CMake script rather than shell so it stays
# portable when Windows lands (v2).

if(NOT DEFINED TEST_EXECUTABLE OR NOT DEFINED EXPECT_REGEX)
  message(FATAL_ERROR "expect_abort.cmake requires -DTEST_EXECUTABLE and -DEXPECT_REGEX")
endif()

execute_process(
  COMMAND "${TEST_EXECUTABLE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout_text
  ERROR_VARIABLE stderr_text)

set(combined "${stdout_text}${stderr_text}")

if(NOT combined MATCHES "${EXPECT_REGEX}")
  message(FATAL_ERROR "expected output matching '${EXPECT_REGEX}'.\nGot:\n${combined}")
endif()

# A clean exit means the guard never fired — the exact false pass this test exists
# to prevent.
if(result STREQUAL "0")
  message(FATAL_ERROR "expected abnormal termination, but the process exited 0.\n"
                      "The RT_SCOPE guard did not abort.\nOutput:\n${combined}")
endif()

message(STATUS "ok: process terminated abnormally (${result}) with the expected diagnostic")
