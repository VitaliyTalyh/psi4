set(headers_list "")
# List of headers
list(APPEND headers_list )

# If you want to remove some headers specify them explictly here
if(DEVELOPMENT_CODE)
   list(REMOVE_ITEM headers_list "")
else()
   list(REMOVE_ITEM headers_list "")
endif()
# Sort alphabetically
list(SORT headers_list)

set(sources_list "")
# List of sources
list(APPEND sources_list mints.cc )

# If you want to remove some sources specify them explictly here
if(DEVELOPMENT_CODE)
   list(REMOVE_ITEM sources_list "")
else()
   list(REMOVE_ITEM sources_list "")
endif()


# Build static library
add_library(mints_wrapper STATIC ${sources_list})
# Specify dependencies for the library (if any)
add_dependencies(mints_wrapper mints)
set_property(GLOBAL APPEND PROPERTY PSILIB mints_wrapper)
if(BUILD_CUSTOM_BOOST)
   add_dependencies(mints_wrapper custom_boost)
endif()

# Sets install directory for all the headers in the list
install_list_FILES("${headers_list}" include/psi4/libmints_wrapper)
