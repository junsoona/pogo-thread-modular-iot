idf_component_register(
    SRCS "sensirion_gas_index_algorithm.c"
    INCLUDE_DIRS "include"
)

# Vendored, unmodified third-party code (see THIRD_PARTY_NOTICE.md) -- this
# repo's own -Wall -Wextra -Werror policy must never force hand-editing
# Sensirion's own source (that would break the "verbatim" attribution this
# file's provenance depends on). -Wparentheses is the one diagnostic this
# file's style (extra parens around every comparison) actually triggers,
# confirmed against a host-compiler smoke build (tools/test_gas_index_algorithm_smoke.c);
# scoped to just this one source file, not the whole project.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-parentheses)
