# The browser module: the engine and its room as a standalone WebAssembly module (no imports, no Emscripten
# JavaScript), instantiated by the AudioWorklet (site/worklet.js). Built by emcmake (see web/README.md).
add_executable(pianoforge_web src/exports.cpp)
target_link_libraries(pianoforge_web PRIVATE pianoforge::engine)
set_target_properties(pianoforge_web PROPERTIES OUTPUT_NAME pianoforge SUFFIX .wasm)
target_compile_options(pianoforge_web PRIVATE -O3 -fno-exceptions)
set(pianoforge_exports
    _pf_init _pf_alloc _pf_free _pf_load_profile _pf_error _pf_profile_notes _pf_profile_layers _pf_event _pf_render
    _pf_left _pf_right _pf_all_sound_off _pf_set_volume_db _pf_set_option _pf_set_sympathetic_db _pf_set_tuning
    _pf_set_room _pf_set_velocity_curve _pf_voices _pf_resonators _pf_sustain _pf_sostenuto _pf_last_note
    _pf_last_velocity _pf_last_layer _pf_midi_load _pf_midi_close _pf_midi_notes _pf_midi_tracks
    _pf_midi_drums_left_out _pf_midi_length _pf_midi_play _pf_midi_pause _pf_midi_stop _pf_midi_seek _pf_midi_speed
    _pf_midi_loop _pf_midi_position _pf_midi_playing _pf_midi_sounding)
list(JOIN pianoforge_exports "," pianoforge_exports_list)
# The profile is parsed in the module (about 200 MB once decoded): the memory grows on demand, up to 2 GB.
target_link_options(pianoforge_web PRIVATE
    --no-entry -O3 -sSTANDALONE_WASM -sFILESYSTEM=0 -sMALLOC=dlmalloc -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=320MB -sMAXIMUM_MEMORY=2GB -sSTACK_SIZE=1MB
    "-sEXPORTED_FUNCTIONS=${pianoforge_exports_list}")
# Next to the page.
add_custom_command(TARGET pianoforge_web POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:pianoforge_web> ${CMAKE_CURRENT_SOURCE_DIR}/site/pianoforge.wasm)
