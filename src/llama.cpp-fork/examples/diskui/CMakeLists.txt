set(TARGET llama-diskui)

# UI version shown in the title bar; each repo build passes its own release tag
# (-DDISKINFER_UI_VERSION=1.0.1 / 1.1-preview), the default is just a fallback.
if (NOT DEFINED DISKINFER_UI_VERSION)
    set(DISKINFER_UI_VERSION "dev")
endif()

add_executable(${TARGET}
    diskui.cpp
    engine.cpp
    api.cpp
    i18n.cpp)

install(TARGETS ${TARGET} RUNTIME)

target_include_directories(${TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor/cpp-httplib)

target_link_libraries(${TARGET} PRIVATE
    llama
    ${CMAKE_THREAD_LIBS_INIT})

target_compile_features(${TARGET} PRIVATE cxx_std_17)

target_compile_definitions(${TARGET} PRIVATE DISKINFER_UI_VERSION="${DISKINFER_UI_VERSION}")

# ---------------------------------------------------------------- HTTP / JSON
# vendor/cpp-httplib is only added to the build when common is built; use it when
# available and compile the implementation directly otherwise
if (TARGET vendor::cpp-httplib)
    target_link_libraries(${TARGET} PRIVATE vendor::cpp-httplib)
else()
    target_sources(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/vendor/cpp-httplib/httplib.cpp)
endif()

if (TARGET vendor::nlohmann)
    target_link_libraries(${TARGET} PRIVATE vendor::nlohmann)
else()
    target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/vendor)
endif()

# keep the inline parts of httplib.h in sync with the compiled library
target_compile_definitions(${TARGET} PRIVATE
    CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH=1048576
    CPPHTTPLIB_LISTEN_BACKLOG=512
    CPPHTTPLIB_REQUEST_URI_MAX_LENGTH=32768
    CPPHTTPLIB_TCP_NODELAY=1)

if (WIN32 AND NOT MSVC)
    target_link_libraries(${TARGET} PRIVATE ws2_32)
endif()

# ------------------------------------------------------------- MoE fast path
# the MoE statistics live in src/llama-disk.h and are being extended by the
# engine work; probe the header so the TUI still builds while it lands and picks
# the fields up automatically once they are there
include(CheckCXXSourceCompiles)
if (DEFINED DISKUI_HAVE_MOE_STATS)
    unset(DISKUI_HAVE_MOE_STATS CACHE)
endif()
set(CMAKE_REQUIRED_INCLUDES
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/ggml/include)
set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX_FLAGS}")
check_cxx_source_compiles("
#include \"llama-disk.h\"
#include <type_traits>
int main() {
    llama_disk::llama_disk_stats * s = nullptr;
    unsigned long long nodes   = s->moe_nodes;
    unsigned long long experts = s->moe_experts;
    unsigned long long bytes   = s->moe_expert_bytes;
    int                flag    = s->moe;
    (void) nodes; (void) experts; (void) bytes; (void) flag;
    bool has_fn = std::is_same<decltype(llama_disk::moe_enabled()), bool>::value;
    return has_fn ? 0 : 1;
}
" DISKUI_HAVE_MOE_STATS)
if (DISKUI_HAVE_MOE_STATS)
    message(STATUS "diskui: MoE statistics found in llama-disk.h")
    target_compile_definitions(${TARGET} PRIVATE DISKUI_HAVE_MOE_STATS=1)
else()
    message(STATUS "diskui: MoE statistics not in llama-disk.h yet, MoE counters shown as 0")
endif()
