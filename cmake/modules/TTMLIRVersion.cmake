# get git commit hash
execute_process(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  OUTPUT_VARIABLE TTMLIR_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Check if tags exist and fetch from remote if they don't
execute_process(
  COMMAND git tag -l "v[0-9]*.[0-9]*"
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  OUTPUT_VARIABLE EXISTING_TAGS
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

if("${EXISTING_TAGS}" STREQUAL "")
  message(STATUS "No version tags found locally. Fetching tags from upstream...")

  # Check if 'upstream' is set
  execute_process(
    COMMAND git remote
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE REMOTES
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )

  # Add 'upstream' if not already present
  if(NOT "${REMOTES}" MATCHES "upstream")
    execute_process(
      COMMAND git remote add upstream https://github.com/tenstorrent/tt-mlir.git
      WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    )
  endif()

  execute_process(
    COMMAND git fetch --tags upstream
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  )
endif()

# get the latest tag from git matching 'v<major>.<minor>' format
# (note: matching a glob(7) pattern, not a regex)
execute_process(
  COMMAND git describe --tags --match v[0-9]*.[0-9]* --abbrev=0
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  OUTPUT_VARIABLE GIT_TAG
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# get the number of commits since the latest tag
execute_process(
  COMMAND git rev-list ${GIT_TAG}..HEAD --count
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  OUTPUT_VARIABLE GIT_COMMITS
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Extract the major and minor version from the tag (assumes tags are in "major.minor" format)
string(REGEX MATCH "^v([0-9]+)\\.([0-9]+)$" GIT_TAG_MATCH ${GIT_TAG})
set(TTMLIR_VERSION_MAJOR ${CMAKE_MATCH_1})
set(TTMLIR_VERSION_MINOR ${CMAKE_MATCH_2})
set(TTMLIR_VERSION_PATCH ${GIT_COMMITS})

message(STATUS "Project commit hash: ${TTMLIR_GIT_HASH}")
message(STATUS "Project version: ${TTMLIR_VERSION_MAJOR}.${TTMLIR_VERSION_MINOR}.${TTMLIR_VERSION_PATCH}")

add_definitions("-DTTMLIR_GIT_HASH=${TTMLIR_GIT_HASH}")
add_definitions("-DTTMLIR_VERSION_MAJOR=${TTMLIR_VERSION_MAJOR}")
add_definitions("-DTTMLIR_VERSION_MINOR=${TTMLIR_VERSION_MINOR}")
add_definitions("-DTTMLIR_VERSION_PATCH=${TTMLIR_VERSION_PATCH}")
