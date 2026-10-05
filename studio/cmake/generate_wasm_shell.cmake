# Script to configure wasm_shell.html for cdsp-studio
file(READ "${INPUT_FILE}" CONTENT)
string(REPLACE "@APPNAME@" "${APPNAME}" CONTENT "${CONTENT}")
string(REPLACE "@APPEXPORTNAME@" "${APPEXPORTNAME}" CONTENT "${CONTENT}")
string(REPLACE "@PRELOAD@" "" CONTENT "${CONTENT}")
file(WRITE "${OUTPUT_FILE}" "${CONTENT}")
