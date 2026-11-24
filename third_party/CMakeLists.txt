set(LIBYAML_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libyaml")
add_library(inkconfigyaml STATIC
  ${LIBYAML_DIR}/src/api.c ${LIBYAML_DIR}/src/reader.c
  ${LIBYAML_DIR}/src/scanner.c ${LIBYAML_DIR}/src/parser.c
  ${LIBYAML_DIR}/src/loader.c ${LIBYAML_DIR}/src/writer.c
  ${LIBYAML_DIR}/src/emitter.c ${LIBYAML_DIR}/src/dumper.c)
target_include_directories(inkconfigyaml PUBLIC "${LIBYAML_DIR}/include")
target_compile_definitions(inkconfigyaml PUBLIC YAML_DECLARE_STATIC PRIVATE
  YAML_VERSION_MAJOR=0 YAML_VERSION_MINOR=2 YAML_VERSION_PATCH=5 YAML_VERSION_STRING="0.2.5")
