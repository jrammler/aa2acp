file(GLOB_RECURSE sources
  "${PROJECT_SOURCE_DIR}/src/*.cpp"
  "${PROJECT_SOURCE_DIR}/include/*.hpp")

foreach(source IN LISTS sources)
  file(READ "${source}" contents)
  string(REGEX MATCH "std::(cout|cerr)[ \t\r\n]*<<" direct_stream_log
         "${contents}")
  if(direct_stream_log)
    file(RELATIVE_PATH relative_source "${PROJECT_SOURCE_DIR}" "${source}")
    message(FATAL_ERROR
      "${relative_source} uses direct stdout/stderr logging. Use aa2acp::bridge::log(LogLevel) instead.")
  endif()
endforeach()
