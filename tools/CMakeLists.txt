add_subdirectory(ttmlir-opt)
add_subdirectory(ttmlir-lsp-server)
add_subdirectory(ttmlir-translate)
if (TTMLIR_ENABLE_EXPLORER)
  add_subdirectory(explorer)
endif()

if (TTMLIR_ENABLE_PYKERNEL)
  add_subdirectory(pykernel)
endif()
add_subdirectory(ttnn-standalone)
