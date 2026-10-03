# Resolve the current build sha and write it into a header, run on EVERY build.
#
# Invoked with `cmake -P` from a custom target rather than inlined into
# CMakeLists.txt, because anything in CMakeLists.txt runs at configure time
# and would be frozen into the generated build files. The whole point of this
# stamp is to describe the bytes that are running, so it has to be re-resolved
# per build.
#
# The header is written with copy_if_different semantics: an unchanged sha
# leaves the file's timestamp alone, so including it does not force a rebuild
# on every single build.
#
# Expects: DEFOLD_HOME, STAMP_HEADER

execute_process(
  COMMAND git -c "safe.directory=${DEFOLD_HOME}" -C "${DEFOLD_HOME}" rev-parse --short HEAD
  OUTPUT_VARIABLE SHA
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT SHA)
  set(SHA "nogit")
endif()

# A dirty tree means the binary contains changes that no commit describes, so
# the sha alone would be a lie. Mark it rather than hide it.
execute_process(
  COMMAND git -c "safe.directory=${DEFOLD_HOME}" -C "${DEFOLD_HOME}" status --porcelain
  OUTPUT_VARIABLE DIRTY
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(DIRTY)
  set(SHA "${SHA}-dirty")
endif()

set(CONTENT "// Generated per build by share/cmake/wasmcart_build_stamp.cmake.\n#define WASMCART_BUILD \"${SHA}\"\n")
set(TMP "${STAMP_HEADER}.tmp")
file(WRITE "${TMP}" "${CONTENT}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${TMP}" "${STAMP_HEADER}")
file(REMOVE "${TMP}")
