if(NOT DEFINED EXECUTABLE OR NOT DEFINED OUTPUT_DIRECTORY
   OR NOT DEFINED WEBAPP_DIST OR NOT DEFINED WORKSPACE_DIRECTORY)
    message(FATAL_ERROR "Local deployment staging arguments are incomplete")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
get_filename_component(executable_name "${EXECUTABLE}" NAME)
configure_file(
    "${EXECUTABLE}"
    "${OUTPUT_DIRECTORY}/${executable_name}"
    COPYONLY)

# Seeded once and never rewritten: this is a file a developer edits to try a
# different port or workspace, and every build would otherwise revert it.
if(NOT EXISTS "${OUTPUT_DIRECTORY}/app.toml")
    file(TO_CMAKE_PATH "${WORKSPACE_DIRECTORY}" workspace_path)
    file(WRITE "${OUTPUT_DIRECTORY}/app.toml"
        "host = \"127.0.0.1\"\n"
        "port = 8888\n"
        "workspace = \"${workspace_path}\"\n")
endif()

if(EXISTS "${WEBAPP_DIST}/index.html")
    file(REMOVE_RECURSE "${OUTPUT_DIRECTORY}/web")
    file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}/web")
    file(COPY "${WEBAPP_DIST}/" DESTINATION "${OUTPUT_DIRECTORY}/web")
endif()
