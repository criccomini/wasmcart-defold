defold_log("platform_html5.cmake:")

if(NOT TARGET_PLATFORM MATCHES "^(wasm-web|wasm_pthread-web)$")
  message(FATAL_ERROR "platform_html5.cmake included for non-web TARGET_PLATFORM: ${TARGET_PLATFORM}")
endif()

if(DEFOLD_WASMCART)
  # sys_wasmcart.cpp provides ResolveMountFileName against the cart's asset
  # imports; this keeps sys.cpp from defining it too.
  # DM_HAS_NO_GETENV: a cart has no environment, and every getenv() call site
  # drags in emscripten's environ_get/environ_sizes_get WASI imports for a
  # lookup that can only return null.
  target_compile_definitions(defold_sdk INTERFACE DM_SYS_CUSTOM_HOST_PATHS DM_PLATFORM_WASMCART DM_HAS_NO_GETENV)

  # setjmp/longjmp must be wasm-native, not emscripten's JS trampolines.
  # dmConfigFile's parser longjmps out of an error, and the default lowering
  # imports _emscripten_throw_longjmp plus invoke_* from JS -- which a cart
  # host does not provide, so the jump never unwinds and the parser spins.
  # Must be set for BOTH compile and link.
  target_compile_options(defold_sdk INTERFACE -sSUPPORT_LONGJMP=wasm)
  target_link_options(defold_sdk INTERFACE -sSUPPORT_LONGJMP=wasm)
endif()

# Common compile-time definitions (mirrors waf_dynamo for web)
target_compile_definitions(defold_sdk INTERFACE
  GL_ES_VERSION_2_0
  GOOGLE_PROTOBUF_NO_RTTI
  __STDC_LIMIT_MACROS
  DDF_EXPOSE_DESCRIPTORS
  DM_NO_SYSTEM_FUNCTION
  JC_TEST_NO_DEATH_TEST
  PTHREADS_DEBUG
  DM_HOSTFS=\"/node_vfs/\"
  DM_TEST_DLIB_HTTPCLIENT_NO_HOST_SERVER)

# Common compile options
target_compile_options(defold_sdk INTERFACE
  -Wall
  -fPIC
  -fno-exceptions
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
  -Wno-nontrivial-memcall
  -sDISABLE_EXCEPTION_CATCHING=1)

# Threading: wasm_pthread-web uses pthreads
set(_DEFOLD_WITH_PTHREAD OFF)
if(TARGET_PLATFORM STREQUAL "wasm_pthread-web")
  set(_DEFOLD_WITH_PTHREAD ON)
endif()

if(_DEFOLD_WITH_PTHREAD)
  target_compile_options(defold_sdk INTERFACE -pthread)
endif()

target_compile_options(defold_sdk INTERFACE
  "$<$<CONFIG:Debug>:-gseparate-dwarf>"
  "$<$<CONFIG:Debug>:-gsource-map>")
target_link_options(defold_sdk INTERFACE
  "$<$<CONFIG:Debug>:-gseparate-dwarf>"
  "$<$<CONFIG:Debug>:-gsource-map>")

# Link options base
set(_DEFOLD_INITIAL_MEMORY 33554432)
if(WITH_ASAN AND TARGET_PLATFORM MATCHES "^wasm")
  set(_DEFOLD_INITIAL_MEMORY 67108864)
endif()

set(_DEFOLD_EM_LINK_OPTS
  -Wno-warn-absolute-paths
  --emit-symbol-map
  -lidbfs.js
  -sDISABLE_EXCEPTION_CATCHING=1
  -sALLOW_UNIMPLEMENTED_SYSCALLS=0
  -sEXPORTED_RUNTIME_METHODS=["ccall","UTF8ToString","callMain","HEAPU8","stringToNewUTF8"]
  -sINITIAL_MEMORY=${_DEFOLD_INITIAL_MEMORY}
  -sMAX_WEBGL_VERSION=2
  -sGL_SUPPORT_AUTOMATIC_ENABLE_EXTENSIONS=0
  -sSTACK_SIZE=5MB
)

# A cart exports the wasmcart ABI instead of main(), and leaves its host
# imports undefined for the host to supply at instantiation. Both of these
# would break that, so they are only added for a normal web build.
if(NOT DEFOLD_WASMCART)
  list(APPEND _DEFOLD_EM_LINK_OPTS
    -sEXPORTED_FUNCTIONS=_main,_malloc,_free
    -sERROR_ON_UNDEFINED_SYMBOLS=1
    # The JS loader creates the Memory and passes it in. A cart has no loader:
    # the host instantiates the module directly, so the cart owns and exports
    # its memory instead of importing one.
    -sIMPORTED_MEMORY=1)
endif()

# Browser minimum versions
if(_DEFOLD_WITH_PTHREAD)
  list(APPEND _DEFOLD_EM_LINK_OPTS
    -sMIN_FIREFOX_VERSION=79
    -sMIN_SAFARI_VERSION=150000
    -sMIN_CHROME_VERSION=75)
else()
  list(APPEND _DEFOLD_EM_LINK_OPTS
    -sMIN_FIREFOX_VERSION=40
    -sMIN_SAFARI_VERSION=101000
    -sMIN_CHROME_VERSION=45)
endif()

# WebGPU support
if(WITH_WEBGPU)
  list(APPEND _DEFOLD_EM_LINK_OPTS
    -sUSE_WEBGPU
    -sGL_WORKAROUND_SAFARI_GETCONTEXT_BUG=0
    -sASYNCIFY
    -sWASM_BIGINT=1)
  # Note: ASYNCIFY_ADVISE etc. are opt-level dependent in waf; omitted here
endif()

# WASM output configuration
if(WITH_UBSAN)
  list(APPEND _DEFOLD_EM_LINK_OPTS -sASSERTIONS=1)
endif()
list(APPEND _DEFOLD_EM_LINK_OPTS -sWASM=1 -sALLOW_MEMORY_GROWTH=1)

if(_DEFOLD_WITH_PTHREAD)
  list(APPEND _DEFOLD_EM_LINK_OPTS -pthread)
endif()

if(WITH_ASAN AND TARGET_PLATFORM MATCHES "^wasm")
  target_compile_options(defold_sdk INTERFACE
    -fsanitize=address
    -fno-omit-frame-pointer
    -fsanitize-address-use-after-scope)
  target_compile_definitions(defold_sdk INTERFACE DM_SANITIZE_ADDRESS)
  target_link_options(defold_sdk INTERFACE
    -fsanitize=address
    -fno-omit-frame-pointer
    -fsanitize-address-use-after-scope)
endif()

if(WITH_UBSAN AND TARGET_PLATFORM MATCHES "^wasm")
  target_compile_options(defold_sdk INTERFACE -fsanitize=undefined)
  target_compile_definitions(defold_sdk INTERFACE DM_SANITIZE_UNDEFINED)
  target_link_options(defold_sdk INTERFACE -fsanitize=undefined)
endif()

target_link_options(defold_sdk INTERFACE ${_DEFOLD_EM_LINK_OPTS})
