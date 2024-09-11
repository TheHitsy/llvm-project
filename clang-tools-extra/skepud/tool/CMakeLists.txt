add_clang_tool(skepud
  ClangdMain.cpp
  Check.cpp
  $<TARGET_OBJECTS:obj.clangDaemonTweaks>
  )

set(LLVM_LINK_COMPONENTS
  support
  )

set(CLANGD_XPC_LIBS "")
if(CLANGD_BUILD_XPC)
  list(APPEND CLANGD_XPC_LIBS "clangdXpcJsonConversions" "clangdXpcTransport")
endif()

clang_target_link_libraries(skepud
  PRIVATE
  clangAST
  clangBasic
  clangFormat
  clangFrontend
  clangLex
  clangSema
  clangTooling
  clangToolingCore
  clangToolingRefactoring
  clangToolingSyntax
  )

target_link_libraries(skepud
  PRIVATE
  clangTidy

  clangDaemon
  clangdRemoteIndex
  clangdSupport
  ${CLANGD_XPC_LIBS}
  )
