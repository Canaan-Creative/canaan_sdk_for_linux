# SPDX-License-Identifier: MIT
# Copyright (c) 2026, Canaan Creative Co., Ltd.
#
# For the licensing terms, see the LICENSE file (MIT License).
#
# NO WARRANTY. The Software is provided "AS IS", without warranty of any kind, express or
# implied, including but not limited to the warranties of merchantability, fitness for a
# particular purpose and non-infringement. In no event shall the authors or copyright
# holders be liable for any claim, damages or other liability arising from, out of or in
# connection with the Software or the use or other dealings in the Software.

function(pubkey_header target var pem name decl)
    if(NOT EXISTS "${pem}")
        message(FATAL_ERROR "${var}: ${pem} not found")
    endif()
    file(READ "${pem}" body)
    if(NOT body MATCHES "^-----BEGIN PUBLIC KEY-----\n" OR body MATCHES "[^-A-Za-z0-9+/= \n]")
        message(FATAL_ERROR "${var}: ${pem} is not a PEM public key")
    endif()
    string(REPLACE "\n" "\\n\"\n\"" body "${body}")
    set(dir ${CMAKE_CURRENT_BINARY_DIR}/${name}_${target})
    file(WRITE ${dir}/${name}.h.tmp "${decl} =\n\"${body}\";\n")
    configure_file(${dir}/${name}.h.tmp ${dir}/${name}.h COPYONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pem}")
    target_include_directories(${target} PRIVATE ${dir})
    string(TOUPPER ${name} macro)
    target_compile_definitions(${target} PRIVATE ${macro}_HEADER="${name}.h")
endfunction()

function(aup_pubkey_header target pem)
    pubkey_header(${target} AUP_VERIFY_PUBKEY_PEM "${pem}" aup_pubkey
        "static const char aup_public_pem[]")
endfunction()

function(pic_pubkey_header target pem)
    pubkey_header(${target} PIC_VERIFY_PUBKEY_PEM "${pem}" pic_pubkey
        "const char core_pic_public_pem[]")
endfunction()
