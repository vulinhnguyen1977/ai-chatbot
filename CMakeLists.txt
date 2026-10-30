cmake_minimum_required(VERSION 3.16)
project(mstutor CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT WIN32)
  message(FATAL_ERROR
    "This build targets Windows. Password hashing (CNG), TLS (Schannel) and the "
    "user interface (Win32/RichEdit) all use Windows APIs; see README.md.")
endif()

# --- third-party (vendored, no package manager required) ---------------------
add_library(sqlite3 STATIC third_party/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party)
target_compile_definitions(sqlite3 PUBLIC
  SQLITE_ENABLE_FTS5           # lexical half of hybrid retrieval
  SQLITE_THREADSAFE=1
  SQLITE_DEFAULT_FOREIGN_KEYS=1
  SQLITE_OMIT_LOAD_EXTENSION   # nothing loads extensions; smaller and safer
)

# --- application -------------------------------------------------------------
# WIN32 selects the GUI subsystem, so double-clicking the exe opens the window
# with no console flashing up. The console subcommands still work: main()
# attaches to the parent console when it is given one.
add_executable(mstutor WIN32
  src/main.cpp
  src/util.cpp
  src/crypto.cpp
  src/config.cpp
  src/db.cpp
  src/tls_socket.cpp
  src/mailer.cpp
  src/auth.cpp
  src/llm.cpp
  src/guard.cpp
  src/rag.cpp
  src/chat.cpp
  src/markdown.cpp
  src/prefs.cpp
  src/credential_store.cpp
  src/runtime.cpp
  src/gui.cpp
  src/gui_richtext.cpp
  src/gui_edit.cpp
  src/selftest.cpp
  # Application icon and version metadata. Both toolchains compile .rc files.
  res/mstutor.rc
)

target_include_directories(mstutor PRIVATE src third_party)
target_compile_definitions(mstutor PRIVATE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  UNICODE
  _UNICODE
  _WIN32_WINNT=0x0A00          # Windows 10: per-monitor DPI entry points
  WINVER=0x0A00
)
target_link_libraries(mstutor PRIVATE
  sqlite3
  bcrypt     # CNG: PBKDF2, SHA-256, secure random
  secur32    # SSPI/Schannel: TLS for SMTP
  crypt32    # certificate chain validation
  ws2_32     # Winsock
  comctl32   # control subclassing
  gdi32      # fonts and brushes
  user32
  shell32    # CommandLineToArgvW
  ole32
  advapi32   # registry, for the light/dark theme setting
)

if(MSVC)
  target_compile_options(mstutor PRIVATE /W4 /utf-8 /EHsc)
  target_compile_definitions(mstutor PRIVATE _CRT_SECURE_NO_WARNINGS)
  # wWinMain rather than WinMain, matching -municode on the MinGW side.
  set_target_properties(mstutor PROPERTIES LINK_FLAGS "/ENTRY:wWinMainCRTStartup")
else()
  target_compile_options(mstutor PRIVATE -Wall -Wextra)
  # -municode gives the wWinMain entry point; the static flags mean the shipped
  # exe needs no MinGW runtime DLLs.
  target_link_options(mstutor PRIVATE
    -municode -static -static-libgcc -static-libstdc++)
endif()

# Put the binary at the tree root so config/, runtime/ and data/ resolve
# relative to it exactly as they do in the shipped zip.
set_target_properties(mstutor PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_SOURCE_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_SOURCE_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}"
)

enable_testing()
add_test(NAME selftest COMMAND mstutor selftest)
