# Copies monaco's prebuilt, self-contained workers to stable names next to the bundle.
# Parcel splits monaco's own worker entries into shared bundles that load out of order, so
# static/source/code_editor.ts loads these instead.
# Usage: cmake -DMONACO_DIR=<node_modules/monaco-editor> -DDESTINATION=<bin/monaco> -P copy_monaco_workers.cmake

foreach(WORKER json editor)
    file(GLOB MATCHES "${MONACO_DIR}/min/vs/assets/${WORKER}.worker-*.js")
    list(LENGTH MATCHES MATCH_COUNT)
    if (NOT MATCH_COUNT EQUAL 1)
        message(FATAL_ERROR "Expected exactly one prebuilt monaco ${WORKER} worker in ${MONACO_DIR}/min/vs/assets, found ${MATCH_COUNT}.")
    endif()
    file(COPY_FILE "${MATCHES}" "${DESTINATION}/${WORKER}.worker.js" ONLY_IF_DIFFERENT)
endforeach()
