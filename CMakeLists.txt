cmake_minimum_required(VERSION 3.24)
project(PingOrganizer LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Windows only: this app is built on ConPTY and WebView2.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the C/C++ runtime statically so the exe runs on a clean Windows install
# without the Visual C++ Redistributable. Has to be set before wxWidgets is
# fetched, and wxWidgets has to agree (wxBUILD_USE_STATIC_RUNTIME below).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# WebView2 SDK. The nuget package is a plain zip; the static loader means there
# is no WebView2Loader.dll to ship next to the exe.
# ---------------------------------------------------------------------------
set(WEBVIEW2_VERSION "1.0.4191.47" CACHE STRING "Microsoft.Web.WebView2 version")

if(CMAKE_GENERATOR_PLATFORM MATCHES "^(ARM64|arm64)$")
    set(WEBVIEW2_ARCH arm64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WEBVIEW2_ARCH x64)
else()
    set(WEBVIEW2_ARCH x86)
endif()

FetchContent_Declare(webview2
    URL "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${WEBVIEW2_VERSION}/microsoft.web.webview2.${WEBVIEW2_VERSION}.nupkg"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(webview2)

add_library(WebView2 INTERFACE)
target_include_directories(WebView2 INTERFACE "${webview2_SOURCE_DIR}/build/native/include")
target_link_libraries(WebView2 INTERFACE
    "${webview2_SOURCE_DIR}/build/native/${WEBVIEW2_ARCH}/WebView2LoaderStatic.lib"
    version shlwapi)

# ---------------------------------------------------------------------------
# wxWidgets, built from source as static libs so the result is one exe.
# Everything we do not use is switched off to keep the build short.
# ---------------------------------------------------------------------------
set(wxBUILD_SHARED   OFF CACHE BOOL   "" FORCE)
set(wxBUILD_USE_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(wxBUILD_INSTALL  OFF CACHE BOOL   "" FORCE)
set(wxBUILD_DEMOS    OFF CACHE BOOL   "" FORCE)
set(wxBUILD_SAMPLES  OFF CACHE STRING "" FORCE)
set(wxBUILD_TESTS    OFF CACHE STRING "" FORCE)
foreach(feature WEBVIEW MEDIACTRL GLCANVAS STC RIBBON PROPGRID AUI RICHTEXT)
    set(wxUSE_${feature} OFF CACHE BOOL "" FORCE)
endforeach()

FetchContent_Declare(wxwidgets
    URL "https://github.com/wxWidgets/wxWidgets/releases/download/v3.3.2/wxWidgets-3.3.2.tar.bz2"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(wxwidgets)

# ---------------------------------------------------------------------------
# xterm.js, vendored into assets/ on first configure so later builds are offline.
# ---------------------------------------------------------------------------
set(ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")

function(vendor_asset url name)
    if(EXISTS "${ASSET_DIR}/${name}")
        return()
    endif()
    message(STATUS "Downloading ${name}")
    file(DOWNLOAD "${url}" "${ASSET_DIR}/${name}" STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        file(REMOVE "${ASSET_DIR}/${name}")
        list(GET status 1 reason)
        message(FATAL_ERROR "Could not download ${name}: ${reason}")
    endif()
endfunction()

set(XTERM_VERSION 5.5.0)
set(XTERM_FIT_VERSION 0.10.0)
vendor_asset("https://cdn.jsdelivr.net/npm/@xterm/xterm@${XTERM_VERSION}/lib/xterm.js" xterm.js)
vendor_asset("https://cdn.jsdelivr.net/npm/@xterm/xterm@${XTERM_VERSION}/css/xterm.css" xterm.css)
vendor_asset("https://cdn.jsdelivr.net/npm/@xterm/addon-fit@${XTERM_FIT_VERSION}/lib/addon-fit.js" addon-fit.js)

# ---------------------------------------------------------------------------
# The app.
# ---------------------------------------------------------------------------
add_executable(PingOrganizer WIN32
    src/main.cpp
    src/TerminalPanel.cpp
    src/TerminalPanel.h
    src/ConPty.cpp
    src/ConPty.h
    src/Base64.h
    src/app.rc)

target_compile_definitions(PingOrganizer PRIVATE
    UNICODE _UNICODE NOMINMAX
    _WIN32_WINNT=0x0A00          # ConPTY needs the Windows 10 SDK surface
    WINVER=0x0A00)

target_link_libraries(PingOrganizer PRIVATE wxcore wxbase WebView2
    dwmapi uxtheme)       # title bar and control theming for dark mode

if(MSVC)
    target_compile_options(PingOrganizer PRIVATE /W4 /permissive- /utf-8 /MP)
endif()

add_custom_command(TARGET PingOrganizer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ASSET_DIR}" "$<TARGET_FILE_DIR:PingOrganizer>/assets"
    COMMENT "Staging xterm.js assets next to the executable")
