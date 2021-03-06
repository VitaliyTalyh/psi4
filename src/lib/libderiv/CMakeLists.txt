#
# Figure out which files libint will produce, and compile them
#
math(EXPR LIBDERIV_OPT_AM1 ${LIBINT_OPT_AM}-1) # A.M. level for 1st derivative ERIs
math(EXPR LIBDERIV_OPT_AM2 ${LIBINT_OPT_AM}-2) # A.M. level for 2nd derivative ERIs
math(EXPR LIBDERIV_NEW_AM1 ${LIBDERIV_OPT_AM1}*2)
math(EXPR LIBDERIV_NEW_AM2 ${LIBDERIV_OPT_AM2}*2)
math(EXPR LIBDERIV_NEW_AM12 ${LIBDERIV_OPT_AM2}*2)
configure_file(${CMAKE_SOURCE_DIR}/src/lib/libderiv/libderiv_config.h.in ${CMAKE_BINARY_DIR}/src/lib/libderiv/libderiv_config.h)

include(SetupLibint)
set(LD_SRC "init_libderiv.cc" "d1hrr_order_0000.cc" "d1vrr_order_0000.cc" "d12hrr_order_0000.cc" "d12vrr_order_0000.cc")
build_libderiv_files(LD_SRC1 ${LIBDERIV_OPT_AM1} TRUE)
foreach(f ${LD_SRC1})
    set(LD_SRC ${LD_SRC} "d1${f}")
endforeach()
build_libderiv_files(LD_SRC2 ${LIBDERIV_OPT_AM2} FALSE)
foreach(f ${LD_SRC2})
    set(LD_SRC ${LD_SRC} "d12${f}")
endforeach()
set(ABC A B C D)
set(XYZ X Y Z)
math(EXPR MAXAM ${LIBDERIV_NEW_AM1}+1)
foreach(abcval ${ABC})
    foreach(xyzval ${XYZ})
        foreach(amval RANGE ${MAXAM})
            list(GET AM ${amval} amlabel)
            set(LD_SRC ${LD_SRC} "deriv_build_${abcval}${xyzval}_${amlabel}.cc")
        endforeach()
    endforeach()
endforeach()
set_libderiv_compiler_flags()
add_library(deriv STATIC ${LD_SRC})
set_property(GLOBAL APPEND PROPERTY LIBLIST deriv)

# Build the compiler
set(COMPILER_SRC build_libderiv.c emit_d1hrr_build_macro.c emit_deriv1_managers.c emit_deriv_build_macro.c
                 emit_d1hrr_build.c emit_deriv12_managers.c emit_deriv_build.c mem_man.c)
add_executable(libderiv_compiler ${COMPILER_SRC})
add_dependencies(libderiv_compiler int)

# Generate the libderiv files, after building the compiler, only if libderiv.compiled doesn't exist
add_custom_command(
    COMMAND libderiv_compiler
    COMMAND touch libderiv.compiled
    DEPENDS libderiv_compiler
    OUTPUT libderiv.compiled
    OUTPUT ${LD_SRC}
    COMMENT "Generating libderiv files"
)

add_custom_target(libderiv DEPENDS libderiv.compiled int)
install_list_FILES("${PROJECT_BINARY_DIR}/src/lib/libderiv/libderiv.h" include/psi4/libderiv)
