# Copies the forge's native library next to the forge package (cmake -P, with LIB and DEST defined). A forge running
# meanwhile has the library loaded, and Windows refuses to overwrite a loaded file but lets it be renamed: the one in
# place moves aside (pianoforge_forge_native.<n>.old), the running processes keep it, the next ones load the new one.
get_filename_component(name "${LIB}" NAME)
file(MAKE_DIRECTORY "${DEST}")
set(target "${DEST}/${name}")
file(GLOB stale "${DEST}/*.old")
foreach(old IN LISTS stale)
    file(REMOVE "${old}") # fails quietly while still loaded: removed by a later build
endforeach()
if(EXISTS "${target}")
    string(TIMESTAMP stamp "%Y%m%d%H%M%S")
    file(RENAME "${target}" "${target}.${stamp}.old" RESULT moved)
endif()
file(COPY_FILE "${LIB}" "${target}")
