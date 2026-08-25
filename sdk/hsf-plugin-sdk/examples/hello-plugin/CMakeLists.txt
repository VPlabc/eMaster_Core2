# The example plugin. This is also the smoke test for hsf_add_plugin(): if the
# helper is wrong, this stops building.
hsf_add_plugin(
  NAME    hsf.example.hello
  SOURCES src/hello_plugin.cpp)

# hsf_add_plugin derives the target name from the id by replacing dots, so
# hsf.example.hello -> hsf_example_hello. The loader test refers to that.
