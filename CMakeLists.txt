cmake_minimum_required(VERSION 3.22)

project(LiveRig VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# --- broken Command Line Tools libc++ workaround ---------------------------
# Some machines carry a half-removed /Library/Developer/CommandLineTools/usr/
# include/c++/v1 from an older toolchain. It shadows the SDK's libc++, so even
# <algorithm> stops resolving. If that is the case here, compile against the
# SDK's libc++ only. Reinstalling the Command Line Tools makes this a no-op.
if (APPLE)
    execute_process(COMMAND xcrun --show-sdk-path
                    OUTPUT_VARIABLE RIG_SDK_PATH
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    set(RIG_STALE_LIBCXX "/Library/Developer/CommandLineTools/usr/include/c++/v1")

    if (EXISTS "${RIG_SDK_PATH}/usr/include/c++/v1/algorithm"
        AND EXISTS "${RIG_STALE_LIBCXX}"
        AND NOT EXISTS "${RIG_STALE_LIBCXX}/algorithm")
        set(RIG_LIBCXX_FLAGS -nostdinc++ -isystem "${RIG_SDK_PATH}/usr/include/c++/v1")
        # juceaide is configured and built during this configure step, in its own
        # CMake process, so it has to be reached through the environment.
        set(ENV{CXXFLAGS} "-nostdinc++ -isystem ${RIG_SDK_PATH}/usr/include/c++/v1 $ENV{CXXFLAGS}")
        message(STATUS "LiveRig: working around a shadowed libc++ in the Command Line Tools")
    endif()
endif()

# --- JUCE -------------------------------------------------------------------
# Prefer a local checkout at ./JUCE; fall back to fetching it.
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# --- Neural Amp Modeler core (vendored, MIT) --------------------------------
# The inference engine behind the Amp stage. Compiled quiet: it is upstream
# code, and its warnings are not ours to fix. An OBJECT library on purpose —
# the architectures register themselves through static initialisers, which a
# static archive would dead-strip.
add_library(nam_core OBJECT
    NeuralAmpModelerCore/NAM/activations.cpp
    NeuralAmpModelerCore/NAM/container.cpp
    NeuralAmpModelerCore/NAM/conv1d.cpp
    NeuralAmpModelerCore/NAM/convnet.cpp
    NeuralAmpModelerCore/NAM/dsp.cpp
    NeuralAmpModelerCore/NAM/get_dsp.cpp
    NeuralAmpModelerCore/NAM/linear.cpp
    NeuralAmpModelerCore/NAM/lstm.cpp
    NeuralAmpModelerCore/NAM/nam_file.cpp
    NeuralAmpModelerCore/NAM/ring_buffer.cpp
    NeuralAmpModelerCore/NAM/sequential.cpp
    NeuralAmpModelerCore/NAM/util.cpp
    NeuralAmpModelerCore/NAM/wavenet/a2_fast.cpp
    NeuralAmpModelerCore/NAM/wavenet/model.cpp
    NeuralAmpModelerCore/NAM/wavenet/slimmable.cpp)

target_include_directories(nam_core SYSTEM PUBLIC
    NeuralAmpModelerCore
    NeuralAmpModelerCore/Dependencies/eigen
    NeuralAmpModelerCore/Dependencies/nlohmann)

# Floats end to end: the chain is float, and the official plugin ships this way.
target_compile_definitions(nam_core PUBLIC NAM_SAMPLE_FLOAT)
target_compile_options(nam_core PRIVATE -w)

if (RIG_LIBCXX_FLAGS)
    target_compile_options(nam_core PRIVATE ${RIG_LIBCXX_FLAGS})
endif()

# --- shared DSP / engine sources -------------------------------------------
set(RIG_DSP_SOURCES
    Source/dsp/PitchTracker.cpp
    Source/dsp/OctaveShifter.cpp
    Source/dsp/PlateVerb.cpp
    Source/dsp/AmpModel.cpp
    Source/FxChain.cpp
    Source/StemPlayer.cpp
    Source/FootControl.cpp
    Source/Parameters.cpp)

set(RIG_COMPILE_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_CAMERA=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_REPORT_APP_USAGE=0
    JUCE_VDSP_FRAMEWORK=1)

# --- app --------------------------------------------------------------------
juce_add_gui_app(LiveRig
    PRODUCT_NAME              "LiveRig"
    COMPANY_NAME              "Daniel Fox"
    BUNDLE_ID                 "com.danielfox.liverig"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "LiveRig needs audio input access to process your instrument."
    HARDENED_RUNTIME_ENABLED  FALSE
    ICON_BIG                  "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png")

target_sources(LiveRig PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/AudioEngine.cpp
    Source/SettingsStore.cpp
    Source/SongMaker.cpp
    Source/MidiSender.cpp
    Source/InstrumentHost.cpp
    Source/EffectSlot.cpp
    Source/VocalChannel.cpp
    Source/PresetStore.cpp
    ${RIG_DSP_SOURCES})

target_compile_definitions(LiveRig PRIVATE ${RIG_COMPILE_DEFS})

if (RIG_LIBCXX_FLAGS)
    target_compile_options(LiveRig PRIVATE ${RIG_LIBCXX_FLAGS})
endif()

target_link_libraries(LiveRig PRIVATE
    nam_core
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_gui_basics
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Ad-hoc sign so macOS keeps a stable TCC identity for the microphone prompt,
# then refresh /Applications: the Dock launches that copy, and a build that
# stops in the build tree is a build that gets tested against last night's
# app. A running instance keeps its old files until relaunch; the swap is safe.
add_custom_command(TARGET LiveRig POST_BUILD
    COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:LiveRig>"
    COMMAND rm -rf "/Applications/LiveRig.app"
    COMMAND ditto "$<TARGET_BUNDLE_DIR:LiveRig>" "/Applications/LiveRig.app"
    COMMAND codesign --force --sign - "/Applications/LiveRig.app"
    COMMENT "Signing and refreshing /Applications/LiveRig.app"
    VERBATIM)

# Puts the built app in /Applications and refreshes the Desktop icon.
add_custom_target(install-app
    COMMAND rm -rf "/Applications/LiveRig.app"
    COMMAND cp -R "$<TARGET_BUNDLE_DIR:LiveRig>" "/Applications/LiveRig.app"
    COMMAND codesign --force --sign - "/Applications/LiveRig.app"
    COMMAND rm -f "$ENV{HOME}/Desktop/LiveRig"
    COMMAND ln -s "/Applications/LiveRig.app" "$ENV{HOME}/Desktop/LiveRig"
    DEPENDS LiveRig
    COMMENT "Installing LiveRig.app and the Desktop icon"
    VERBATIM)

# --- offscreen UI snapshots -------------------------------------------------
juce_add_console_app(rig_uishot PRODUCT_NAME "rig_uishot")

target_sources(rig_uishot PRIVATE
    Tests/UiSnapshot.cpp
    Source/MainComponent.cpp
    Source/AudioEngine.cpp
    Source/SettingsStore.cpp
    Source/SongMaker.cpp
    Source/MidiSender.cpp
    Source/InstrumentHost.cpp
    Source/EffectSlot.cpp
    Source/VocalChannel.cpp
    Source/PresetStore.cpp
    ${RIG_DSP_SOURCES})

target_compile_definitions(rig_uishot PRIVATE ${RIG_COMPILE_DEFS} JUCE_STANDALONE_APPLICATION=1)
target_include_directories(rig_uishot PRIVATE Source)

if (RIG_LIBCXX_FLAGS)
    target_compile_options(rig_uishot PRIVATE ${RIG_LIBCXX_FLAGS})
endif()

target_link_libraries(rig_uishot PRIVATE
    nam_core
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_dsp
    juce::juce_gui_basics
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# --- offline DSP validation harness ----------------------------------------
juce_add_console_app(rig_tests PRODUCT_NAME "rig_tests")

target_sources(rig_tests PRIVATE
    Tests/TestMain.cpp
    Source/InstrumentHost.cpp
    Source/EffectSlot.cpp
    Source/VocalChannel.cpp
    Source/PresetStore.cpp
    ${RIG_DSP_SOURCES})

target_compile_definitions(rig_tests PRIVATE ${RIG_COMPILE_DEFS} JUCE_STANDALONE_APPLICATION=1)

if (RIG_LIBCXX_FLAGS)
    target_compile_options(rig_tests PRIVATE ${RIG_LIBCXX_FLAGS})
endif()

target_include_directories(rig_tests PRIVATE Source)

target_link_libraries(rig_tests PRIVATE
    nam_core
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_core
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# Hosting Audio Units drags in AUGenericView, which lives here.
target_link_libraries(rig_tests PRIVATE "-framework CoreAudioKit")
