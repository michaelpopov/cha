if(NOT DEFINED EXECUTABLE OR NOT DEFINED OUTPUT_DIRECTORY
   OR NOT DEFINED WEBAPP_DIST OR NOT DEFINED IMPORT_SEED_DIRECTORY
   OR NOT DEFINED CONFIG_TEMPLATE)
    message(FATAL_ERROR "Local deployment staging arguments are incomplete")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
get_filename_component(executable_name "${EXECUTABLE}" NAME)
configure_file(
    "${EXECUTABLE}"
    "${OUTPUT_DIRECTORY}/${executable_name}"
    COPYONLY)

# The application directory contains a source tree only as an explicit import
# seed. A live database always belongs outside it.
file(REMOVE "${OUTPUT_DIRECTORY}/app.toml")
configure_file(
    "${CONFIG_TEMPLATE}"
    "${OUTPUT_DIRECTORY}/cha.toml.example"
    COPYONLY)
file(REMOVE_RECURSE "${OUTPUT_DIRECTORY}/import-seed")
file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}/import-seed")
file(COPY "${IMPORT_SEED_DIRECTORY}/"
    DESTINATION "${OUTPUT_DIRECTORY}/import-seed")

if(EXISTS "${WEBAPP_DIST}/index.html")
    file(REMOVE_RECURSE "${OUTPUT_DIRECTORY}/web")
    file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}/web")
    file(COPY "${WEBAPP_DIST}/" DESTINATION "${OUTPUT_DIRECTORY}/web")
endif()
