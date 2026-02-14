# svg_to_hpp.cmake
# CMake module for generating .hpp files from .svg files using xml-to-nui

#[=======================================================================[.rst:
svg_to_hpp
--------

Generate C++ header files from SVG files using the xml-to-nui tool.

.. command:: svg_to_hpp

  ::

    svg_to_hpp(
      NAMES <name1> [<name2> ...]
      SVG_DIR <input-directory>
      HPP_DIR <output-directory>
      [TOOLPATH <path-to-xml-to-nui>]
      [TARGET <target-name>]
      [DEPENDS <dependency> ...]
      [ABBREVIATE]
    )

  Generates .hpp files from .svg files for each name in NAMES.

  Options:
    NAMES       - List of base names (without path or extension)
    SVG_DIR     - Directory containing input .svg files
    HPP_DIR     - Directory for output .hpp files (created if needed)
    TARGET      - Optional target name for grouping (default: svg_to_hpp_generated)
    DEPENDS     - Additional dependencies beyond xml-to-nui
    TOOLPATH    - Optional path to the xml-to-nui tool (default: xml-to-nui from target)
    ABBREVIATE   - Whether to use namespace abbreviations in generated code

  Example::

    svg_to_hpp(
      NAMES icon_home icon_settings icon_user
      SVG_DIR ${CMAKE_SOURCE_DIR}/assets/icons
      HPP_DIR ${CMAKE_BINARY_DIR}/generated/icons
      TARGET my_icons
    )

#]=======================================================================]

function(svg_to_hpp)
    # Parse arguments
    set(options "ABBREVIATE")
    set(oneValueArgs SVG_DIR HPP_DIR TARGET NAMESPACE TOOLPATH)
    set(multiValueArgs NAMES DEPENDS)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # Validate required arguments
    if(NOT ARG_SVG_DIR)
        message(FATAL_ERROR "svg_to_hpp: SVG_DIR argument is required")
    endif()
    if(NOT ARG_HPP_DIR)
        message(FATAL_ERROR "svg_to_hpp: HPP_DIR argument is required")
    endif()
    set(TOOLPATH "${ARG_TOOLPATH}")
    if(NOT ARG_TOOLPATH)
        set(TOOLPATH "$<TARGET_FILE:xml-to-nui>")
    endif()

    # If NAMES is empty, glob for all .svg files in SVG_DIR
    if(NOT ARG_NAMES)
        file(GLOB svg_files "${ARG_SVG_DIR}/*.svg")
        if(NOT svg_files)
            message(WARNING "svg_to_hpp: No SVG files found in ${ARG_SVG_DIR}")
            return()
        endif()

        # Extract base names (remove path and .svg extension)
        foreach(svg_path ${svg_files})
            get_filename_component(base_name ${svg_path} NAME_WE)
            list(APPEND ARG_NAMES ${base_name})
        endforeach()

        message(STATUS "svg_to_hpp: Auto-discovered ${CMAKE_MATCH_COUNT} SVG files: ${ARG_NAMES}")
    endif()

    # Default target name
    if(NOT ARG_TARGET)
        set(ARG_TARGET svg_to_hpp_generated)
    endif()
    if (NOT ARG_NAMESPACE)
        set(ARG_NAMESPACE "GeneratedSvgs")
    endif()
    if (ARG_ABBREVIATE)
        set(ARG_ABBREVIATE ON)
    else()
        set(ARG_ABBREVIATE OFF)
    endif()

    # Track generated files
    set(generated_headers "")

    # Process each SVG file
    foreach(name ${ARG_NAMES})
        set(svg_file "${ARG_SVG_DIR}/${name}.svg")
        set(hpp_file "${ARG_HPP_DIR}/${name}.hpp")

        # Create custom command for this SVG -> HPP conversion
        add_custom_command(
            OUTPUT ${hpp_file}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${ARG_HPP_DIR}
            COMMAND ${ARG_TOOLPATH} -i ${svg_file} -o ${hpp_file} -n ${ARG_NAMESPACE} -f ${name} -a $<IF:$<BOOL:${ARG_ABBREVIATE}>,-u,>
            DEPENDS ${ARG_TOOLPATH} ${svg_file} ${ARG_DEPENDS}
            COMMENT "Generating ${name}.hpp from ${name}.svg"
            VERBATIM
        )

        list(APPEND generated_headers ${hpp_file})
    endforeach()

    # Create a custom target that depends on all generated headers
    add_custom_target(${ARG_TARGET}
        DEPENDS ${generated_headers}
        COMMENT "Generating HPP files from SVG sources"
    )

    # Make the target depend on xml-to-nui to ensure build order
    if(NOT ARG_TOOLPATH)
        add_dependencies(${ARG_TARGET} xml-to-nui)
    endif()

    # Export the list of generated files (useful for dependent targets)
    set(${ARG_TARGET}_GENERATED_HEADERS ${generated_headers} PARENT_SCOPE)

endfunction()
